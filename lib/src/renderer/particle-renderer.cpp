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
#include <cstdlib>

namespace EdgeLighting
{
    namespace
    {
        // Deterministic xorshift32 RNG so the spark pattern is reproducible
        // across runs without pulling in a heavier PRNG or std::random.
        struct Rng
        {
            uint32_t s = 0x13375eedU;
            uint32_t Next()
            {
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                return s;
            }
            /// Uniform in [0, 1).
            float Uniform() { return static_cast<float>(Next() & 0x00FFFFFFu) / 16777216.0f; }
            /// Uniform in [-1, 1).
            float Signed() { return Uniform() * 2.0f - 1.0f; }
        };
        Rng gRng;

        /// The arc particles emit from: @c NeonConfig::arcs[0], the one the
        /// tracer / wipe animations drive. nullptr when there is none or it
        /// is dark (zero length or intensity) - nothing spawns then.
        inline const Arc *GetEmitArc(const Config &config)
        {
            if (config.neon.arcs.empty())
            {
                return nullptr;
            }
            const Arc &arc = config.neon.arcs[0];
            if (arc.length <= 0.001f || arc.intensity <= 0.0f)
            {
                return nullptr;
            }
            return &arc;
        }

        /// Colour latch layout: one texel per pool slot, slot s at
        /// (s % LATCH_WIDTH, s / LATCH_WIDTH). 128 x 64 holds the pool ceiling
        /// while staying far under the 2048 px GLES 3.0 guarantees per axis.
        constexpr int LATCH_WIDTH = 128;
        constexpr int LATCH_HEIGHT = ParticleConfig::MAX_PARTICLES_CAP / LATCH_WIDTH;
        static_assert(LATCH_WIDTH * LATCH_HEIGHT == ParticleConfig::MAX_PARTICLES_CAP,
                      "the colour latch must hold exactly one texel per pool slot");

        /// Floats per queued latch write: slot, rect-local spawn point (x, y),
        /// fallback rgb.
        constexpr size_t LATCH_FLOATS = 6;
    }

