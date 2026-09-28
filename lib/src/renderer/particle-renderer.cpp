#include "renderer/particle-renderer.h"
#include "util/color-utils.h"
#include "util/constants.h"
#include "util/geometry-utils.h"
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
    }

    bool ParticleRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link ParticleRenderer shaders.");
            return false;
        }
        // Reserve a bit of scratch so the first frame doesn't allocate.
        mParticles.reserve(1024);
        mVboScratch.reserve(1024 * 7);
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

        // Compute dt from the effect clock so pausing the clock also pauses
        // the sim (the demo pauses by not advancing `time`).
        float dt = 0.0f;
        if (mLastTime >= 0.0f)
        {
            if (time < mLastTime)
            {
                // The clock's time moved backward — e.g. Clock::Stop() resets
                // it to 0 (Pause leaves it unchanged, so this doesn't fire for
                // a plain pause). The existing pool no longer corresponds to
                // any sensible point on the new timeline, so start over
                // instead of leaving a frozen "ghost" of stale particles on
                // screen forever (dt would otherwise just clamp to 0 below
                // and freeze them in place rather than clearing them).
                mParticles.clear();
                mArcSpawnAcc.clear();
                mPrevArcSites.clear();
                mMovementSpawnAcc = 0.0f;
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
        // stable on low-end hardware sustaining only that rate — below it,
        // the sim runs in slower-than-realtime lockstep with the actual
        // frame rate rather than lurching forward in oversized, bursty steps.
        constexpr float kMinSimulatedFps = 10.0f;
        constexpr float kMaxDt = 1.0f / kMinSimulatedFps;
        dt = std::min(dt, kMaxDt);

        agingStep(dt, config.particle.drag);
        switch (config.particle.mode)
        {
        case ParticleMode::ARC_HEAD_TAIL:
        {
            emitFromArcs(config, dt, time);
            break;
        }
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

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

#if !defined(PLATFORM_LINUX) && !defined(PLATFORM_WINDOWS)
        // Desktop GL needs this to let the vertex shader write gl_PointSize.
        // GLES 3.0 has it enabled implicitly.
        glEnable(GL_PROGRAM_POINT_SIZE);
#endif

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        uploadAndDraw();
        mShaderProgram.Unuse();
    }

    void ParticleRenderer::OnConfigChanged(const Config & /*config*/)
    {
        // Nothing that needs a full rebuild — every knob is consumed inline.
    }

    bool ParticleRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::PARTICLE_VERT_SRC,
                                       ShaderSource::PARTICLE_FRAG_SRC,
                                       "ParticleRenderer");

        constexpr GLsizei stride = 7 * sizeof(float);
        // aPos (loc 0): 2 floats.
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, stride, 0);
        // aColor (loc 1): 3 floats after pos.
        mVertexArray.SetAttribPointer(1, 3, GL_FLOAT, stride, 2 * sizeof(float));
        // aLife (loc 2): 1 float after colour.
        mVertexArray.SetAttribPointer(2, 1, GL_FLOAT, stride, 5 * sizeof(float));
        // aSize (loc 3): 1 float last.
        mVertexArray.SetAttribPointer(3, 1, GL_FLOAT, stride, 6 * sizeof(float));

        return mShaderProgram.IsValid();
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
        }
        mParticles.erase(
            std::remove_if(mParticles.begin(), mParticles.end(),
                           [](const Particle &p)
                           { return p.life <= 0.0f; }),
            mParticles.end());
    }

    // =========================================================================
    // ARC_HEAD_TAIL mode: sparks ejected from the arc's endpoints.
    // =========================================================================

    void ParticleRenderer::emitFromArcs(const Config &config, float dt, float time)
    {
        const auto &p = config.particle;
        if (p.emissionRate <= 0.0f || dt <= 0.0f)
        {
            return;
        }

        // Collect emit sites from NeonConfig's arc pair (arcStart/arcLength and
        // the optional arcStart2/arcLength2). Each arc contributes 0/1/2 sites
        // (tail, head).
        auto pushArcSites = [&](float start, float length,
                                std::vector<float> &sites)
        {
            if (length <= 0.001f)
            {
                return;
            }
            if (p.emitFromHead)
            {
                float head = start + length;
                head -= std::floor(head);
                sites.push_back(head);
            }
            if (p.emitFromTail)
            {
                float tail = start - std::floor(start);
                sites.push_back(tail);
            }
        };

        std::vector<float> sites;
        sites.reserve(2);
        pushArcSites(config.neon.arcStart, config.neon.arcLength, sites);
        if (sites.empty())
        {
            mPrevArcSites.clear();
            return;
        }
        if (mArcSpawnAcc.size() != sites.size())
        {
            mArcSpawnAcc.assign(sites.size(), 0.0f);
            mPrevArcSites = sites; // First frame: no previous position to sweep from.
        }

        for (size_t i = 0; i < sites.size(); ++i)
        {
            mArcSpawnAcc[i] += p.emissionRate * dt;
            int toSpawn = static_cast<int>(mArcSpawnAcc[i]);
            mArcSpawnAcc[i] -= static_cast<float>(toSpawn);

            float currPos = sites[i];
            float prevPos = mPrevArcSites[i];
            // Unwrap the perimeter's 1.0 -> 0.0 seam so the sweep below
            // interpolates the short way around instead of across the ring.
            if (currPos < prevPos && (prevPos - currPos) > 0.5f)
            {
                currPos += 1.0f;
            }

            for (int n = 0; n < toSpawn; ++n)
            {
                if (static_cast<int>(mParticles.size()) >= p.maxParticles)
                {
                    mPrevArcSites = sites;
                    return;
                }

                // On a slow/low-fps device dt can be large, so a fast-moving
                // arc endpoint would otherwise jump straight to its new
                // position each frame and spawn every particle there,
                // leaving a visible gap along the path it swept through.
                // Spread this frame's spawns across that swept path instead
                // (t = 0 start of frame -> 1 end of frame), each fast-
                // forwarded by initialAge so it lands where it would have
                // been had it actually spawned mid-frame.
                float t = (toSpawn > 1) ? static_cast<float>(n) / static_cast<float>(toSpawn) : 1.0f;
                float interpPos = prevPos + t * (currPos - prevPos);
                interpPos -= std::floor(interpPos);
                float initialAge = (1.0f - t) * dt;

                spawnAt(config, interpPos, time, initialAge);
            }
        }

        mPrevArcSites = sites;
    }

    void ParticleRenderer::spawnAt(const Config &config, float perimeterPos, float time, float initialAge)
    {
        const auto &p = config.particle;

        // Spawn position in local (rect-centered) coords.
        glm::vec2 pos = GeometryUtils::GetPointOnRectangle(perimeterPos, config.geometry);

        // True local tangent, taken from a nearby point on the perimeter
        // rather than derived from the rect centre. Direction-from-centre
        // is only the true outward normal at the midpoint of a straight
        // edge — everywhere else on an edge, or on a rounded corner, it
        // skews toward the corner instead of staying perpendicular to the
        // boundary (so at outwardBias = 1 sparks would only fire straight
        // out at edge midpoints, and increasingly sideways elsewhere).
        constexpr float kNormalEps = 0.001f;
        float aheadT = perimeterPos + kNormalEps;
        aheadT -= std::floor(aheadT);
        glm::vec2 ahead = GeometryUtils::GetPointOnRectangle(aheadT, config.geometry);
        glm::vec2 tangent = ahead - pos;
        float tangentLen = glm::length(tangent);
        tangent = tangentLen > 1e-5f ? tangent / tangentLen : glm::vec2(1.0f, 0.0f);

        // Outward normal: perpendicular to the tangent, sign-corrected to
        // point away from the rect centre.
        glm::vec2 outward(-tangent.y, tangent.x);
        if (glm::dot(outward, pos) < 0.0f)
        {
            outward = -outward;
        }

        // A random sign so tangential drift goes both ways.
        if (gRng.Uniform() < 0.5f)
        {
            tangent = -tangent;
        }

        glm::vec2 emitDir = glm::normalize(
            glm::mix(tangent, outward, p.outwardBias));

        // Rotate emitDir by a random angle within the spread cone.
        float spreadRad = glm::radians(p.spreadDeg) * 0.5f;
        float ang = gRng.Signed() * spreadRad;
        float cs = std::cos(ang);
        float sn = std::sin(ang);
        glm::vec2 vel(cs * emitDir.x - sn * emitDir.y,
                      sn * emitDir.x + cs * emitDir.y);
        vel *= p.speed * (0.5f + gRng.Uniform() * 0.5f);

        // Colour from the neon gradient at the emit position, matching the
        // same time-rotated colour the neon shader is showing at that point.
        // Without this rotation the sparks colour would stay fixed to the
        // static gradient position, diverging from the filament when
        // hueRotationRate != 0 (e.g. blue sparks on a red ring). Sampled at
        // (time - initialAge) — the real moment this particle spawned —
        // rather than the current frame time, so swept/fast-forwarded
        // particles (initialAge > 0) pick up the gradient's rotation as of
        // their own spawn instant, not a moment up to dt later.
        glm::vec3 col(1.0f, 0.7f, 0.3f);
        if (!config.neon.colorStops.empty())
        {
            float rotatedPos = perimeterPos - (time - initialAge) * config.neon.hueRotationRate;
            rotatedPos -= std::floor(rotatedPos);
            col = ColorUtils::SampleStops(rotatedPos, config.neon.colorStops,
                                          config.neon.blendSpace);
        }

        // Fast-forward the particle by initialAge so it starts already
        // partway along its path, matching where it would be had it spawned
        // mid-frame at interpPos instead of at the current site (see
        // emitFromArcs).
        Particle np;
        np.pos = pos + vel * initialAge;
        np.vel = vel * std::exp(-config.particle.drag * initialAge);
        np.life = p.lifetime - initialAge;
        np.lifeMax = p.lifetime;
        np.color = col;
        np.sizeStart = p.sizeStart;

        if (np.life > 0.0f)
        {
            mParticles.push_back(np);
        }
    }

    // =========================================================================
    // MOVEMENT mode: motes scattered along the whole lit arc, tracking it.
    // =========================================================================

    void ParticleRenderer::emitAlongArc(const Config &config, float dt, float time)
    {
        const auto &p = config.particle;
        if (p.movementEmissionRate <= 0.0f || dt <= 0.0f || config.neon.arcLength <= 0.001f)
        {
            return;
        }

        mMovementSpawnAcc += p.movementEmissionRate * dt;
        int toSpawn = static_cast<int>(mMovementSpawnAcc);
        mMovementSpawnAcc -= static_cast<float>(toSpawn);

        for (int n = 0; n < toSpawn; ++n)
        {
            if (static_cast<int>(mParticles.size()) >= p.maxParticles)
            {
                return;
            }

            // On a slow/low-fps device dt (and so toSpawn) can be large — a
            // whole frame's worth of motes would otherwise pop in at once,
            // all at full life, reading as a pulsing burst instead of a
            // steady trickle. Stagger each one's initial age across the
            // frame's time slice so they land pre-aged, as if they'd
            // actually spawned continuously through the frame.
            float initialAge = (static_cast<float>(toSpawn - 1 - n) / static_cast<float>(toSpawn)) * dt;
            spawnAlongArc(config, time, initialAge);
        }
    }

    void ParticleRenderer::spawnAlongArc(const Config &config, float time, float initialAge)
    {
        const auto &p = config.particle;

        // Pick a point along the currently lit arc, skewed toward the head
        // end (arcStart + arcLength) via headBias so the cloud is dense at
        // the head and thins out toward the tail (arcStart) — t = 0 at the
        // tail, 1 at the head.
        float t = std::pow(gRng.Uniform(), p.headBias);
        float perimeterPos = config.neon.arcStart + t * config.neon.arcLength;
        perimeterPos -= std::floor(perimeterPos);

        glm::vec2 pos = GeometryUtils::GetPointOnRectangle(perimeterPos, config.geometry);

        // Tangent direction: sample a point a hair further along the arc and
        // use the vector between them, so drift is consistently oriented
        // along the arc's increasing-parameter direction (the "flow" of the
        // line) instead of a per-particle coin flip.
        constexpr float kTangentEps = 0.001f;
        float aheadT = perimeterPos + kTangentEps;
        aheadT -= std::floor(aheadT);
        glm::vec2 ahead = GeometryUtils::GetPointOnRectangle(aheadT, config.geometry);
        glm::vec2 tangent = ahead - pos;
        float tangentLen = glm::length(tangent);
        tangent = tangentLen > 1e-5f ? tangent / tangentLen : glm::vec2(1.0f, 0.0f);

        // Outward normal: perpendicular to the tangent above, sign-corrected
        // to point away from the rect centre. Used only to jitter the spawn
        // point off the line, not to drive drift direction. (Deriving this
        // from the tangent — rather than direction-from-centre — matters
        // because direction-from-centre is only the true outward normal at
        // the midpoint of a straight edge; elsewhere, including rounded
        // corners, it skews toward the corner.)
        glm::vec2 outward(-tangent.y, tangent.x);
        if (glm::dot(outward, pos) < 0.0f)
        {
            outward = -outward;
        }
        pos += outward * (gRng.Signed() * p.scatterRadius);

        glm::vec2 vel = tangent * p.movementSpeed * (0.5f + gRng.Uniform() * 0.5f);

        // Colour from the neon gradient at the emit position, matching the
        // same time-rotated colour the neon shader is showing at that point.
        // Without this rotation the motes' colour would stay fixed to the
        // static gradient position, diverging from the filament when
        // hueRotationRate != 0 (e.g. blue motes on a red ring). Sampled at
        // (time - initialAge) — the real moment this mote spawned — rather
        // than the current frame time, so fast-forwarded motes (initialAge
        // > 0) pick up the gradient's rotation as of their own spawn
        // instant, not a moment up to dt later.
        glm::vec3 col(1.0f, 0.7f, 0.3f);
        if (!config.neon.colorStops.empty())
        {
            float rotatedPos = perimeterPos - (time - initialAge) * config.neon.hueRotationRate;
            rotatedPos -= std::floor(rotatedPos);
            col = ColorUtils::SampleStops(rotatedPos, config.neon.colorStops,
                                          config.neon.blendSpace);
        }

        // Taper size from full at the head (t = 1) down to tailSizeFraction
        // at the tail (t = 0), reinforcing the big-head/small-tail read.
        float sizeMul = glm::mix(p.tailSizeFraction, 1.0f, t);

        // Fast-forward by initialAge so this mote lands already partway
        // through its life/drift, matching where it would be had it spawned
        // mid-frame instead of at the current instant (see emitAlongArc).
        glm::vec2 driftedVel = vel * std::exp(-config.particle.drag * initialAge);

        Particle np;
        np.pos = pos + vel * initialAge;
        np.vel = driftedVel;
        np.life = p.movementLifetime - initialAge;
        np.lifeMax = p.movementLifetime;
        np.color = col;
        np.sizeStart = p.movementSizeStart * sizeMul;

        if (np.life > 0.0f)
        {
            mParticles.push_back(np);
        }
    }

    void ParticleRenderer::uploadAndDraw()
    {
        // Pack into interleaved floats: pos.xy, rgb, life, size.
        mVboScratch.clear();
        mVboScratch.reserve(mParticles.size() * 7);
        for (const auto &p : mParticles)
        {
            float lifeFrac = p.lifeMax > 0.0f ? std::max(0.0f, p.life / p.lifeMax) : 0.0f;
            mVboScratch.push_back(p.pos.x);
            mVboScratch.push_back(p.pos.y);
            mVboScratch.push_back(p.color.r);
            mVboScratch.push_back(p.color.g);
            mVboScratch.push_back(p.color.b);
            mVboScratch.push_back(lifeFrac);
            mVboScratch.push_back(p.sizeStart);
        }

        mVertexArray.SetVertexData(mVboScratch.data(),
                                   mVboScratch.size() * sizeof(float),
                                   GL_DYNAMIC_DRAW);
        mVertexArray.DrawArrays(GL_POINTS, static_cast<GLint>(mParticles.size()));
    }

} // namespace EdgeLighting
