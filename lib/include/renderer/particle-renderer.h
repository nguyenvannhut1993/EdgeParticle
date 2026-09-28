#ifndef _EDGE_LIGHTING_PARTICLE_RENDERER_H_
#define _EDGE_LIGHTING_PARTICLE_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/framebuffer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace EdgeLighting
{
    class NeonRenderer;

    /// Where a frame's newborn particles took their colour from.
    typedef enum class ParticleColorSource
    {
        NEON_OUTPUT, ///< The neon's drawn glow (scaled path only): colour AND coverage as drawn.
        COLOR_RING   ///< No drawn glow this frame: the hue-rotated colour-stop ring, from the CPU.
    } ParticleColorSource;

    /// Point-sprite particle system with two selectable emission strategies
    /// (@ref ParticleConfig::mode, see @ref ParticleMode). Emission reads
    /// @c NeonConfig::arcs[0] - the arc the tracer / wipe animations drive -
    /// and spawns nothing while it is missing or dark.
    ///
    /// @see ParticleConfig for tuning knobs. Compositing: rendered in the same
    /// local (rect-centered) coord space as the neon quad with premultiplied
    /// GL_ONE / one-minus-src-alpha blending, so it lays cleanly on top of the
    /// neon output.
    ///
    /// --- Colour -----------------------------------------------------------
    ///
    /// Each particle takes ONE colour, when it spawns, and keeps it until it
    /// dies. The colour lives on the GPU, in @c mColorLatch: one RGBA8 texel
    /// of premultiplied colour + coverage per pool slot, written by
    /// @ref renderLatchPass for the newborns of the frame and read by slot in
    /// particle.vert. Nothing rewrites a slot while its particle lives, which
    /// is what stops the colour following the neon as it moves - reading the
    /// neon from the draw shader would re-sample it every frame.
    ///
    /// The source (@ref ParticleColorSource) is picked per frame:
    ///   - the DRAWN glow of the neon attached with @ref SetNeonSource, the
    ///     pixel under the spawn point - colour and coverage exactly as
    ///     composited, so a particle is a copy of the neon pixel it was born
    ///     on, and is invisible where the neon drew nothing. Scaled path only
    ///     (@c NeonConfig::resolutionScale < 1): at 1.0 the neon draws
    ///     straight to the target and keeps no buffer.
    ///   - otherwise - scale 1.0, no source, or a frame the neon did not
    ///     draw (off, @c DebugConfig::opaqueOnly) - the hue-rotated
    ///     @c NeonConfig::colorStops ring, from the CPU, at full coverage.
    class ParticleRenderer : public BaseRenderer
    {
    public:
        ParticleRenderer() = default;
        virtual ~ParticleRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

        /// Take newborn colours from @p neon's drawn glow, on the frames it
        /// has one - see the class note. Pass an empty pointer to go back to
        /// the CPU ring.
        ///
        /// Register this renderer AFTER @p neon: the latch reads what the neon
        /// left behind, so registered before it, this layer would read the
        /// previous frame's and lag the neon by a frame.
        void SetNeonSource(std::weak_ptr<const NeonRenderer> neon) { mNeonSource = std::move(neon); }

        /// Live particles after the last Render. For debug readouts.
        size_t GetParticleCount() const { return mParticles.size(); }

        /// Where the last Render took newborn colours from. For debug readouts.
        ParticleColorSource GetColorSource() const { return mColorSource; }

    private:
        typedef struct Particle
        {
            glm::vec2 pos;       ///< Rect-local coords.
            glm::vec2 vel;       ///< Pixels / second.
            float     life;      ///< Seconds remaining.
            float     lifeMax;
            int       slot;      ///< Texel in @c mColorLatch holding this particle's colour.
            float     sizeStart; ///< Pixels at spawn.
        } Particle;

        bool setupShaders();
        /// Hand every pool slot back and drop any queued latch writes.
        void resetSlots();
        void agingStep(float dt, float drag);

        // --- MOVEMENT mode: motes scattered along the whole of arcs[0]. ---
        void emitAlongArc(const Config &config, float dt, float time);
        void spawnTrailMote(const Config &config, float time, float initialAge, float flowSign);

        /// Write this frame's newborn colours into their @c mColorLatch slots,
        /// from @c mColorSource: @p neon's output buffer, or the queued CPU
        /// ring colour. @p rectCenter (window px, y up) and @p viewport place
        /// the spawn points on the output buffer. Retargets the framebuffer
        /// and viewport, so it restores both before returning.
        /// @pre Blending disabled - a latch write replaces the texel.
        /// @pre @p neon is non-null when @c mColorSource is NEON_OUTPUT.
        void renderLatchPass(const NeonRenderer *neon, const glm::vec2 &rectCenter,
                             const glm::vec2 &viewport);

        void uploadAndDraw();

    private:
        ShaderProgram mShaderProgram;
        ShaderProgram mLatchShader;
        VertexArray   mVertexArray{"Particles"};
        VertexArray   mLatchVertexArray{"Particles.Latch"};
        /// One RGBA8 texel per pool slot, @c ParticleConfig::MAX_PARTICLES_CAP
        /// of them. Allocated once, in @ref Initialize.
        Framebuffer   mColorLatch{"Particles.ColorLatch"};

        std::vector<Particle> mParticles;
        std::vector<float>    mVboScratch; ///< 5 floats per particle: pos.xy, slot, life, size.
        /// Pool slots no live particle holds. A particle takes one at spawn
        /// and gives it back when it dies.
        std::vector<int>      mFreeSlots;
        /// This frame's newborns, waiting for @ref renderLatchPass: 6 floats
        /// each - slot, spawn point in rect-local px (x, y), CPU ring rgb.
        std::vector<float>    mLatchScratch;

        std::weak_ptr<const NeonRenderer> mNeonSource;
        /// Set at the top of each Render from what the neon left behind this
        /// frame. The CPU ring colour is only computed when it is COLOR_RING.
        ParticleColorSource mColorSource = ParticleColorSource::COLOR_RING;

        /// @c NeonConfig::colorStops sorted ascending, as
        /// @ref ColorUtils::SampleRing requires. Rebuilt in OnConfigChanged.
        std::vector<ColorStop> mSortedRingStops;

        /// MOVEMENT: single fractional spawn accumulator (spawns are scattered
        /// across the whole arc rather than tied to discrete sites). Fed at a
        /// rate proportional to the lit arc's length in px.
        float mTrailSpawnAcc = 0.0f;
        /// MOVEMENT: the arc's head position (start + length) last
        /// frame, wrapped to [0, 1). Used to detect which way the lit arc is
        /// actually sweeping, so newly spawned motes drift the same way
        /// (see spawnTrailMote) instead of a fixed assumed direction.
        /// Negative = not yet seeded.
        float mPrevMovementHead = -1.0f;
        /// MOVEMENT: sign of the arc's last observed sweep direction (+1 or
        /// -1), persisted across frames where the arc is momentarily
        /// stationary so the drift direction doesn't flicker.
        float mFlowSign = 1.0f;

        float mLastTime = -1.0f;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_PARTICLE_RENDERER_H_
