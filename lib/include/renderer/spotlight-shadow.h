#ifndef _EDGE_LIGHTING_SPOTLIGHT_SHADOW_H_
#define _EDGE_LIGHTING_SPOTLIGHT_SHADOW_H_

#include "core/config.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/vertex-buffer.h"
#include "gl/framebuffer.h"

#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    /// What @ref SpotlightShadow draws through: the pieces of
    /// SpotlightRenderer its passes REUSE rather than copy - the strip
    /// uniforms (projection, dither, clip area), the layer's light blend, the
    /// plain light program and the strips themselves. Implemented privately
    /// by SpotlightRenderer, so this is the whole contract between the light
    /// and its shadow: change how the light uploads its uniforms or blends
    /// and every shadow pass follows, with nothing in the shadow to edit.
    ///
    /// Every call is a FULL-RESOLUTION draw onto whatever is bound: a lamp
    /// that casts a shadow pins @c SpotlightConfig::resolutionScale to 1.0,
    /// so no shadow pass ever runs beside the scaled path.
    class SpotShadowHost
    {
    public:
        /// Every uniform the plain strip program reads - spotlight.frag's own
        /// and, through @ref SpotlightShadow::UploadUniforms, the caster's -
        /// onto @p program, which the caller has in use. Called for the
        /// shadow's own variants so they draw the strips under exactly the
        /// light pass's values.
        virtual void UploadStripUniforms(ShaderProgram &program, int viewportWidth, int viewportHeight,
                                         const Config &config) = 0;
        /// The layer's blend for its light (the screen), enabled.
        virtual void UseLightBlend() = 0;
        /// The plain light pass - every lamp, shadows applied - into whatever
        /// is bound, under whatever blend is set.
        virtual void DrawLamps(int viewportWidth, int viewportHeight, const Config &config) = 0;
        /// Draw @p count strip vertices from @p first with the program in use.
        virtual void DrawStrips(int first, int count) = 0;

    protected:
        ~SpotShadowHost() = default;
    };

    /// One lamp's run of strip vertices, as SpotlightRenderer::buildStrips
    /// laid it out - what the shadow's per-lamp attributes are written over.
    /// Only lamps that emitted vertices have one.
    typedef struct SpotLampStrip
    {
        int lampIndex;       ///< Index into @c SpotlightConfig::lamps.
        int firstVertex;     ///< First vertex of the lamp's strip.
        int vertexCount;     ///< Vertices in it.
        float apertureWidth; ///< The aperture the light's solve used (floored), which sets the penumbra.
    } SpotLampStrip;

    /// The spotlight's shadow caster - @c SpotlightConfig::shadowCaster and
    /// @c SpotLight::castsShadow - as ONE module beside SpotlightRenderer.
    ///
    /// A picture standing off the wall: each fragment of a lamp that opts in
    /// tests the line from itself to the lamp (every lamp stands
    /// SPOT_LAMP_WALL_DISTANCE px off the wall) against the caster, so the
    /// shadow is thrown away from the lamp and follows it when it moves; the
    /// lamp's aperture sets the penumbra, and the caster's own face stays lit.
    /// It is a coverage multiply that only removes light, so the light's
    /// solved strips stand as they are - and, like the clip, it pins the
    /// resolution scale (@ref PinsResolutionScale). The caster is a flat panel
    /// by default; @c ShadowCaster::solid makes it a block from the wall out,
    /// and @c frameWidth / @c frameDepth give it rails that shade the recessed
    /// picture inside them.
    ///
    /// Two optional looks cost passes of their own. @c ShadowCaster::fill
    /// washes a lamp's shadow out by the OTHER lamps' light: the plain lamps
    /// are drawn into @c mFillBuffer in @ref RenderOffscreen, then drawn again
    /// onto the target by the SPOT_FILL variant in @ref RenderLight, which
    /// reads how much of each pixel's light came from elsewhere.
    /// @c ShadowCaster::darkness darkens the backdrop under a shadow: the
    /// casting lamps' strips through the SPOT_SHADOW_DARKEN variant, under
    /// the light, in @ref RenderBeforeLight.
    ///
    /// **Its own files, on purpose.** SpotlightRenderer and its shaders are
    /// edited by several people, so the shadow keeps everything it can out of
    /// them and enters each through a single marked hook:
    ///   - this class (spotlight-shadow.{h,cpp}) - the CPU derivation and every
    ///     GL object the shadow owns: its attribute buffer, its three programs,
    ///     its fill buffer;
    ///   - spotlight-shadow.glsl (fragment) and spotlight-shadow-vert.glsl
    ///     (vertex), injected ahead of spotlight.frag / spotlight.vert, which
    ///     each call into them once;
    ///   - spotlight-shadow-tuning.h, its constants;
    ///   - core/shadow-caster.h, its config.
    /// Its per-lamp attributes ride in a buffer of its own (@c mAttribs),
    /// bound into the light's strip VAO at locations SPOT_SHADOW_ATTRIB_*, so
    /// the light's StripVertex never carries them. What it needs of the light
    /// is @ref SpotShadowHost and nothing else.
    ///
    /// SpotlightRenderer calls, in order of a session: @ref AttachAttributes
    /// once its strip buffer exists, @ref OnStripsBuilt after every strip
    /// rebuild, @ref OnConfigChanged on every config change,
    /// @ref UploadUniforms inside its strip-uniform upload, and per frame
    /// @ref RenderOffscreen (phase 1), then @ref RenderBeforeLight and
    /// @ref RenderLight on the direct path of its Render (phase 2).
    ///
    /// docs/guides/spotlight-shadow.md is the reference;
    /// docs/design/spotlight-shadow-plan.md and
    /// docs/design/spotlight-solid-frame-plan.md the measured record.
    class SpotlightShadow
    {
    public:
        explicit SpotlightShadow(SpotShadowHost &host);

        /// Whether a lamp this config DRAWS has @c castsShadow set - what pins
        /// @c SpotlightConfig::resolutionScale to 1.0, beside the clip. Where
        /// the caster's lit face meets its own shadow the mask steps across
        /// one pixel, which a reduced buffer would smear exactly as it smears
        /// the clip's edge. Counts the bit whether or not the caster is set up
        /// to cast anything, so a caster dragged across zero size does not
        /// flip the path.
        static bool PinsResolutionScale(const SpotlightConfig &spotlight);

        /// Allocate the shadow's attribute buffer for @p maxVertices strip
        /// vertices - the light's own ceiling, once - and bind it into
        /// @p strips at locations SPOT_SHADOW_ATTRIB_*.
        void AttachAttributes(const VertexArray &strips, int maxVertices);

        /// Bake every vertex's shadow attributes for the strips just laid out
        /// - @p vertexCount in all, lamp by lamp as @p strips lists them - and
        /// upload them; rebuild the casting lamps' runs for the darkening
        /// pass. A vertex no strip lists casts nothing.
        void OnStripsBuilt(const SpotlightConfig &spotlight, const std::vector<SpotLampStrip> &strips,
                           int vertexCount);

        /// Give @c mFillBuffer back the moment a config stops asking for the
        /// fill - same lifetime rule as SpotlightRenderer's scaled buffer.
        void OnConfigChanged(const Config &config);

        /// The caster's uniforms - its rectangle, the solid switch, the
        /// feather and the recessed picture - onto @p program, in use.
        /// Uploaded unconditionally, like the clip area's: a lamp that casts
        /// nothing carries a zero weight and never reads them.
        void UploadUniforms(ShaderProgram &program, const Config &config);

        /// Phase 1: the fill's first pass - every lamp, shadows applied,
        /// screened into @c mFillBuffer, under no host scissor - when this
        /// config fills. Otherwise nothing. Restores the framebuffer and
        /// viewport it was handed and leaves the blend as a Render does.
        void RenderOffscreen(int viewportWidth, int viewportHeight, const Config &config);

        /// Phase 2, before the light, onto the caller's target: the darkening
        /// pass when this config darkens. Sets its own blend and LEAVES it, so
        /// the caller sets the light's blend after this. Runs
        /// @ref RenderOffscreen itself if nothing ran it for this viewport.
        void RenderBeforeLight(int viewportWidth, int viewportHeight, const Config &config);

        /// Phase 2, the light: when this config fills, the strips onto the
        /// caller's target through the SPOT_FILL variant, under the light's
        /// blend, and true - the caller then draws no light of its own.
        /// False, having drawn nothing, otherwise (a fill that could not be
        /// built or allocated included: the physical shadow is the right thing
        /// to fall back to). Ends the frame @ref RenderOffscreen began.
        bool RenderLight(int viewportWidth, int viewportHeight, const Config &config);

    private:
        /// One strip vertex's shadow attributes, constant along a lamp's strip.
        typedef struct ShadowVertex
        {
            /// The lamp's app position (x, y), the ratio t = caster / lamp
            /// wall distance, and the penumbra's half width on the caster's
            /// plane in px. Well-formed for EVERY lamp, a lamp that casts
            /// nothing included (t 0, half width floored), so the mask stays
            /// finite if it is ever evaluated for one - today
            /// spotlight-shadow.glsl only reaches it for a weight above 0.
            float shadow[4];
            /// How much of the caster this lamp honours:
            /// @c ShadowCaster::opacity for a lamp that casts a shadow, 0 for
            /// one that does not. A WEIGHT, as the clip's is.
            float weight;
            /// The frame's rails shading the picture inside them: t_in =
            /// frameDepth / (lamp distance - picture depth), and the penumbra's
            /// half width at the rails' front in px. (0, floored width) when
            /// the caster has no recess.
            float recess[2];
        } ShadowVertex;

        static_assert(sizeof(ShadowVertex) == 7 * sizeof(float),
                      "ShadowVertex must be tightly packed - AttachAttributes uses "
                      "sizeof(ShadowVertex) as the stride.");

        /// A run of strip vertices whose lamps cast a shadow - what the
        /// darkening pass draws. Neighbouring casting lamps share one run.
        typedef struct CasterRange
        {
            int first; ///< First vertex in the strips.
            int count; ///< Vertices in the run.
        } CasterRange;

        /// What @ref RenderOffscreen leaves for phase 2 in the same frame.
        typedef struct FrameState
        {
            bool prepared = false;  ///< RenderOffscreen ran and RenderLight has not consumed it.
            glm::ivec2 viewport{0}; ///< The viewport it ran for.
            bool fillReady = false; ///< @c mFillBuffer holds this frame's lamps.
        } FrameState;

        /// Build @c mFillShader on the first frame that fills. Tried once; a
        /// failure is logged and a filled frame draws the physical shadow.
        /// @return whether the program is usable.
        bool ensureFillProgram();

        /// Build @c mDarkenShader and @c mDarkenFillShader on the first frame
        /// that darkens. Tried once; a failure is logged and the frame draws
        /// the light-only shadow.
        void ensureDarkenPrograms();

        /// Whether @c mFillBuffer holds this frame's lamps at this viewport -
        /// running @ref RenderOffscreen first if nothing did, for a host that
        /// drives the renderer without EdgeLightingEffect.
        bool isFillReady(int viewportWidth, int viewportHeight, const Config &config);

        /// The fill's first pass target: allocate @c mFillBuffer at the
        /// viewport's size, bind it and clear it.
        /// @return false if the buffer would not allocate - nothing is bound
        ///         then, and the pass must not draw.
        bool bindFillTarget(int width, int height);

        /// The fill's own uniforms - its source buffer on unit 0, the amount
        /// and the buffer's height - for whichever program reads it: the fill
        /// pass, and the darkening pass of a filled frame.
        void uploadFillUniforms(ShaderProgram &program, const Config &config, int viewportHeight);

    private:
        SpotShadowHost &mHost;

        /// The per-vertex shadow attributes, ceiling-sized once by
        /// @ref AttachAttributes and refilled by @ref OnStripsBuilt.
        VertexBuffer mAttribs{"Spotlight.Shadow"};
        /// Staging for @c mAttribs, reserved to the ceiling once, so a
        /// rebuild under an animation never allocates.
        std::vector<ShadowVertex> mShadowVerts;
        int mMaxVertices = 0; ///< The ceiling @c mAttribs holds; 0 until attached.
        int mVertexCount = 0; ///< Strip vertices @ref OnStripsBuilt last saw.

        /// The casting lamps' vertex runs, rebuilt with the attributes.
        /// Reserved to @c SPOT_MAX_LAMPS once.
        std::vector<CasterRange> mCasterRanges;

        /// spotlight.frag with SPOT_FILL defined: the fill's second pass.
        ShaderProgram mFillShader;
        bool mFillProgramTried = false; ///< Whether @ref ensureFillProgram has run.
        /// The fill's source: every lamp, shadows applied, at the viewport's
        /// size. Allocated the first frame a fill needs it, released when a
        /// config stops asking.
        Framebuffer mFillBuffer{"Spotlight.Fill"};

        /// spotlight.frag with SPOT_SHADOW_DARKEN - the darkening pass - and
        /// the same with SPOT_FILL as well, for a frame that also fills.
        ShaderProgram mDarkenShader;
        ShaderProgram mDarkenFillShader;
        bool mDarkenProgramsTried = false; ///< Whether @ref ensureDarkenPrograms has run.

        /// What this frame's @ref RenderOffscreen left - see @ref FrameState.
        FrameState mFrame;
    };
}

#endif // _EDGE_LIGHTING_SPOTLIGHT_SHADOW_H_
