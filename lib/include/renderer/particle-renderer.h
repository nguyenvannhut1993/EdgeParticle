#ifndef _EDGE_LIGHTING_PARTICLE_RENDERER_H_
#define _EDGE_LIGHTING_PARTICLE_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    /// Point-sprite particle system with two selectable emission strategies
    /// (@ref ParticleConfig::mode, see @ref ParticleMode).
    ///
    /// @see ParticleConfig for tuning knobs. Compositing: rendered in the same
    /// local (rect-centered) coord space as the neon quad with premultiplied
    /// GL_ONE / one-minus-src-alpha blending, so it lays cleanly on top of the
    /// neon output.
    class ParticleRenderer : public BaseRenderer
    {
    public:
        ParticleRenderer() = default;
        virtual ~ParticleRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        struct Particle
        {
            glm::vec2 pos;      ///< Rect-local coords.
            glm::vec2 vel;      ///< Pixels / second.
            float     life;     ///< Seconds remaining.
            float     lifeMax;
            glm::vec3 color;    ///< Linear RGB.
            float     sizeStart;///< Pixels at spawn.
        };

        bool setupShaders();
        void agingStep(float dt, float drag);

        // --- ARC_HEAD_TAIL mode: sparks ejected from the arc's endpoints. ---
        void emitFromArcs(const Config &config, float dt, float time);
        void spawnAt(const Config &config, float perimeterPos, float time, float initialAge);

        // --- MOVEMENT mode: motes scattered along the whole lit arc. ---
        void emitAlongArc(const Config &config, float dt, float time);
        void spawnAlongArc(const Config &config, float time, float initialAge);

        void uploadAndDraw();

    private:
        ShaderProgram mShaderProgram;
        VertexArray   mVertexArray{"Particles"};

        std::vector<Particle> mParticles;
        std::vector<float>    mVboScratch; ///< 7 floats per particle: pos.xy, rgb, life, size.

        /// ARC_HEAD_TAIL: fractional spawn accumulator per arc-endpoint site
        /// so emissionRate * dt doesn't lose fractional particles at small dt.
        std::vector<float> mArcSpawnAcc;
        /// ARC_HEAD_TAIL: each site's position last frame, so a fast-moving
        /// (or low-fps, large-dt) endpoint spawns swept across its path
        /// instead of jumping straight to the new position and leaving gaps.
        std::vector<float> mPrevArcSites;
        /// MOVEMENT: single fractional spawn accumulator (spawns are scattered
        /// across the whole arc rather than tied to discrete sites).
        float mMovementSpawnAcc = 0.0f;

        float mLastTime = -1.0f;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_PARTICLE_RENDERER_H_