    bool ParticleRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link ParticleRenderer shaders.");
            return false;
        }
        // Sized to the pool CEILING, once, like the neon's emission table:
        // the size is a compile-time constant, so there is nothing for a
        // later call to discover. RGBA8, the format of what it copies - the
        // neon's scaled buffer - and more than a stop colour needs.
        if (!mColorLatch.Resize(LATCH_WIDTH, LATCH_HEIGHT,
                                GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST))
        {
            LOG_E("Failed to allocate the ParticleRenderer colour latch.");
            return false;
        }
        resetSlots();
        // Reserve a bit of scratch so the first frame doesn't allocate.
        mParticles.reserve(1024);
        mVboScratch.reserve(1024 * 5);
        mLatchScratch.reserve(256 * LATCH_FLOATS);
        return true;
    }

    void ParticleRenderer::Update(float, float, const Config &)
    {
        // Age + emit is done in Render() where we also have `time` for dt.
    }

    void ParticleRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.particle.enable)
        {
            // Freeze the pool; if the user disables mid-run we keep the
            // particles idling. Re-enable and they resume ageing normally.
            mLastTime = time;
            return;
        }

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

        // Compute dt from the effect clock so pausing the clock also pauses
        // the sim (the demo pauses by not advancing `time`).
        float dt = 0.0f;
        if (mLastTime >= 0.0f)
        {
            if (time < mLastTime)
            {
                // The clock's time moved backward - e.g. Clock::Stop() resets
                // it to 0 (Pause leaves it unchanged, so this doesn't fire for
                // a plain pause). The existing pool no longer corresponds to
                // any sensible point on the new timeline, so start over
                // instead of leaving a frozen "ghost" of stale particles on
                // screen forever (dt would otherwise just clamp to 0 below
                // and freeze them in place rather than clearing them).
                mParticles.clear();
                resetSlots();
                mTrailSpawnAcc = 0.0f;
                mPrevMovementHead = -1.0f;
                mFlowSign = 1.0f;
            }
            else
            {
                dt = time - mLastTime;
            }
        }
        mLastTime = time;
        // Floor the simulated frame rate at kMinSimulatedFps: this both caps
        // jumbo dt spikes (window drag / a paused frame resuming) so
        // particles don't teleport, and is exactly what keeps the sim
        // stable on low-end hardware sustaining only that rate - below it,
        // the sim runs in slower-than-realtime lockstep with the actual
        // frame rate rather than lurching forward in oversized, bursty steps.
        constexpr float kMinSimulatedFps = 10.0f;
        constexpr float kMaxDt = 1.0f / kMinSimulatedFps;
        dt = std::min(dt, kMaxDt);

        agingStep(dt, config.particle.drag);
        switch (config.particle.mode)
        {
        case ParticleMode::MOVEMENT:
        {
            emitAlongArc(config, dt, time);
            break;
        }
        }
        if (mParticles.empty())
        {
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

        // Newborns first: the draw below reads their slots. Only a frame that
        // spawned has anything to latch, so a second Render at the same time
        // (an offscreen capture) redraws without touching the latch.
        if (!mLatchScratch.empty())
        {
            // A latch write replaces the texel; blending would mix in the
            // colour of whichever dead particle held the slot before.
            glDisable(GL_BLEND);
            renderLatchPass(neon.get(), center,
                            glm::vec2(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)));
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        // Unit 1, never 0: the latch pass reads its neon source on unit 0
        // while rendering INTO this texture, so keeping the latch off unit 0
        // is what keeps it off every unit that program samples.
        mColorLatch.BindTexture(1);
        mShaderProgram.SetUniform("uColorLatch", 1);
        mShaderProgram.SetUniform("uLatchWidth", LATCH_WIDTH);
        uploadAndDraw();
        mShaderProgram.Unuse();
    }

    void ParticleRenderer::OnConfigChanged(const Config &config)
    {
        // Every knob is consumed inline; the only derived state is the sorted
        // copy of the ring stops, which ColorUtils::SampleRing requires.
        mSortedRingStops = ColorUtils::SortStops(config.neon.colorStops);
    }

    bool ParticleRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::PARTICLE_VERT_SRC,
                                       ShaderSource::PARTICLE_FRAG_SRC,
                                       "ParticleRenderer");
        mLatchShader = ShaderProgram(ShaderSource::PARTICLE_LATCH_VERT_SRC,
                                     ShaderSource::PARTICLE_LATCH_FRAG_SRC,
                                     "ParticleRenderer.Latch");

        constexpr GLsizei stride = 5 * sizeof(float);
        // aPos (loc 0): 2 floats.
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, stride, 0);
        // aSlot (loc 1): 1 float after pos.
        mVertexArray.SetAttribPointer(1, 1, GL_FLOAT, stride, 2 * sizeof(float));
        // aLife (loc 2): 1 float after the slot.
        mVertexArray.SetAttribPointer(2, 1, GL_FLOAT, stride, 3 * sizeof(float));
        // aSize (loc 3): 1 float last.
        mVertexArray.SetAttribPointer(3, 1, GL_FLOAT, stride, 4 * sizeof(float));

        // Latch points: aSlot (loc 0), aSpawnPos (loc 1), aFallback (loc 2).
        constexpr GLsizei latchStride = static_cast<GLsizei>(LATCH_FLOATS * sizeof(float));
        mLatchVertexArray.SetAttribPointer(0, 1, GL_FLOAT, latchStride, 0);
        mLatchVertexArray.SetAttribPointer(1, 2, GL_FLOAT, latchStride, 1 * sizeof(float));
        mLatchVertexArray.SetAttribPointer(2, 3, GL_FLOAT, latchStride, 3 * sizeof(float));

        return mShaderProgram.IsValid() && mLatchShader.IsValid();
    }

    void ParticleRenderer::resetSlots()
    {
        // Descending, so pop_back hands out slot 0 first.
        mFreeSlots.resize(ParticleConfig::MAX_PARTICLES_CAP);
        for (int i = 0; i < ParticleConfig::MAX_PARTICLES_CAP; ++i)
        {
            mFreeSlots[i] = ParticleConfig::MAX_PARTICLES_CAP - 1 - i;
        }
        mLatchScratch.clear();
    }

    void ParticleRenderer::agingStep(float dt, float drag)
    {
        if (dt <= 0.0f)
        {
            return;
        }
        float velScale = std::exp(-drag * dt);
        for (auto &p : mParticles)
        {
            p.pos += p.vel * dt;
            p.vel *= velScale;
            p.life -= dt;
            // The slot goes back before the erase below, so a newborn this
            // same frame can reuse it - its latch write lands before the draw.
            if (p.life <= 0.0f)
            {
                mFreeSlots.push_back(p.slot);
            }
        }
        mParticles.erase(
            std::remove_if(mParticles.begin(), mParticles.end(),
                           [](const Particle &p)
                           { return p.life <= 0.0f; }),
            mParticles.end());
    }
    
    // =========================================================================
    // MOVEMENT mode: motes scattered along the whole lit arc, tracking it.
    // =========================================================================

    void ParticleRenderer::emitAlongArc(const Config &config, float dt, float time)
    {
        const auto &p = config.particle;
        const Arc *arc = GetEmitArc(config);
        if (p.emissionRate <= 0.0f || dt <= 0.0f || arc == nullptr)
        {
            return;
        }

        // Track the arc's head (start + length, its leading edge)
        // frame to frame so newly spawned motes drift the same way the lit
        // arc is actually sweeping. Without this, spawnTrailMote's tangent
        // always pointed toward increasing perimeter parameter regardless
        // of which way the arc itself was moving - fine while arcStart only
        // ever increased, but as soon as the effect swept the other way
        // (a back-and-forth modulator, a shrinking arc, reversed winding)
        // the motes drifted against the arc's motion and flew out ahead of
        // it instead of trailing behind like a comet tail.
        // Unwrap the perimeter's 1.0 -> 0.0 seam by taking the shortest
        // signed distance around the loop.
        float head = arc->start + arc->length;
        head -= std::floor(head);
        if (mPrevMovementHead >= 0.0f)
        {
            float headDelta = head - mPrevMovementHead;
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
        mPrevMovementHead = head;

        // Perimeter of the rounded rect in px (the corner radius clamped to
        // half the shorter side), so the lit arc's length can be turned from
        // a perimeter fraction into pixels.
        const auto &geo = config.geometry;
        float r = std::max(0.0f, std::min(geo.cornerRadius,
                                          std::min(geo.width, geo.height) * 0.5f));
        float perimeter = 2.0f * (geo.width - 2.0f * r) +
                          2.0f * (geo.height - 2.0f * r) +
                          2.0f * PI * r;

        float arcPixelLength = arc->length * perimeter;

        // Spawn rate scales with the lit arc's pixel length, so the trail
        // keeps the same density per px however long the arc grows:
        // arcPixelLength * 2 * emissionRate / 100 motes per second.
        mTrailSpawnAcc += arcPixelLength * 2.0f * (p.emissionRate / 100.0f) * dt;
        int toSpawn = static_cast<int>(mTrailSpawnAcc);
        mTrailSpawnAcc -= static_cast<float>(toSpawn);

        // The pool, and the colour latch behind it, stop at the ceiling.
        const int maxParticles = std::min(p.maxParticles, ParticleConfig::MAX_PARTICLES_CAP);
        for (int n = 0; n < toSpawn; n++)
        {
            if (static_cast<int>(mParticles.size()) >= maxParticles)
            {
                return;
            }

            // On a slow/low-fps device dt (and so toSpawn) can be large - a
            // whole frame's worth of motes would otherwise pop in at once,
            // all at full life, reading as a pulsing burst instead of a
            // steady trickle. Stagger each one's initial age across the
            // frame's time slice so they land pre-aged, as if they'd
            // actually spawned continuously through the frame.
            float initialAge = (static_cast<float>(toSpawn - 1 - n) / static_cast<float>(toSpawn)) * dt;
            spawnTrailMote(config, time, initialAge, mFlowSign);
        }
    }

    void ParticleRenderer::spawnTrailMote(const Config &config, float time, float initialAge, float flowSign)
    {
        const auto &p = config.particle;
        const auto &geo = config.geometry;
        const Arc &arc = config.neon.arcs[0]; // emitAlongArc checked it is lit

        // Pick a point along the currently lit arc, skewed toward the head
        // end (start + length) via headBias so the cloud is dense at the
        // head and thins out toward the tail (start) - t = 0 at the tail, 1
        // at the head.
        float t = std::pow(gRng.Uniform(), p.headBias);
        float perimeterPos = arc.start + t * arc.length;
        perimeterPos -= std::floor(perimeterPos);

        glm::vec2 pos = GeometryUtils::GetPointOnRectangle(perimeterPos, geo);
        // Where the mote is born ON the line, before the scatter below moves
        // it off - the point whose neon colour it takes.
        const glm::vec2 spawnPoint = pos;

        // Tangent direction: sample a point a hair further along the arc and
        // use the vector between them. flowSign (tracked in emitAlongArc from
        // the arc's actual head movement) flips it to match whichever way the
        // effect is really sweeping.
        constexpr float kTangentEps = 0.001f;
        float aheadT = perimeterPos + kTangentEps;
        aheadT -= std::floor(aheadT);
        glm::vec2 ahead = GeometryUtils::GetPointOnRectangle(aheadT, geo);
        glm::vec2 tangent = ahead - pos;
        float tangentLen = glm::length(tangent);
        tangent = tangentLen > 1e-5f ? tangent / tangentLen : glm::vec2(1.0f, 0.0f);
        tangent *= flowSign;

        // Scatter the spawn point uniformly over a disc of scatterRadius
        // (sqrt keeps the area density even), then stretch the disc 3x along
        // the line so the cloud reads as a streak rather than a blob.
        float scatterAngle = gRng.Uniform() * 6.283185f;
        float scatterRadialDist = std::sqrt(gRng.Uniform()) * p.scatterRadius;
        glm::vec2 scatterDir(std::cos(scatterAngle), std::sin(scatterAngle));
        glm::vec2 directionScale(1.0f + 2.0f * std::abs(tangent.x),
                                 1.0f + 2.0f * std::abs(tangent.y));
        glm::vec2 offset = scatterDir * scatterRadialDist * directionScale;
        pos += offset;

        // Slow drift along the flow, plus a pull back across the line that
        // cancels the perpendicular part of the scatter offset, so motes
        // settle onto the line instead of spreading away from it.
        float speedMul = (1.0f + gRng.Uniform() * 0.5f) * p.speed;
        glm::vec2 vel = tangent * 1.5f * p.scatterRadius * speedMul * 0.001f;
        glm::vec2 offsetToCenterLine = std::abs(tangent.x) > std::abs(tangent.y)
                                           ? glm::vec2(0.0f, -offset.y)
                                           : glm::vec2(-offset.x, 0.0f);
        vel += offsetToCenterLine * 0.002f * speedMul;

        // Taper size from full at the head (t = 1) down to tailSizeFraction
        // at the tail (t = 0), then randomise it into [0.5, 1.0] of that and
        // the lifetime into [0.1, 1.0] of lifetime, so the trail does not
        // read as a uniform grid of identical dots.
        float sizeRandomness = 0.5f + gRng.Uniform() * 0.5f;
        float sizeMul = glm::mix(p.tailSizeFraction, 1.0f, t) * sizeRandomness;
        float lifeRandomness = 0.1f + gRng.Uniform() * 0.9f;
        float adjustedLifetime = p.lifetime * lifeRandomness;

        // Fast-forward by initialAge so this mote lands already partway
        // through its life/drift, matching where it would be had it spawned
        // mid-frame instead of at the current instant (see emitAlongArc).
        glm::vec2 driftedVel = vel * std::exp(-p.drag * initialAge);

        Particle np;
        np.pos = pos + vel * initialAge;
        np.vel = driftedVel;
        np.life = adjustedLifetime - initialAge;
        np.lifeMax = adjustedLifetime;
        np.sizeStart = p.sizeStart * sizeMul;

        // Only a particle that is actually kept takes a slot. The empty-pool
        // test is defensive: emitAlongArc stops at the cap the pool is sized
        // to, so there is always one free by the time a spawn gets here.
        if (np.life <= 0.0f || mFreeSlots.empty())
        {
            return;
        }
        np.slot = mFreeSlots.back();
        mFreeSlots.pop_back();
        mParticles.push_back(np);

        // Queue the colour for renderLatchPass, which writes it into the slot
        // once; nothing touches the slot again until this particle dies. The
        // neon's drawn colour is read there, on the GPU, at the spawn point.
        //
        // The ring colour is the fallback for a frame with no drawn glow to
        // copy, so it is only worked out then. Sampled at (time - initialAge) -
        // the real moment this mote spawned - so fast-forwarded motes pick up
        // the ring's rotation as of their own spawn instant. (The drawn glow
        // is this frame's, at most dt * hueRotationRate apart.)
        glm::vec3 fallback(0.0f);
        if (mColorSource == ParticleColorSource::COLOR_RING)
        {
            fallback = glm::vec3(1.0f, 0.7f, 0.3f);
            if (!mSortedRingStops.empty())
            {
                float rotatedPos = perimeterPos - (time - initialAge) * config.neon.hueRotationRate;
                rotatedPos -= std::floor(rotatedPos);
                fallback = glm::vec3(ColorUtils::SampleRing(rotatedPos, mSortedRingStops,
                                                            config.neon.blendSpace));
            }
        }
        mLatchScratch.push_back(static_cast<float>(np.slot));
        mLatchScratch.push_back(spawnPoint.x);
        mLatchScratch.push_back(spawnPoint.y);
        mLatchScratch.push_back(fallback.r);
        mLatchScratch.push_back(fallback.g);
        mLatchScratch.push_back(fallback.b);
    }

    void ParticleRenderer::renderLatchPass(const NeonRenderer *neon, const glm::vec2 &rectCenter,
                                           const glm::vec2 &viewport)
    {
        // The target this renderer was handed - framebuffer AND viewport, both
        // of which the bind below replaces. Not necessarily the window's: an
        // OffscreenCapture hands the renderer a real FBO.
        const RenderTargetState prevTarget = RenderTargetState::Capture();

        // A host scissor box is in the CALLER's window coordinates and means
        // nothing on the latch; left on, it would drop newborn texels and
        // leave those particles wearing a dead particle's colour.
        GLUtils::NoScissorScope noScissor;

        // Binds the latch and sets the viewport to LATCH_WIDTH x LATCH_HEIGHT.
        // No clear: only the newborns' texels are written, and every other
        // slot has to keep the colour its particle was born with.
        mColorLatch.Bind();

        mLatchShader.Use();
        mLatchShader.SetUniform("uLatchWidth", LATCH_WIDTH);
        mLatchShader.SetUniform("uLatchHeight", LATCH_HEIGHT);
        const bool fromNeon = mColorSource == ParticleColorSource::NEON_OUTPUT;
        mLatchShader.SetUniform("uFromNeon", fromNeon ? 1 : 0);
        if (fromNeon)
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

    void ParticleRenderer::uploadAndDraw()
    {
        // Pack into interleaved floats: pos.xy, slot, life, size. The colour
        // is not here - particle.vert reads it from the latch by slot.
        mVboScratch.clear();
        mVboScratch.reserve(mParticles.size() * 5);
        for (const auto &p : mParticles)
        {
            float lifeFrac = p.lifeMax > 0.0f ? std::max(0.0f, p.life / p.lifeMax) : 0.0f;
            mVboScratch.push_back(p.pos.x);
            mVboScratch.push_back(p.pos.y);
            mVboScratch.push_back(static_cast<float>(p.slot));
            mVboScratch.push_back(lifeFrac);
            mVboScratch.push_back(p.sizeStart);
        }

        mVertexArray.SetVertexData(mVboScratch.data(),
                                   mVboScratch.size() * sizeof(float),
                                   GL_DYNAMIC_DRAW);
        mVertexArray.DrawArrays(GL_POINTS, static_cast<GLint>(mParticles.size()));
    }

} // namespace EdgeLighting
