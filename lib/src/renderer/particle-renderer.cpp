#include "renderer/particle-renderer.h"
#include "renderer/neon-renderer.h"
#include "util/color-utils.h"
#include "util/constants.h"
#include "util/geometry-utils.h"
#include "util/gl-utils.h"
#include "util/log-util.h"
#include "shaders.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    namespace
    {
        /// Whether @p arc spawns anything: non-zero length and intensity. A
        /// dark arc keeps its emitter and its ring block, so an animation
        /// darkening it does not reallocate.
        inline bool IsLit(const Arc &arc)
        {
            return arc.length > 0.001f && arc.intensity > 0.0f;
        }

        /// MOVEMENT's spawn density along an arc: arcPixelLength * 2 *
        /// emissionRate / 100 motes per second, so the trail is equally dense
        /// per px however long the arc grows.
        inline float AlongArcRate(const ParticleArcConfig &p, float arcPixelLength)
        {
            return arcPixelLength * 2.0f * (p.emissionRate / 100.0f);
        }

        /// Birth offset of newborn @p n of @p count this frame. On a slow
        /// device dt (and so count) can be large - a whole frame's worth of
        /// motes would otherwise pop in at once, all at full life, reading as
        /// a pulsing burst instead of a steady trickle. Staggered across the
        /// frame's time slice, they land as if spawned continuously through
        /// it.
        inline float StaggeredAge(int n, int count, float dt)
        {
            return (static_cast<float>(count - 1 - n) / static_cast<float>(count)) * dt;
        }

        /// Perimeter position t (wrapped) drawn from the arc with the
        /// headBias skew: t = 0 is the tail, 1 the head. Returns t; writes the
        /// wrapped perimeter position to @p perimeterPos.
        inline float PickAlongArc(const Arc &arc, const ParticleArcConfig &p, ParticleRng &rng,
                                  float &perimeterPos)
        {
            float t = std::pow(rng.Uniform(), p.headBias);
            perimeterPos = arc.start + t * arc.length;
            perimeterPos -= std::floor(perimeterPos);
            return t;
        }

        /// Unit tangent at perimeter position @p perimeterPos, pointing toward
        /// increasing perimeter parameter, from a point a hair further along -
        /// @p point is the perimeter point at @p perimeterPos. Correct on the
        /// rounded corners too, where direction-from-centre would skew.
        inline glm::vec2 UnitTangent(float perimeterPos, const glm::vec2 &point, const RectGeometry &geo)
        {
            constexpr float kTangentEps = 0.001f;
            float aheadT = perimeterPos + kTangentEps;
            aheadT -= std::floor(aheadT);
            glm::vec2 ahead = GeometryUtils::GetPointOnRectangle(aheadT, geo);
            glm::vec2 tangent = ahead - point;
            float tangentLen = glm::length(tangent);
            return tangentLen > 1e-5f ? tangent / tangentLen : glm::vec2(1.0f, 0.0f);
        }

        /// The edge normal at @p point pointing OUT of the rect: perpendicular
        /// to @p tangent, sign-corrected away from the centre (the origin of
        /// rect-local space).
        inline glm::vec2 OutwardNormal(const glm::vec2 &tangent, const glm::vec2 &point)
        {
            glm::vec2 normal(-tangent.y, tangent.x);
            return glm::dot(normal, point) < 0.0f ? -normal : normal;
        }

        /// Finish one newborn and append it - the tail every spawn function
        /// shares. Drops a mote whose whole life fits inside @p initialAge
        /// (it was born dead), and fills the CPU ring colour only when the
        /// arc takes it this frame (@c ParticleEmitContext::wantFallback).
        inline void PushSpawn(const Config &config, const ParticleArcConfig &p, const ParticleEmitContext &ctx,
                              float perimeterPos, const glm::vec2 &spawnPoint, const glm::vec2 &pos,
                              const glm::vec2 &vel, float lifetime, float size, float initialAge,
                              std::vector<ParticleSpawn> &out)
        {
            if (lifetime - initialAge <= 0.0f)
            {
                return;
            }

            ParticleSpawn s;
            s.pos = pos;
            s.vel = vel;
            s.spawnTime = ctx.simTime - initialAge;
            s.lifetime = lifetime;
            s.size = size;
            s.drag = p.drag;
            s.spawnPoint = spawnPoint;

            // The ring colour is the fallback for a frame with no drawn glow
            // to copy, so it is only worked out then. Sampled at
            // (time - initialAge) - the real moment this mote spawned - so
            // fast-forwarded motes pick up the ring's rotation as of their own
            // spawn instant.
            s.fallback = glm::vec3(0.0f);
            if (ctx.wantFallback)
            {
                s.fallback = glm::vec3(1.0f, 0.7f, 0.3f);
                if (ctx.sortedStops != nullptr && !ctx.sortedStops->empty())
                {
                    float rotatedPos = perimeterPos - (ctx.clockTime - initialAge) * config.neon.hueRotationRate;
                    rotatedPos -= std::floor(rotatedPos);
                    s.fallback = glm::vec3(ColorUtils::SampleRing(rotatedPos, *ctx.sortedStops,
                                                                  config.neon.blendSpace));
                }
            }
            out.push_back(s);
        }

        /// Colour latch layout: one texel per ring slot, slot s at
        /// (s % LATCH_WIDTH, s / LATCH_WIDTH). Each arc's block adds
        /// MAX_PARTICLES_CAP / LATCH_WIDTH rows - 32 - so all eight arcs fit in
        /// 128 x 256, far under the 2048 px GLES 3.0 guarantees per axis.
        constexpr int LATCH_WIDTH = 128;
        static_assert(ParticleArcConfig::MAX_PARTICLES_CAP % LATCH_WIDTH == 0,
                      "each arc must add whole latch rows");

        /// Slots per arc block, as the unsigned type the FIFO counters use.
        constexpr uint64_t BLOCK_SLOTS = static_cast<uint64_t>(ParticleArcConfig::MAX_PARTICLES_CAP);

        /// Floats per queued latch write: slot, rect-local spawn point (x, y),
        /// CPU colour as straight rgba, and the copy-the-neon flag.
        constexpr size_t LATCH_FLOATS = 8;

        /// Newborns the per-frame scratch holds before it has to grow - enough
        /// that the first frames of a layer do not allocate.
        constexpr size_t SCRATCH_RESERVE = 256;

        /// Floor on the simulated frame rate. Caps jumbo steps (window drag, a
        /// paused frame resuming) so motes don't teleport, and keeps the sim
        /// in slower-than-realtime lockstep below this rate rather than
        /// lurching forward in oversized, bursty steps.
        constexpr float MIN_SIMULATED_FPS = 10.0f;
        constexpr float MAX_STEP = 1.0f / MIN_SIMULATED_FPS;
    }

    // =========================================================================
    // ParticleEmitter
    // =========================================================================

    void ParticleEmitter::Reset()
    {
        mSpawnAcc = 0.0f;
        mPrevHead = -1.0f;
        mFlowSign = 1.0f;
    }

    void ParticleEmitter::Emit(const Config &config, const ParticleArcConfig &p, const Arc &arc,
                               const ParticleEmitContext &ctx, std::vector<ParticleSpawn> &out)
    {
        if (p.emissionRate <= 0.0f || p.lifetime <= 0.0f || ctx.dt <= 0.0f)
        {
            return;
        }

        trackFlow(arc);

        const float arcPixelLength = arc.length * ctx.perimeterPx;
        switch (p.mode)
        {
        case ParticleMode::MOVEMENT:
        {
            const int count = takeSpawnCount(AlongArcRate(p, arcPixelLength), p, ctx.dt);
            for (int n = 0; n < count; n++)
            {
                spawnTrailMote(config, p, arc, ctx, StaggeredAge(n, count, ctx.dt), out);
            }
            break;
        }
        case ParticleMode::HEAD_SPARKS:
        {
            // A point source: the arc's length does not add sparks, so the
            // rate is emissionRate itself, not a density along the arc.
            const int count = takeSpawnCount(p.emissionRate, p, ctx.dt);
            for (int n = 0; n < count; n++)
            {
                spawnHeadSpark(config, p, arc, ctx, StaggeredAge(n, count, ctx.dt), out);
            }
            break;
        }
        }
    }

    void ParticleEmitter::trackFlow(const Arc &arc)
    {
        // Track the arc's head (start + length, its leading edge) frame to
        // frame so newly spawned motes follow the way the lit arc is actually
        // sweeping - a back-and-forth modulator, a shrinking arc or a
        // reversed winding would otherwise send them the wrong way. Unwrap
        // the perimeter's 1.0 -> 0.0 seam by taking the shortest signed
        // distance around the loop.
        float head = arc.start + arc.length;
        head -= std::floor(head);
        if (mPrevHead >= 0.0f)
        {
            float headDelta = head - mPrevHead;
            headDelta -= std::round(headDelta);
            // Ignore sub-epsilon jitter (float noise, or the arc genuinely
            // holding still) so the flow direction doesn't flicker frame to
            // frame; keep the last direction observed while actually moving.
            constexpr float kFlowDeadzone = 1e-4f;
            if (std::fabs(headDelta) > kFlowDeadzone)
            {
                mFlowSign = (headDelta > 0.0f) ? 1.0f : -1.0f;
            }
        }
        mPrevHead = head;
    }

    int ParticleEmitter::takeSpawnCount(float rate, const ParticleArcConfig &p, float dt)
    {
        // maxParticles holds as a RATE, not a count: no mote outlives
        // `lifetime`, so an arc spawning at most maxParticles / lifetime per
        // second never has more than maxParticles alive. That bound is what
        // lets the manager size the arc's ring block once and never search it
        // for a free slot - and unlike stopping at a count, it thins the
        // emission evenly instead of in bursts.
        const int maxParticles = std::clamp(p.maxParticles, 0, ParticleArcConfig::MAX_PARTICLES_CAP);
        rate = std::min(rate, static_cast<float>(maxParticles) / p.lifetime);

        mSpawnAcc += rate * dt;
        int count = static_cast<int>(mSpawnAcc);
        mSpawnAcc -= static_cast<float>(count);
        return count;
    }

    // =========================================================================
    // MOVEMENT: motes scattered along the whole lit arc, tracking it.
    // =========================================================================

    void ParticleEmitter::spawnTrailMote(const Config &config, const ParticleArcConfig &p, const Arc &arc,
                                         const ParticleEmitContext &ctx, float initialAge,
                                         std::vector<ParticleSpawn> &out)
    {
        const auto &geo = config.geometry;
        ParticleRng &rng = *ctx.rng;

        // Pick a point along the currently lit arc, skewed toward the head
        // end (start + length) via headBias so the cloud is dense at the
        // head and thins out toward the tail (start).
        float perimeterPos = 0.0f;
        const float t = PickAlongArc(arc, p, rng, perimeterPos);

        glm::vec2 pos = GeometryUtils::GetPointOnRectangle(perimeterPos, geo);
        // Where the mote is born ON the line, before the scatter below moves
        // it off - the point whose neon colour it takes.
        const glm::vec2 spawnPoint = pos;

        // Tangent in the direction the arc is really sweeping (mFlowSign).
        const glm::vec2 tangent = UnitTangent(perimeterPos, pos, geo) * mFlowSign;

        // Scatter the spawn point uniformly over a disc of scatterRadius
        // (sqrt keeps the area density even), then stretch the disc 3x along
        // the line so the cloud reads as a streak rather than a blob.
        float scatterAngle = rng.Uniform() * 6.283185f;
        float scatterRadialDist = std::sqrt(rng.Uniform()) * p.scatterRadius;
        glm::vec2 scatterDir(std::cos(scatterAngle), std::sin(scatterAngle));
        glm::vec2 directionScale(1.0f + 2.0f * std::abs(tangent.x),
                                 1.0f + 2.0f * std::abs(tangent.y));
        glm::vec2 offset = scatterDir * scatterRadialDist * directionScale;
        pos += offset;

        // Slow drift along the flow, plus a pull back across the line that
        // cancels the perpendicular part of the scatter offset, so motes
        // settle onto the line instead of spreading away from it.
        float speedMul = (1.0f + rng.Uniform() * 0.5f) * p.speed;
        glm::vec2 vel = tangent * 1.5f * p.scatterRadius * speedMul * 0.001f;
        glm::vec2 offsetToCenterLine = std::abs(tangent.x) > std::abs(tangent.y)
                                           ? glm::vec2(0.0f, -offset.y)
                                           : glm::vec2(-offset.x, 0.0f);
        vel += offsetToCenterLine * 0.002f * speedMul;

        // Taper size from full at the head (t = 1) down to tailSizeFraction
        // at the tail (t = 0), then randomise it into [0.5, 1.0] of that and
        // the lifetime into [0.1, 1.0] of lifetime, so the trail does not
        // read as a uniform grid of identical dots.
        float sizeRandomness = 0.5f + rng.Uniform() * 0.5f;
        float sizeMul = glm::mix(p.tailSizeFraction, 1.0f, t) * sizeRandomness;
        float lifeRandomness = 0.1f + rng.Uniform() * 0.9f;
        float adjustedLifetime = p.lifetime * lifeRandomness;

        PushSpawn(config, p, ctx, perimeterPos, spawnPoint, pos, vel, adjustedLifetime,
                  p.sizeStart * sizeMul, initialAge, out);
    }

    // =========================================================================
    // HEAD_SPARKS: sparks shot from the arc's leading edge.
    // =========================================================================

    void ParticleEmitter::spawnHeadSpark(const Config &config, const ParticleArcConfig &p, const Arc &arc,
                                         const ParticleEmitContext &ctx, float initialAge,
                                         std::vector<ParticleSpawn> &out)
    {
        const auto &geo = config.geometry;
        ParticleRng &rng = *ctx.rng;

        // The LEADING edge, in the direction the arc is sweeping: the head
        // (start + length) while it advances, the tail (start) while it runs
        // backward - sparks fly off whichever end is moving into new ground.
        float edgePos = mFlowSign > 0.0f ? arc.start + arc.length : arc.start;
        edgePos -= std::floor(edgePos);
        const glm::vec2 point = GeometryUtils::GetPointOnRectangle(edgePos, geo);
        const glm::vec2 tangent = UnitTangent(edgePos, point, geo) * mFlowSign;
        const glm::vec2 outward = OutwardNormal(tangent, point);

        // Launch direction: between the sweep direction and straight out of
        // the rect, then turned by a random angle inside the spread cone.
        glm::vec2 dir = glm::mix(tangent, outward, glm::clamp(p.outwardBias, 0.0f, 1.0f));
        const float dirLen = glm::length(dir);
        dir = dirLen > 1e-5f ? dir / dirLen : outward;
        const float halfSpread = glm::radians(glm::clamp(p.spreadDeg, 0.0f, 360.0f)) * 0.5f;
        const float turn = rng.Signed() * halfSpread;
        const float cs = std::cos(turn);
        const float sn = std::sin(turn);
        dir = glm::vec2(cs * dir.x - sn * dir.y, sn * dir.x + cs * dir.y);

        // Born in a disc of scatterRadius around the edge, so the sparks do
        // not all leave from a single pixel.
        const float jitterAngle = rng.Uniform() * 6.283185f;
        const float jitterDist = std::sqrt(rng.Uniform()) * p.scatterRadius;
        const glm::vec2 pos = point + glm::vec2(std::cos(jitterAngle), std::sin(jitterAngle)) * jitterDist;

        // speed is a real launch speed here, px/s. Sparks live the upper part
        // of lifetime, [0.3, 1.0] of it, so the spray keeps a visible reach.
        const glm::vec2 vel = dir * p.speed * (0.5f + rng.Uniform() * 0.5f);
        const float lifetime = p.lifetime * (0.3f + rng.Uniform() * 0.7f);
        const float size = p.sizeStart * (0.5f + rng.Uniform() * 0.5f);

        PushSpawn(config, p, ctx, edgePos, point, pos, vel, lifetime, size, initialAge, out);
    }

    // =========================================================================
    // ParticleManager
    // =========================================================================

    bool ParticleManager::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link ParticleManager shaders.");
            return false;
        }
        // The ring and the latch are sized lazily, on the first Render that
        // knows the arc count.
        resetScratch();
        return true;
    }

    void ParticleManager::Update(float, float, const Config &)
    {
        // Spawning is done in Render(), where the neon has already drawn the
        // glow the newborns take their colour from.
    }

    void ParticleManager::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.particle.enable)
        {
            // Off is off: every mote goes and the layer lets go of its memory,
            // so switching it back on starts from nothing.
            releaseAll();
            return;
        }
        mRunning = true;

        // Where this frame's newborns take their colour from. The drawn glow
        // lives in the neon's scaled buffer, which only exists on its scaled
        // path - neon on, Res Scale below 1. The neon is registered before
        // this layer, so its Render has already filled the buffer this frame;
        // the buffer check itself guards the frames it could not (an
        // allocation failure, or a config change the neon has not drawn yet).
        // opaqueOnly keeps the buffer but skips the gather that fills it, so
        // it would hand back a stale glow. Anything else: the CPU ring.
        const std::shared_ptr<const NeonRenderer> neon = mNeonSource.lock();
        const bool neonDrawsToBuffer = config.neon.enable && !config.debug.opaqueOnly &&
                                       config.neon.resolutionScale < 1.0f;
        if (neon && neonDrawsToBuffer && neon->HasOutputBuffer())
        {
            mColorSource = ParticleColorSource::NEON_OUTPUT;
        }
        else
        {
            mColorSource = ParticleColorSource::COLOR_RING;
        }

        // Step the sim clock off the effect clock, so pausing that pauses the
        // motes too. A clock that moved BACKWARD (Clock::Stop resets it to 0)
        // leaves the motes belonging to no sensible point on the new
        // timeline, so they are cleared rather than frozen as a ghost.
        float dt = 0.0f;
        if (mLastTime >= 0.0f)
        {
            if (time < mLastTime)
            {
                clearParticles();
            }
            else
            {
                dt = time - mLastTime;
            }
        }
        mLastTime = time;
        dt = std::min(dt, MAX_STEP);
        mSimTime += dt;

        // Neon arcs or the layer's own. A switch keeps the motes in flight -
        // they are valid motes, just from the other arcs - but every emitter
        // now reads a different arc, so a sweep direction learned on the old
        // one means nothing.
        const auto &pc = config.particle;
        if (pc.useNeonArc != mUseNeonArc)
        {
            mUseNeonArc = pc.useNeonArc;
            for (auto &emitter : mEmitters)
            {
                emitter.Reset();
            }
        }

        // One emitter and one ring block per arc, sized from the arc COUNT -
        // lit or not - so an animation darkening an arc does not reallocate.
        // With no entry to configure them, nothing emits either way.
        const size_t arcCount = pc.useNeonArc ? config.neon.arcs.size() : pc.arcs.size();
        const int emitterCount = pc.arcs.empty()
                                     ? 0
                                     : static_cast<int>(std::min(arcCount,
                                                                 static_cast<size_t>(ParticleConfig::MAX_ARCS_CAP)));
        if (!ensureCapacity(emitterCount) || mCapacity == 0)
        {
            return;
        }

        retireDead();

        // An arc switched off stops outright, as the layer does: its motes go
        // at once and its emitter forgets its carry and sweep, so switching
        // it back on starts it fresh. Its ring block stays - the layout
        // follows the arc COUNT, not which arcs are on. A DARK arc is not
        // switched off: an animation that darkens one leaves its motes to
        // live out their lives, which is what lets a trail fade after it.
        const int lastEntry = static_cast<int>(pc.arcs.size()) - 1;
        for (int k = 0; k < emitterCount; ++k)
        {
            if (!pc.arcs[static_cast<size_t>(std::min(k, lastEntry))].enable)
            {
                RingBlock &block = mBlocks[static_cast<size_t>(k)];
                block.windowStart = block.written;
                mEmitters[static_cast<size_t>(k)].Reset();
            }
        }

        // Spawn. A second Render at the same time (an offscreen capture) has
        // dt 0 and spawns nothing, so it redraws the frame unchanged.
        if (dt > 0.0f)
        {
            // Perimeter of the rounded rect in px (the corner radius clamped
            // to half the shorter side), once for every emitter: it turns an
            // arc's length from a perimeter fraction into pixels.
            const auto &geo = config.geometry;
            float r = std::max(0.0f, std::min(geo.cornerRadius,
                                              std::min(geo.width, geo.height) * 0.5f));
            float perimeter = 2.0f * (geo.width - 2.0f * r) +
                              2.0f * (geo.height - 2.0f * r) +
                              2.0f * PI * r;

            ParticleEmitContext ctx;
            ctx.dt = dt;
            ctx.simTime = mSimTime;
            ctx.clockTime = time;
            ctx.perimeterPx = perimeter;
            ctx.sortedStops = &mSortedRingStops;
            ctx.rng = &mRng;

            // An own arc is filled in here, one emitter at a time. It carries
            // no colour stops, so filling it allocates nothing.
            Arc ownArc;
            for (int k = 0; k < emitterCount; ++k)
            {
                // A neon arc past the end of the list takes the last entry.
                const ParticleArcConfig &p = pc.arcs[static_cast<size_t>(std::min(k, lastEntry))];
                const Arc *arc = &ownArc;
                if (pc.useNeonArc)
                {
                    arc = &config.neon.arcs[static_cast<size_t>(k)];
                }
                else
                {
                    ownArc.start = p.arcStart;
                    ownArc.length = p.arcLength;
                }
                if (!p.enable || !IsLit(*arc))
                {
                    continue;
                }

                ctx.wantFallback = !p.useCustomColor && mColorSource == ParticleColorSource::COLOR_RING;
                mSpawnScratch.clear();
                mEmitters[static_cast<size_t>(k)].Emit(config, p, *arc, ctx, mSpawnScratch);
                commitNewborns(k, p);
            }
        }

        const bool anyInFlight = std::any_of(mBlocks.begin(), mBlocks.end(), [](const RingBlock &block)
                                             { return block.windowStart != block.written; });
        if (!anyInFlight)
        {
            // Nothing possibly alive: nothing to latch either, since a
            // newborn would have opened a window.
            return;
        }

        // --- MVP: same convention as NeonRenderer (rect-centered local coords). ---
        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                    0.0f, static_cast<float>(viewportHeight),
                                    -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

