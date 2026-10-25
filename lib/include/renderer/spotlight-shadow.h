#ifndef _EDGE_LIGHTING_SPOTLIGHT_SHADOW_H_
#define _EDGE_LIGHTING_SPOTLIGHT_SHADOW_H_

#include "core/config.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/framebuffer.h"

#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
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
    /// picture inside them. @c ShadowCaster::lightOnPicture dims a lamp's
    /// light on the caster's face, the rails' shadow on the picture darkening
    /// it instead.
    ///
    /// Two optional looks cost passes of their own. @c ShadowCaster::fill
    /// washes a lamp's shadow out by the OTHER lamps' light: the lamps are
    /// drawn into @c mFillBuffer in @ref RenderOffscreen, and after the light
    /// the SPOT_FILL variant screens back, per lamp, the light its lifted
    /// shadow lets through (@ref RenderAfterLight). @c ShadowCaster::darkness
    /// darkens the backdrop under a shadow: the casting lamps' strips through
    /// the SPOT_SHADOW_DARKEN variant, under the light, in
    /// @ref RenderBeforeLight - which also draws the rails' shadow on a
    /// picture kept out of the light, whatever darkness is.
    ///
    /// **Added to the light, never changing it.** SpotlightRenderer and its
    /// shaders belong to other people, so the shadow enters them only through
    /// INSERTED lines - a call at each hook below, one call in each shader's
    /// main(), and @c ShadowCaster with two fields in core/config.h - and not
    /// one of their existing lines differs. What that costs is carried here:
    ///   - its passes draw through programs of its own: a copy of the light's
    ///     plain program (the fill's source) and three variants (fill,
    ///     darkening, both), every one built from the light's own sources;
    ///   - it uploads the light's four strip uniforms onto them itself
    ///     (@ref uploadLightUniforms, a MIRROR of SpotlightRenderer's
    ///     drawLamps, which @ref warnUnmirroredUniforms checks against each
    ///     program it builds) and sets the layer's screen blend itself;
    ///   - its per-lamp attributes ride in a buffer of its own (@c mAttribs),
    ///     which @ref AttachAttributes binds into the light's strip VAO at
    ///     locations SPOT_SHADOW_ATTRIB_*, so StripVertex never carries them;
    ///   - the fill draws AFTER the light, as a correction, rather than in its
    ///     place, so the light's own draw call stays as it is.
    ///
    /// The hooks, in order of a session: @ref AttachAttributes (ensureBuffer),
    /// @ref AddLampStrip per drawn lamp and @ref OnStripsBuilt (buildStrips),
    /// @ref OnConfigChanged, @ref UploadUniforms (drawLamps),
    /// @ref PinsResolutionScale (GetClampedSpotScale), and per frame
    /// @ref RenderOffscreen (phase 1), then @ref RenderBeforeLight and
    /// @ref RenderAfterLight on either side of the light's draw (phase 2).
    /// The rest of the module: spotlight-shadow.glsl and
    /// spotlight-shadow-vert.glsl (injected ahead of spotlight.frag /
    /// spotlight.vert) and spotlight-shadow-tuning.h.
    ///
    /// docs/guides/spotlight-shadow.md is the reference;
    /// docs/design/spotlight-shadow-plan.md and
    /// docs/design/spotlight-solid-frame-plan.md the measured record.
    class SpotlightShadow
    {
    public:
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
        /// @p strips at locations SPOT_SHADOW_ATTRIB_*. Keeps @p strips to
        /// draw its own passes through; it must outlive this object (both
        /// are SpotlightRenderer's members).
        void AttachAttributes(const VertexArray &strips, int maxVertices);

        /// One drawn lamp's run of the strips ends at vertex @p endVertex:
        /// lamp @p lampIndex, whose solve floored its aperture to
        /// @p apertureWidth (the penumbra's size). Runs are back to back, so
        /// each starts where the one before ended - the first at 0.
        void AddLampStrip(int lampIndex, int endVertex, float apertureWidth);

        /// The strips are laid out - @p vertexCount in all, the runs
        /// @ref AddLampStrip reported: bake every vertex's shadow attributes,
        /// upload them and rebuild the casting lamps' runs. A vertex no run
        /// covers casts nothing.
        void OnStripsBuilt(const SpotlightConfig &spotlight, int vertexCount);

        /// Give @c mFillBuffer back the moment a config stops asking for the
        /// fill - same lifetime rule as SpotlightRenderer's scaled buffer -
        /// and say so in the log when a casting lamp starts or stops holding
        /// the scale (the light's own line speaks for the clip only).
        void OnConfigChanged(const Config &config);

        /// The caster's uniforms - its rectangle, the solid switch, the
        /// feather, the recessed picture and the light it takes - onto
        /// @p program, in use: the light's own program, from its drawLamps.
        /// Uploaded unconditionally, like the clip area's: a lamp that casts
        /// nothing carries a zero weight and never reads them.
        void UploadUniforms(ShaderProgram &program, const Config &config);

        /// Phase 1: the fill's source - every lamp, shadows applied, the face
        /// lit, screened into @c mFillBuffer under no host scissor - when this
        /// config fills. Otherwise nothing. Restores the framebuffer and
        /// viewport it was handed and leaves the blend as a Render does.
        void RenderOffscreen(int viewportWidth, int viewportHeight, const Config &config);

        /// Phase 2, before the light, onto the caller's target: the darkening
        /// pass when this config darkens. Sets its own blend and LEAVES it -
        /// the light sets its own after this. Runs @ref RenderOffscreen
        /// itself if nothing ran it for this viewport.
        void RenderBeforeLight(int viewportWidth, int viewportHeight, const Config &config);

        /// Phase 2, after the light, onto the caller's target: when this
        /// config fills, the casting lamps again through the SPOT_FILL
        /// variant, screening back what each lifted shadow lets through. Sets
        /// the layer's screen blend and leaves it. Ends the frame
        /// @ref RenderOffscreen began.
        void RenderAfterLight(int viewportWidth, int viewportHeight, const Config &config);

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
            /// Two WEIGHTS, as the clip's is one. [0]: how much of the caster
            /// this lamp honours - @c ShadowCaster::opacity for a lamp that
            /// casts a shadow, 0 for one that does not. [1]: 1 for a lamp
            /// @c ShadowCaster::lightOnPicture applies to (its light on the
            /// face scaled, the rails' shadow darkening the picture), 0
            /// otherwise - a gate of its own, since dimming the light needs
            /// no caster that casts.
            float weight[2];
            /// The frame's rails shading the picture inside them: t_in =
            /// frameDepth / (lamp distance - picture depth), and the penumbra's
            /// half width at the rails' front in px. (0, floored width) when
            /// the caster has no recess.
            float recess[2];
        } ShadowVertex;

        static_assert(sizeof(ShadowVertex) == 8 * sizeof(float),
                      "ShadowVertex must be tightly packed - AttachAttributes uses "
                      "sizeof(ShadowVertex) as the stride.");

        /// One drawn lamp's run of strip vertices, as @ref AddLampStrip
        /// reported it.
        typedef struct LampStrip
        {
            int lampIndex;       ///< Index into @c SpotlightConfig::lamps.
            int firstVertex;     ///< First vertex of the lamp's strip.
            int vertexCount;     ///< Vertices in it.
            float apertureWidth; ///< The aperture the light's solve used (floored).
        } LampStrip;

        /// A run of strip vertices whose lamps cast a shadow - what the
        /// darkening and fill passes draw. Neighbouring casting lamps share
        /// one run.
        typedef struct CasterRange
        {
            int first; ///< First vertex in the strips.
            int count; ///< Vertices in the run.
        } CasterRange;

        /// What @ref RenderOffscreen leaves for phase 2 in the same frame.
        typedef struct FrameState
        {
            bool prepared = false;  ///< RenderOffscreen ran and RenderAfterLight has not consumed it.
            glm::ivec2 viewport{0}; ///< The viewport it ran for.
            bool fillReady = false; ///< @c mFillBuffer holds this frame's lamps.
        } FrameState;

        /// Build @c mSourceShader and @c mFillShader on the first frame that
        /// fills. Tried once; a failure is logged and a filled frame draws
        /// the physical shadow.
        /// @return whether both programs are usable.
        bool ensureFillPrograms();

        /// Build @c mDarkenShader and @c mDarkenFillShader on the first frame
        /// that darkens. Tried once; a failure is logged and the frame draws
        /// the light-only shadow.
        void ensureDarkenPrograms();

        /// Whether @c mFillBuffer holds this frame's lamps at this viewport -
        /// running @ref RenderOffscreen first if nothing did, for a host that
        /// drives the renderer without EdgeLightingEffect.
        bool isFillReady(int viewportWidth, int viewportHeight, const Config &config);

        /// The fill's source target: allocate @c mFillBuffer at the
        /// viewport's size, bind it and clear it.
        /// @return false if the buffer would not allocate - nothing is bound
        ///         then, and the pass must not draw.
        bool bindFillTarget(int width, int height);

        /// The caster's uniforms onto @p program, in use - with uPictureLight
        /// 1 when @p realLight (the fill's source, which has to hold the
        /// lamps' real light), the config's value otherwise.
        void uploadCasterUniforms(ShaderProgram &program, const Config &config, bool realLight);

        /// The light's own strip uniforms onto one of this module's programs:
        /// what SpotlightRenderer's drawLamps uploads on the direct path - the
        /// y-flipped ortho, uDither 1 and the clip area - MIRRORED here, since
        /// the light's files take no call that would hand them over. Every
        /// shadow pass runs at full resolution (a casting lamp pins the
        /// scale), which is why uDither is a constant. Keep in step with
        /// drawLamps; @ref warnUnmirroredUniforms names anything it misses.
        void uploadLightUniforms(ShaderProgram &program, int viewportWidth, int viewportHeight,
                                 const Config &config);

        /// Log, once per program built, every active uniform of @p program
        /// this module does not upload - a uniform the light's shader gained
        /// after @ref uploadLightUniforms was written, which the shadow's
        /// passes would otherwise draw as 0.
        static void warnUnmirroredUniforms(const ShaderProgram &program, const char *label);

        /// The fill's own uniforms - its source buffer on unit 0, the amount
        /// and the buffer's height - for whichever program reads it: the fill
        /// pass, and the darkening pass of a filled frame.
        void uploadFillUniforms(ShaderProgram &program, const Config &config, int viewportHeight);

    private:
        /// The per-vertex shadow attributes, ceiling-sized once by
        /// @ref AttachAttributes and refilled by @ref OnStripsBuilt. A
        /// VertexArray for its buffer: its own VAO is never drawn, since the
        /// attributes are read through the light's (@c mStrips).
        VertexArray mAttribs{"Spotlight.Shadow"};
        /// The light's strip VAO, which every pass of this module draws -
        /// set by @ref AttachAttributes, null until then.
        const VertexArray *mStrips = nullptr;
        /// Staging for @c mAttribs, reserved to the ceiling once, so a
        /// rebuild under an animation never allocates.
        std::vector<ShadowVertex> mShadowVerts;
        int mMaxVertices = 0; ///< The ceiling @c mAttribs holds; 0 until attached.
        int mVertexCount = 0; ///< Strip vertices @ref OnStripsBuilt last saw.

        /// The runs @ref AddLampStrip reports during a rebuild, consumed and
        /// cleared by @ref OnStripsBuilt. Reserved to @c SPOT_MAX_LAMPS once.
        std::vector<LampStrip> mLampStrips;
        int mStripsEnd = 0; ///< Where the last reported run ended.

        /// The casting lamps' vertex runs, rebuilt with the attributes.
        /// Reserved to @c SPOT_MAX_LAMPS once.
        std::vector<CasterRange> mCasterRanges;

        /// The light's plain program, a copy of this module's own: the fill's
        /// source, so the light's program draws one target and this the
        /// other.
        ShaderProgram mSourceShader;
        /// spotlight.frag with SPOT_FILL defined: the fill's correction.
        ShaderProgram mFillShader;
        bool mFillProgramsTried = false; ///< Whether @ref ensureFillPrograms has run.
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

        /// Whether a casting lamp holds a requested scale below 1.0 at 1.0,
        /// as last logged by @ref OnConfigChanged.
        bool mScalePinLogged = false;
    };
}

#endif // _EDGE_LIGHTING_SPOTLIGHT_SHADOW_H_
