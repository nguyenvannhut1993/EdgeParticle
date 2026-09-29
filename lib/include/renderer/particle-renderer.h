#ifndef _EDGE_LIGHTING_PARTICLE_RENDERER_H_
#define _EDGE_LIGHTING_PARTICLE_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/framebuffer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <vector>

// The particle layer, in two classes:
//   - ParticleEmitter spawns the motes of ONE arc, as that arc's
//     ParticleArcConfig says. Plain spawn state, no GL.
//   - ParticleManager is the renderer: one emitter per arc, and the ring,
//     colour latch and draw they all share.

namespace EdgeLighting
{
    class NeonRenderer;

    /// Deterministic xorshift32 RNG, so the mote pattern is reproducible across
    /// runs without pulling in a heavier PRNG or std::random. One per
    /// @ref ParticleManager, drawn from by its emitters in arc order.
    typedef struct ParticleRng
    {
        uint32_t state = 0x13375eedU;

        uint32_t Next()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }
        /// Uniform in [0, 1).
        float Uniform() { return static_cast<float>(Next() & 0x00FFFFFFu) / 16777216.0f; }
        /// Uniform in [-1, 1).
        float Signed() { return Uniform() * 2.0f - 1.0f; }
    } ParticleRng;

    /// One newborn mote as its emitter produced it: everything the GPU needs to
    /// place it for its whole life, plus what the colour latch needs once.
    typedef struct ParticleSpawn
    {
        glm::vec2 pos;        ///< Position at @c spawnTime, rect-local px (y up), after the scatter.
        glm::vec2 vel;        ///< Velocity at @c spawnTime, px/s.
        float     spawnTime;  ///< Sim time of birth - up to one step in the past, see initialAge.
        float     lifetime;   ///< Seconds from @c spawnTime to death.
        float     size;       ///< Point size at birth, px.
        float     drag;       ///< Exponential drag (1/s) as configured at birth.
        glm::vec2 spawnPoint; ///< The point ON the line it was born from - where its colour is read.
        glm::vec3 fallback;   ///< CPU ring colour; filled only when the context asks for it.
    } ParticleSpawn;

    /// Per-frame inputs, worked out once by the manager. Only
    /// @c wantFallback changes from one arc to the next.
    typedef struct ParticleEmitContext
    {
        float dt = 0.0f;           ///< This step, already clamped.
        float simTime = 0.0f;      ///< Sim clock after this step; spawn times are measured on it.
        float clockTime = 0.0f;    ///< Effect clock - rotates the fallback ring's hue.
        float perimeterPx = 0.0f;  ///< Rounded-rect perimeter in px.
        /// Fill @c ParticleSpawn::fallback: the arc being emitted takes the
        /// CPU ring colour this frame - no drawn glow, no custom colour.
        bool wantFallback = false;
        const std::vector<ColorStop> *sortedStops = nullptr; ///< Ring stops, sorted, for the fallback.
        ParticleRng *rng = nullptr;
    } ParticleEmitContext;

    // =========================================================================
    // ParticleEmitter
    // =========================================================================

    /// Spawns the motes of ONE arc - a neon arc or one of the layer's own, as
    /// the manager resolves it - with that arc's @ref ParticleArcConfig.
    ///
    /// Holds only what has to survive between frames - the fractional spawn
    /// carry and the arc's sweep direction, 12 bytes. No GL objects and no
    /// particles: @ref ParticleManager owns both, for every arc at once, which
    /// is what keeps the cost of an extra arc down to an extra call to
    /// @ref Emit.
    ///
    /// --- Adding a mode -------------------------------------------------------
    ///
    /// Append it to @ref ParticleMode, give it a spawn function here, and a
    /// case in @ref Emit that works out its rate and calls that function once
    /// per newborn. Two rules, because the manager is built on them:
    ///   - motion is a launch velocity under drag, nothing else - the GPU
    ///     evaluates that in closed form and cannot evaluate anything more;
    ///   - no mote may live longer than @c ParticleArcConfig::lifetime: the
    ///     arc's cap and its ring block are both sized on it being the
    ///     maximum.
    class ParticleEmitter
    {
    public:
        /// Append this frame's newborns from @p arc to @p out, spawned as
        /// @p p says. The manager calls it only for an enabled, lit arc.
        void Emit(const Config &config, const ParticleArcConfig &p, const Arc &arc,
                  const ParticleEmitContext &ctx, std::vector<ParticleSpawn> &out);

        /// Forget the carry and the sweep direction, e.g. after the clock
        /// jumps back.
        void Reset();

    private:
        /// Update @c mFlowSign from how far the arc's head moved since the
        /// last frame. Every mode tracks it, so a mode switch finds the
        /// direction already known.
        void trackFlow(const Arc &arc);

        /// How many motes to spawn this frame at @p rate motes per second:
        /// the arc's @c maxParticles clamp, then the fractional carry. Every
        /// mode's rate goes through here.
        int takeSpawnCount(float rate, const ParticleArcConfig &p, float dt);

        // --- One spawn function per ParticleMode, called from Emit ----------

        /// MOVEMENT: a trail mote scattered along the arc, drifting with the
        /// sweep and settling back onto the line.
        void spawnTrailMote(const Config &config, const ParticleArcConfig &p, const Arc &arc,
                            const ParticleEmitContext &ctx, float initialAge,
                            std::vector<ParticleSpawn> &out);
        /// HEAD_SPARKS: a spark shot from the arc's leading edge.
        void spawnHeadSpark(const Config &config, const ParticleArcConfig &p, const Arc &arc,
                            const ParticleEmitContext &ctx, float initialAge,
                            std::vector<ParticleSpawn> &out);

        /// Fractional spawn carry, so rate * dt loses nothing at small dt.
        float mSpawnAcc = 0.0f;
        /// The arc's head (start + length) last frame, wrapped to [0, 1).
        /// Tracks which way the lit arc is sweeping, so newly spawned motes
        /// drift the same way. Negative = not yet seeded.
        float mPrevHead = -1.0f;
        /// Sign of the arc's last observed sweep (+1 or -1), kept while the
        /// arc holds still so the drift direction does not flicker.
        float mFlowSign = 1.0f;
    };

    // =========================================================================
    // ParticleManager
    // =========================================================================

    /// Where a frame's newborn particles take the NEON's colour from. An arc
    /// with @c ParticleArcConfig::useCustomColor ignores it.
    typedef enum class ParticleColorSource
    {
        NEON_OUTPUT, ///< The neon's drawn glow (scaled path only): colour AND coverage as drawn.
        COLOR_RING   ///< No drawn glow this frame: the hue-rotated colour-stop ring, from the CPU.
    } ParticleColorSource;

    /// Point-sprite motes for EVERY lit arc: one @ref ParticleEmitter per
    /// arc - the neon's or the layer's own, per @c ParticleConfig::useNeonArc,
    /// up to @c ParticleConfig::MAX_ARCS_CAP - each spawning with its own
    /// @ref ParticleArcConfig, all drawn by this one renderer.
    ///
    /// @see ParticleConfig for tuning knobs. Compositing: rendered in the same
    /// local (rect-centred) coord space as the neon quad with premultiplied
    /// GL_ONE / one-minus-src-alpha blending, over the neon output.
    ///
    /// --- One renderer, not one per arc -------------------------------------
    ///
    /// An emitter is 12 bytes of spawn state and no GL. Everything that costs
    /// - shaders, buffers, the colour latch, framebuffer switches - lives here
    /// once, shared by every arc: one ring buffer and one latch pass per
    /// frame, whatever the arc count. On a tile-based GPU the latch pass's
    /// framebuffer switch is the expensive part, which is why it is batched
    /// rather than per arc. Per arc there is only an upload of its newborns
    /// and a draw of its window, one call each (two where the window wraps),
    /// on the same program and vertex array with no state change between.
    ///
    /// --- The CPU only ever touches newborns ----------------------------------
    ///
    /// A mote's motion is an initial velocity under exponential drag, which
    /// has a closed form, so each one is written ONCE, at spawn - start
    /// position and velocity, birth time, lifetime, size, drag, 32 bytes - and
    /// particle.vert works out where it is, how big and how faded, from its
    /// age. Per frame the CPU spawns the newborns, uploads just their records
    /// and advances one index per arc past the dead; it never iterates, moves,
    /// sorts or re-uploads the living. Motion is frame-rate independent as a
    /// side effect.
    ///
    /// --- Ring ----------------------------------------------------------------
    ///
    /// One ring buffer, allocated once per arc COUNT (lit or not, so an
    /// animation darkening an arc does not reallocate), split into one block
    /// of @c ParticleArcConfig::MAX_PARTICLES_CAP records per arc. Each block
    /// is a FIFO of its own, written in birth order: its arc clamps its spawn
    /// RATE to maxParticles / lifetime, so the arc's motes that can still be
    /// alive always fit, and a slot is reused only after everything written
    /// before it has died. The live window is [oldest possibly alive, newest]
    /// per block, drawn as-is, with any dead mote inside it culled in the
    /// vertex shader.
    ///
    /// Blocks, not one FIFO for every arc, because arcs may differ in
    /// lifetime: a shared FIFO would wrap onto a long-lived arc's motes while
    /// they were still alive whenever a short-lived arc spawned fast enough.
    ///
    /// --- Off -----------------------------------------------------------------
    ///
    /// Switching the layer off (@c ParticleConfig::enable) stops it outright:
    /// every mote is cleared at once and everything that holds one is given
    /// back (@ref releaseAll), so a disabled layer costs no ring or latch
    /// memory and switching it on again starts from nothing. Switching one
    /// arc off (@c ParticleArcConfig::enable) clears that arc's motes the
    /// same way. Darkening an arc does not - its motes live out their lives,
    /// so a trail fades behind an arc an animation winds down.
    ///
    /// --- Colour ----------------------------------------------------------------
    ///
    /// Each mote takes ONE colour, when it spawns, and keeps it until it dies:
    /// a texel of premultiplied colour + coverage in @c mColorLatch, at the
    /// mote's ring index, written by @ref renderLatchPass for the newborns of
    /// the frame and read back by particle.vert through gl_VertexID. An arc
    /// with @c ParticleArcConfig::useCustomColor latches its own colour. Every
    /// other arc takes the neon's, from the source (@ref ParticleColorSource)
    /// picked per frame:
    ///   - the DRAWN glow of the neon attached with @ref SetNeonSource, the
    ///     pixel under the spawn point - colour and coverage exactly as
    ///     composited, so a mote is a copy of the neon pixel it was born on
    ///     and is invisible where the neon drew nothing. Scaled path only
    ///     (@c NeonConfig::resolutionScale < 1): at 1.0 the neon draws
    ///     straight to the target and keeps no buffer.
    ///   - otherwise the hue-rotated @c NeonConfig::colorStops ring, from the
    ///     CPU, at full coverage.
    class ParticleManager : public BaseRenderer
    {
    public:
        ParticleManager() = default;
        virtual ~ParticleManager() = default;

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

        /// Motes alive after the last Render. Scans the live window, so it is
        /// meant for debug readouts, not for the frame path.
        size_t GetParticleCount() const;

        /// Ring capacity: arcs x @c ParticleArcConfig::MAX_PARTICLES_CAP.
        size_t GetCapacity() const { return static_cast<size_t>(mCapacity); }

        /// Where the last Render took the neon's colour from, for the arcs
        /// without a custom colour. For debug readouts.
        ParticleColorSource GetColorSource() const { return mColorSource; }

    private:
        /// One mote in the GPU ring, written once at spawn: 8 floats, 32 bytes.
        typedef struct ParticleRecord
        {
            float posX, posY;   ///< Position at spawnTime, rect-local px.
            float velX, velY;   ///< Velocity at spawnTime, px/s.
            float spawnTime;    ///< Sim time of birth.
            float lifetime;     ///< Seconds.
            float size;         ///< Point size at birth, px.
            float drag;         ///< Exponential drag, 1/s.
        } ParticleRecord;

        /// One arc's block of the ring: slots [k x MAX_PARTICLES_CAP,
        /// (k + 1) x MAX_PARTICLES_CAP) for arc k, a FIFO of its own.
        typedef struct RingBlock
        {
            uint64_t written = 0;     ///< Motes ever written; slot = block start + written % MAX_PARTICLES_CAP.
            uint64_t windowStart = 0; ///< Oldest mote that may still be alive.
        } RingBlock;

        bool setupShaders();
        /// Size the ring, the latch and the death times for @p emitterCount
        /// arcs. A no-op while the count holds; a change clears every mote,
        /// whose slots index the old layout. @return false if the latch could
        /// not be allocated - nothing is drawn then.
        bool ensureCapacity(int emitterCount);
        /// Empty every live window and reset every emitter. Nothing is erased
        /// on the GPU: entries outside the windows are never drawn or read
        /// again, and the blocks overwrite them in turn.
        void clearParticles();
        /// What switching the layer off does: drop every mote, give back the
        /// ring, the colour latch, the death times, the last latch upload and
        /// the grown scratch, and put the RNG and both clocks back to their
        /// initial values - the layer is left as a new one, so switching it
        /// on again starts exactly as the first time. Only the compiled
        /// shaders stay. A no-op until the layer runs again (@c mRunning).
        void releaseAll();
        /// Swap the per-frame scratch for empty vectors reserved to the size
        /// the first frames need - freeing whatever a busy arc grew it to.
        void resetScratch();
        /// Advance each block's window start past every dead mote at its
        /// front. Amortised O(deaths).
        void retireDead();
        /// Give the newborns in @c mSpawnScratch - all from arc @p arcIndex -
        /// slots in that arc's block, queue their latch writes with the colour
        /// @p p asks for, and upload their records: one glBufferSubData, two
        /// where they wrap the block's end.
        void commitNewborns(int arcIndex, const ParticleArcConfig &p);
        /// Write this frame's newborn colours into their @c mColorLatch slots:
        /// @p neon's output buffer for a newborn queued to copy it, its queued
        /// CPU colour otherwise. @p rectCenter (window px, y up) and
        /// @p viewport place the spawn points on the output buffer. Retargets
        /// the framebuffer and viewport, so it restores both before returning.
        /// @pre Blending disabled - a latch write replaces the texel.
        /// @pre @p neon is non-null when @c mColorSource is NEON_OUTPUT.
        void renderLatchPass(const NeonRenderer *neon, const glm::vec2 &rectCenter,
                             const glm::vec2 &viewport);
        /// Draw every block's live window - one call each, two where a window
        /// wraps its block's end.
        void drawWindows();

    private:
        ShaderProgram mShaderProgram;
        ShaderProgram mLatchShader;
        VertexArray   mRingArray{"Particles.Ring"};   ///< The GPU ring of ParticleRecords.
        VertexArray   mLatchVertexArray{"Particles.Latch"};
        /// One RGBA8 texel per ring slot. Sized with the ring.
        Framebuffer   mColorLatch{"Particles.ColorLatch"};

        std::vector<ParticleEmitter> mEmitters; ///< One per arc, index = arc index.
        std::vector<RingBlock>       mBlocks;   ///< One per arc, index = arc index.
        ParticleRng mRng;
        /// @c ParticleConfig::useNeonArc as of the last Render. A switch hands
        /// every emitter a different arc, so their sweep directions reset.
        bool mUseNeonArc = true;

        int mCapacity = 0; ///< Ring slots, every block; 0 until the first arc exists.
        /// Death time (sim clock) per ring slot - the only per-mote state on
        /// the CPU, 4 bytes each. Moves the windows and counts the living.
        std::vector<float> mDeathTime;

        /// Scratch, reused every frame so the frame path does not allocate.
        std::vector<ParticleSpawn>  mSpawnScratch;  ///< One arc's newborns this frame.
        std::vector<ParticleRecord> mRecordScratch; ///< Their records, in ring order.
        /// This frame's latch writes, every arc, 8 floats each - slot, spawn
        /// point in rect-local px (x, y), CPU colour as straight rgba, and 1
        /// to copy the neon's drawn glow instead of that colour.
        std::vector<float>          mLatchScratch;

        std::weak_ptr<const NeonRenderer> mNeonSource;
        /// Set at the top of each Render from what the neon left behind this
        /// frame. The CPU ring colour is only computed for an arc without a
        /// custom colour, and only when this is COLOR_RING.
        ParticleColorSource mColorSource = ParticleColorSource::COLOR_RING;

        /// @c NeonConfig::colorStops sorted ascending, as
        /// @ref ColorUtils::SampleRing requires. Rebuilt in OnConfigChanged.
        std::vector<ColorStop> mSortedRingStops;

        /// Sim clock: advances by the clamped step only while the effect
        /// clock does, so pausing or a long stall freezes every mote instead
        /// of ageing it. Monotonic while the layer runs - a clock that jumps
        /// back clears the motes rather than rewinding this - and back to 0
        /// when the layer is switched off, with everything else.
        float mSimTime = 0.0f;
        float mLastTime = -1.0f; ///< Effect clock at the last enabled Render; negative = none yet.
        /// True from the first enabled Render until the layer is switched off
        /// - while false it holds no motes and no ring or latch memory, and
        /// @ref releaseAll has nothing to do.
        bool mRunning = false;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_PARTICLE_RENDERER_H_