#if !defined(PLATFORM_LINUX) && !defined(PLATFORM_WINDOWS)
        // Desktop GL needs this to let the vertex shader write gl_PointSize -
        // both passes below draw points. GLES 3.0 has it enabled implicitly.
        glEnable(GL_PROGRAM_POINT_SIZE);
#endif

        // Newborns first: the draw below reads their slots.
        if (!mLatchScratch.empty())
        {
            // A latch write replaces the texel; blending would mix in the
            // colour of whichever dead mote held the slot before.
            glDisable(GL_BLEND);
            renderLatchPass(neon.get(), center,
                            glm::vec2(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)));
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uSimTime", mSimTime);
        // Unit 1, never 0: the latch pass reads its neon source on unit 0
        // while rendering INTO this texture, so keeping the latch off unit 0
        // is what keeps it off every unit that program samples.
        mColorLatch.BindTexture(1);
        mShaderProgram.SetUniform("uColorLatch", 1);
        mShaderProgram.SetUniform("uLatchWidth", LATCH_WIDTH);
        drawWindows();
        mShaderProgram.Unuse();
    }

    void ParticleManager::OnConfigChanged(const Config &config)
    {
        // Every knob is consumed inline; the only derived state is the sorted
        // copy of the ring stops, which ColorUtils::SampleRing requires. The
        // ring follows the arc count, and is resized from Render, where a GL
        // context is guaranteed.
        mSortedRingStops = ColorUtils::SortStops(config.neon.colorStops);
    }

    size_t ParticleManager::GetParticleCount() const
    {
        // Each window as (at most) two contiguous runs of its block, so the
        // scan does no modulo per slot.
        const size_t cap = static_cast<size_t>(BLOCK_SLOTS);
        size_t alive = 0;
        for (size_t k = 0; k < mBlocks.size() && mCapacity > 0; ++k)
        {
            const RingBlock &block = mBlocks[k];
            const float *deathTime = mDeathTime.data() + k * cap;
            const size_t count = static_cast<size_t>(block.written - block.windowStart);
            const size_t first = static_cast<size_t>(block.windowStart % BLOCK_SLOTS);
            const size_t run = std::min(count, cap - first);
            for (size_t i = first; i < first + run; ++i)
            {
                alive += deathTime[i] > mSimTime ? 1 : 0;
            }
            for (size_t i = 0; i < count - run; ++i)
            {
                alive += deathTime[i] > mSimTime ? 1 : 0;
            }
        }
        return alive;
    }

    bool ParticleManager::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::PARTICLE_VERT_SRC,
                                       ShaderSource::PARTICLE_FRAG_SRC,
                                       "ParticleManager");
        mLatchShader = ShaderProgram(ShaderSource::PARTICLE_LATCH_VERT_SRC,
                                     ShaderSource::PARTICLE_LATCH_FRAG_SRC,
                                     "ParticleManager.Latch");

        // Ring records, one ParticleRecord (9 floats) per mote.
        constexpr GLsizei stride = static_cast<GLsizei>(sizeof(ParticleRecord));
        static_assert(sizeof(ParticleRecord) == 9 * sizeof(float), "ParticleRecord must stay tightly packed");
        // aPos0 (loc 0) and aVel0 (loc 1): 2 floats each.
        mRingArray.SetAttribPointer(0, 2, GL_FLOAT, stride, 0);
        mRingArray.SetAttribPointer(1, 2, GL_FLOAT, stride, 2 * sizeof(float));
        // aSpawnTime, aLifetime, aSize, aDrag (loc 2..5): 1 float each.
        mRingArray.SetAttribPointer(2, 1, GL_FLOAT, stride, 4 * sizeof(float));
        mRingArray.SetAttribPointer(3, 1, GL_FLOAT, stride, 5 * sizeof(float));
        mRingArray.SetAttribPointer(4, 1, GL_FLOAT, stride, 6 * sizeof(float));
        mRingArray.SetAttribPointer(5, 1, GL_FLOAT, stride, 7 * sizeof(float));
        // aSlot (loc 6): the record's own ring slot, so particle.vert finds
        // its latch texel without gl_VertexID - see ParticleRecord::slot.
        mRingArray.SetAttribPointer(6, 1, GL_FLOAT, stride, 8 * sizeof(float));

        // Latch points: aSlot (loc 0), aSpawnPos (loc 1), aColor (loc 2),
        // aFromNeon (loc 3).
        constexpr GLsizei latchStride = static_cast<GLsizei>(LATCH_FLOATS * sizeof(float));
        mLatchVertexArray.SetAttribPointer(0, 1, GL_FLOAT, latchStride, 0);
        mLatchVertexArray.SetAttribPointer(1, 2, GL_FLOAT, latchStride, 1 * sizeof(float));
        mLatchVertexArray.SetAttribPointer(2, 4, GL_FLOAT, latchStride, 3 * sizeof(float));
        mLatchVertexArray.SetAttribPointer(3, 1, GL_FLOAT, latchStride, 7 * sizeof(float));

        return mShaderProgram.IsValid() && mLatchShader.IsValid();
    }

    bool ParticleManager::ensureCapacity(int emitterCount)
    {
        const int capacity = emitterCount * ParticleArcConfig::MAX_PARTICLES_CAP;
        if (capacity == mCapacity && static_cast<int>(mEmitters.size()) == emitterCount)
        {
            return true;
        }

        // Every mote in flight indexes the old ring and latch, so a new layout
        // starts empty.
        mEmitters.assign(static_cast<size_t>(emitterCount), ParticleEmitter{});
        mBlocks.assign(static_cast<size_t>(emitterCount), RingBlock{});
        mLatchScratch.clear();
        mCapacity = 0;

        if (capacity == 0)
        {
            // No arcs: give the memory back rather than holding a ring nothing
            // can write to.
            mColorLatch.Release();
            mRingArray.SetVertexData(nullptr, 0, GL_DYNAMIC_DRAW);
            mDeathTime.clear();
            mDeathTime.shrink_to_fit();
            return true;
        }

        // RGBA8: it holds copies of the neon's RGBA8 scaled buffer, or a stop
        // colour. GL_NEAREST: it is read with texelFetch, one texel per mote.
        if (!mColorLatch.Resize(LATCH_WIDTH, capacity / LATCH_WIDTH,
                                GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST))
        {
            LOG_E("ParticleManager: failed to allocate the colour latch for %d arcs.", emitterCount);
            return false;
        }
        // Contents undefined, and never read before they are written: only
        // the live windows are drawn, and they only ever hold written slots.
        mRingArray.SetVertexData(nullptr, static_cast<size_t>(capacity) * sizeof(ParticleRecord),
                                 GL_DYNAMIC_DRAW);
        mDeathTime.assign(static_cast<size_t>(capacity), 0.0f);
        mDeathTime.shrink_to_fit();
        mCapacity = capacity;

        LOG_I("ParticleManager: %d arc(s), ring of %d motes - %zu KB GPU ring, %zu KB latch, %zu KB CPU.",
              emitterCount, capacity,
              static_cast<size_t>(capacity) * sizeof(ParticleRecord) / 1024,
              static_cast<size_t>(capacity) * 4 / 1024,
              static_cast<size_t>(capacity) * sizeof(float) / 1024);
        return true;
    }

    void ParticleManager::clearParticles()
    {
        for (RingBlock &block : mBlocks)
        {
            block.windowStart = block.written;
        }
        mLatchScratch.clear();
        for (auto &emitter : mEmitters)
        {
            emitter.Reset();
        }
    }

    void ParticleManager::releaseAll()
    {
        // Called every frame the layer is off, so the work is done once, on
        // the first of them.
        if (!mRunning)
        {
            return;
        }
        mRunning = false;

        // Every mote, emitter and ring block, and with them the ring, the
        // colour latch and the death times: the no-arcs path.
        ensureCapacity(0);
        // The last latch upload, which ensureCapacity leaves alone - a new
        // layout overwrites it with its first newborns anyway.
        mLatchVertexArray.SetVertexData(nullptr, 0, GL_DYNAMIC_DRAW);
        // A busy arc grows the scratch well past its reserve.
        resetScratch();

        // Back to the state of a new layer, so switching it on again starts
        // exactly as the first time did: the same seed, the sim clock at
        // zero, and no previous frame, so the first frame back takes no step.
        mRng = ParticleRng{};
        mSimTime = 0.0f;
        mLastTime = -1.0f;
        mUseNeonArc = true;
        mColorSource = ParticleColorSource::COLOR_RING;

        LOG_I("ParticleManager: switched off - every mote cleared, ring and colour latch released.");
    }

    void ParticleManager::resetScratch()
    {
        // Swapped with empty vectors rather than cleared: clear keeps the
        // capacity, and shrink_to_fit is only a request.
        std::vector<ParticleSpawn>().swap(mSpawnScratch);
        std::vector<ParticleRecord>().swap(mRecordScratch);
        std::vector<float>().swap(mLatchScratch);
        mSpawnScratch.reserve(SCRATCH_RESERVE);
        mRecordScratch.reserve(SCRATCH_RESERVE);
        mLatchScratch.reserve(SCRATCH_RESERVE * LATCH_FLOATS);
    }

    void ParticleManager::retireDead()
    {
        // Motes are written in birth order but die in any order, so a window
        // only advances past a dead PREFIX; a dead mote behind a live older
        // one stays in the window and is culled by the vertex shader until
        // the one in front dies. That keeps this O(deaths), and no window ever
        // outgrows its block, see commitNewborns.
        for (size_t k = 0; k < mBlocks.size(); ++k)
        {
            RingBlock &block = mBlocks[k];
            const float *deathTime = mDeathTime.data() + k * static_cast<size_t>(BLOCK_SLOTS);
            while (block.windowStart < block.written &&
                   deathTime[static_cast<size_t>(block.windowStart % BLOCK_SLOTS)] <= mSimTime)
            {
                ++block.windowStart;
            }
        }
    }

    void ParticleManager::commitNewborns(int arcIndex, const ParticleArcConfig &p)
    {
        RingBlock &block = mBlocks[static_cast<size_t>(arcIndex)];
        const size_t blockStart = static_cast<size_t>(arcIndex) * static_cast<size_t>(BLOCK_SLOTS);
        const uint64_t firstWritten = block.written;

        // What every newborn of this arc latches: its own colour, or the
        // neon's - the drawn glow when there is one, else the ring colour its
        // emitter filled in.
        const float fromNeon =
            (!p.useCustomColor && mColorSource == ParticleColorSource::NEON_OUTPUT) ? 1.0f : 0.0f;

        mRecordScratch.clear();
        for (const ParticleSpawn &s : mSpawnScratch)
        {
            // FIFO: the next slot is free only once everything in the window
            // before it has died. The arc's rate clamp keeps the window inside
            // its block; this catches the one-mote rounding of the spawn carry
            // at the edge of it, and a lifetime shortened while longer-lived
            // motes are still in flight.
            if (block.written - block.windowStart >= BLOCK_SLOTS)
            {
                break;
            }
            const size_t slot = blockStart + static_cast<size_t>(block.written % BLOCK_SLOTS);
            mRecordScratch.push_back({s.pos.x, s.pos.y, s.vel.x, s.vel.y,
                                      s.spawnTime, s.lifetime, s.size, s.drag,
                                      static_cast<float>(slot)});
            mDeathTime[slot] = s.spawnTime + s.lifetime;

            const glm::vec4 color = p.useCustomColor ? p.customColor : glm::vec4(s.fallback, 1.0f);
            mLatchScratch.push_back(static_cast<float>(slot));
            mLatchScratch.push_back(s.spawnPoint.x);
            mLatchScratch.push_back(s.spawnPoint.y);
            mLatchScratch.push_back(color.r);
            mLatchScratch.push_back(color.g);
            mLatchScratch.push_back(color.b);
            mLatchScratch.push_back(color.a);
            mLatchScratch.push_back(fromNeon);
            ++block.written;
        }

        // Newborns are contiguous in the block, so the arc's upload is one
        // glBufferSubData - two where they wrap its end - of 32 bytes a mote.
        const size_t count = mRecordScratch.size();
        if (count == 0)
        {
            return;
        }
        const size_t first = static_cast<size_t>(firstWritten % BLOCK_SLOTS);
        const size_t run = std::min(count, static_cast<size_t>(BLOCK_SLOTS) - first);
        mRingArray.BindBuffer();
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>((blockStart + first) * sizeof(ParticleRecord)),
                        static_cast<GLsizeiptr>(run * sizeof(ParticleRecord)), mRecordScratch.data());
        if (run < count)
        {
            glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(blockStart * sizeof(ParticleRecord)),
                            static_cast<GLsizeiptr>((count - run) * sizeof(ParticleRecord)),
                            mRecordScratch.data() + run);
        }
    }

    void ParticleManager::renderLatchPass(const NeonRenderer *neon, const glm::vec2 &rectCenter,
                                          const glm::vec2 &viewport)
    {
        // The target this renderer was handed - framebuffer AND viewport, both
        // of which the bind below replaces. Not necessarily the window's: an
        // OffscreenCapture hands the renderer a real FBO.
        const RenderTargetState prevTarget = RenderTargetState::Capture();

        // A host scissor box is in the CALLER's window coordinates and means
        // nothing on the latch; left on, it would drop newborn texels and
        // leave those motes wearing a dead mote's colour.
        GLUtils::NoScissorScope noScissor;

        // Binds the latch and sets the viewport to its size. No clear: only
        // the newborns' texels are written, and every other slot has to keep
        // the colour its mote was born with.
        mColorLatch.Bind();

        mLatchShader.Use();
        mLatchShader.SetUniform("uLatchWidth", LATCH_WIDTH);
        mLatchShader.SetUniform("uLatchHeight", mCapacity / LATCH_WIDTH);
        // Each newborn says for itself whether it copies the neon (see
        // commitNewborns); none does unless there is a drawn glow to copy, so
        // the buffer only has to be bound then.
        if (mColorSource == ParticleColorSource::NEON_OUTPUT)
        {
            neon->BindOutputBuffer(0);
            mLatchShader.SetUniform("uRectCenter", rectCenter);
            mLatchShader.SetUniform("uViewport", viewport);
        }
        mLatchShader.SetUniform("uNeonOutput", 0);
        mLatchVertexArray.SetVertexData(mLatchScratch.data(),
                                        mLatchScratch.size() * sizeof(float),
                                        GL_DYNAMIC_DRAW);
        mLatchVertexArray.DrawArrays(GL_POINTS, static_cast<GLint>(mLatchScratch.size() / LATCH_FLOATS));
        mLatchShader.Unuse();

        prevTarget.Restore();
        mLatchScratch.clear();
    }

    void ParticleManager::drawWindows()
    {
        // Each window is drawn from the slot it starts at, so `first` is
        // rarely 0. The shader does not care: every record carries its own
        // slot (aSlot) to find its latch texel, rather than inferring it from
        // gl_VertexID - see ParticleRecord::slot. One bind for every block:
        // the calls below share the vertex array.
        mRingArray.Bind();
        for (size_t k = 0; k < mBlocks.size(); ++k)
        {
            const RingBlock &block = mBlocks[k];
            const uint64_t count = block.written - block.windowStart;
            if (count == 0)
            {
                continue;
            }
            const uint64_t blockStart = static_cast<uint64_t>(k) * BLOCK_SLOTS;
            const uint64_t first = block.windowStart % BLOCK_SLOTS;
            const uint64_t run = std::min(count, BLOCK_SLOTS - first);
            glDrawArrays(GL_POINTS, static_cast<GLint>(blockStart + first), static_cast<GLsizei>(run));
            if (run < count)
            {
                glDrawArrays(GL_POINTS, static_cast<GLint>(blockStart), static_cast<GLsizei>(count - run));
            }
        }
    }

} // namespace EdgeLighting
