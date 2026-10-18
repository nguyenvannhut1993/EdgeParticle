#ifndef _EDGE_LIGHTING_SPOTLIGHT_RENDERER_H_
#define _EDGE_LIGHTING_SPOTLIGHT_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/framebuffer.h"

#include <vector>

namespace EdgeLighting
{
    /// Freely placed and aimed cones of light, drawn as ONE screened pass.
    ///
    /// The odd one out in this library: every other renderer parameterises its
    /// look by position along the rect's perimeter, and this one does not
    /// touch the rect at all. A lamp sits at a @c SpotLight::position in app
    /// coordinates and points at a @c SpotLight::angle, and the rect does not
    /// occlude it - a cone crosses the frame freely. The only occluder is one
    /// the host places explicitly: see the shadow caster below.
    ///
    /// It also emits LIGHT ONLY. No backdrop, no fixture housings, no floor,
    /// no framebuffer capture, no LUT. What it is is the whole term stack in
    /// spotlight.frag, evaluated over the smallest geometry that can contain
    /// it.
    ///
    /// Light only, but NOT alpha-free: the pass SCREENS - `dst + src * (1 -
    /// dst)` - in the alpha channel as well as in colour, so the framebuffer
    /// ends up recording where light was written. It used to emit a literal
    /// alpha 0, which costs nothing on an opaque desktop window and erases the
    /// entire layer on an embedded surface a compositor or hardware video
    /// plane blends by alpha. A screen rather than an add so that overlapping
    /// lamps approach full scale instead of clipping. See @ref Render for the
    /// blend and the measurements.
    ///
    /// **One renderer, two resolution paths**, selected by
    /// @c SpotlightConfig::resolutionScale: at 1.0 the strips draw straight
    /// onto the framebuffer this renderer was handed (no offscreen buffer, no
    /// blit); below 1.0 they draw into a buffer of that fraction of the
    /// viewport and are bilinear-blitted back.
    ///
    /// **An enabled clipped or shadow-casting lamp pins the scale to 1.0**,
    /// because the clip mask (and the shadow's edge at the caster) is
    /// resolved per fragment and a reduced buffer would resolve its
    /// boundary at that buffer's texel pitch - moving the edge, leaking light
    /// outside a @c KEEP_INSIDE area, and flattening
    /// @c ClipArea::edgeSoftness. The scaled path below is therefore only
    /// ever reached with no lamp clipped or casting. See
    /// @c GetClampedSpotScale in the .cpp for the measurements and for why
    /// the neon's fix - moving its cut into the blit - could not be reused
    /// here.
    ///
    /// **No uniform that shapes the light differs between the paths.** The
    /// fragment stage reads only interpolated full-res lamp-local coordinates
    /// and flat per-lamp pixel values, so the scaling happens entirely in the
    /// viewport transform: the same ortho over the same app coordinates,
    /// rasterised into a smaller buffer. Only the render target, the blit, the
    /// buffer allocation and WHERE THE OUTPUT IS DITHERED are conditional,
    /// which is what keeps 1.0 bit-identical to the single-path renderer this
    /// grew out of. The dither is the one uniform (@c uDither): at 1.0 the
    /// strips dither their own write to the caller's 8-bit target; below it
    /// they write an undithered half-float buffer and spotlight-blit.frag
    /// dithers after its bilinear read, because a buffer texel is 1 / scale
    /// px wide and the read would average a per-texel dither into blotches -
    /// and an RGBA8 buffer would have rounded the dark tail into blocky
    /// contours first. See SCALED_FORMATS in the .cpp.
    ///
    /// Be aware of what the scale is worth here, because it is NOT the neon's
    /// or the flare's bargain: those shade the whole viewport, so quartering
    /// their fragments always beats a blit. This pass already bounds itself to
    /// what the lamps light, so the blit's full viewport of fragments is a
    /// FIXED cost it may not earn back - measured, it wins only above about
    /// six lamps at default settings. The default is 1.0 for that reason.
    ///
    /// **The geometry is the interesting part.** Each lamp gets a strip of
    /// @c SPOT_STRIP_SEGMENTS quads that hugs the region where that lamp
    /// writes anything at all, and every lamp's strip goes into one VBO drawn
    /// with one @c glDrawArrays. The bound is SOLVED, not guessed: the
    /// renderer inverts spotlight.frag's own falloff to find where it drops
    /// below half an 8-bit step (@ref SolveConeAcross), shared across the
    /// enabled lamps, and stops there. Measured at 1280x720, a typical single
    /// lamp rasterises 12-44% of the viewport and a five-lamp fan 123%;
    /// flattening the taper into an oriented bounding box of the same solve
    /// costs 1.16x to 1.50x more for an identical image.
    ///
    /// **Per-lamp scalars ride as vertex attributes**, constant across each
    /// strip, rather than in a std140 block. There is therefore no per-index
    /// array in either shader stage - the project's no-bare-uniform-arrays
    /// rule is satisfied by construction rather than by a block and a binding
    /// point - and the vertex stage pre-rotates each corner into the lamp's
    /// frame, so the fragment program never touches a sin or a cos.
    ///
    /// **The VBO holds app coordinates verbatim** and the y-flip lives in the
    /// projection (see @ref Render), so it depends only on
    /// @c Config::spotlight and never on the viewport: a resize costs one
    /// uniform, not a rebuild.
    ///
    /// **Light can be cut off by an area** - @c SpotlightConfig::clipArea, a
    /// rounded rectangle in the same app coordinates, kept on whichever side
    /// @c ClipMode names and opted into per lamp by
    /// @c SpotLight::clipped. It is a coverage multiply in the fragment stage,
    /// not a change to the falloff, so a clipped lamp is the same lamp with
    /// part of it missing. In @c ClipMode::KEEP_INSIDE the solve
    /// ALSO tightens the strip to the area's footprint in each lamp's frame,
    /// which is the one case where a clip makes the pass cheaper rather than
    /// only smaller on screen; @c KEEP_OUTSIDE leaves an unbounded region lit
    /// and so leaves the geometry alone.
    ///
    /// Being a per-fragment mask is also what makes it incompatible with a
    /// reduced-resolution buffer, which is why opting a lamp in costs the
    /// resolution scale - see the note on the two paths above.
    ///
    /// **Light can be shadowed by a picture** -
    /// @c SpotlightConfig::shadowCaster, a rounded rectangle standing
    /// @c ShadowCaster::wallDistance px off the wall, opted into per lamp by
    /// @c SpotLight::castsShadow. Each fragment tests the line from itself to
    /// the lamp (every lamp stands SPOT_LAMP_WALL_DISTANCE px off the wall)
    /// against the caster, so the shadow is thrown away from the lamp and
    /// follows it when it moves; the lamp's aperture sets the penumbra. The
    /// caster's own face stays lit. Like the clip it is a coverage multiply
    /// that only removes light, so the solved strips stand as they are - and
    /// like the clip it pins the resolution scale.
    ///
    /// The caster is a flat panel by default. @c ShadowCaster::solid makes it
    /// a block from the wall out to its depth - a framed picture - whose
    /// shadow is attached to it and, for a lamp no further from the wall than
    /// the block is deep, never ends; @c frameWidth / @c frameDepth give it
    /// rails that shade the recessed picture inside them. See
    /// docs/spotlight-solid-frame-plan.md.
    ///
    /// By default that shadow is physical: a second lamp lights a point in
    /// the first one's shadow with exactly its own light, so the shadow shows
    /// lighter wherever the second lamp is weaker. @c ShadowCaster::fill
    /// washes it out instead, at the price of a SECOND PASS: the lamps are
    /// drawn into @c mFillBuffer first, then drawn again onto the target by
    /// a variant of the same program that reads how much of each pixel's
    /// light came from the other lamps.
    ///
    /// All of the shadow's code sits together: the CPU derivation in the
    /// .cpp's THE SHADOW CASTER block, the GL side in this class's THE
    /// SHADOW CASTER method and member groups, the GLSL in spotlight.frag's
    /// THE SHADOW section, and its per-lamp attributes at the end of
    /// @c StripVertex.
    ///
    /// Parameters come from @c Config::spotlight. Nothing else is read - not
    /// @c Config::geometry, not @c Config::neon. The clip area is part of that
    /// sub-config rather than a reuse of @c Config::geometry for exactly this
    /// reason.
    class SpotlightRenderer : public BaseRenderer
    {
    public:
        SpotlightRenderer() = default;
        virtual ~SpotlightRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;
        virtual RendererLayer GetLayer() const override { return RendererLayer::SPOTLIGHT; }

    private:
        /// One vertex of a lamp's strip. Declared here rather than in the .cpp
        /// only so @c mStripVerts below can be a member; nothing outside this
        /// class names it. Everything but @c pos and @c local is constant
        /// across a whole strip - see spotlight.vert for why those ride as
        /// attributes instead of sitting in a uniform block.
        typedef struct StripVertex
        {
            float pos[2];   ///< App px, top-left origin, +y down.
            float local[2]; ///< (along, across) px in the lamp's frame.
            float p0[4];    ///< tanHalfBeam, throwLength, softK, intensity.
            float p1[4];    ///< apertureWidth, bloom, bloomRadius, bloomWindow.
            float color[3]; ///< Linear RGB.
            /// How much of @c SpotlightConfig::clipArea this lamp honours:
            /// 1 for a clipped lamp, 0 for one that crosses the area freely.
            ///
            /// A WEIGHT rather than a flag, and named for it, because that is
            /// how the shader uses it - `mix(1.0, mask, w)`, never a branch
            /// (see spotlight.frag). A float for the same reason, not because
            /// an attribute cannot be an integer.
            ///
            /// Baked here, so a lamp that opted out and a clip area that is
            /// switched off are the same thing by the time the GPU sees them.
            float clipWeight;
            /// Exponent on the cone's spread term - @c SpotLight::spreadFalloff,
            /// clamped by @c DeriveLamp.
            ///
            /// A seventeenth float rather than a fifth component of some
            /// existing vec4, because @c p0 and @c p1 are both full and
            /// @c color + @c clipWeight already pack into one. The odd stride
            /// costs nothing here: these are separate attribute pointers, not
            /// a std140 block.
            float spreadFalloff;

            // THE SHADOW CASTER, per lamp - baked by DeriveLampShadow and read
            // by spotlight.frag's THE SHADOW section (attributes 7, 8, 9).
            /// The shadow, per lamp: the lamp's app position (x, y), the ratio
            /// t = caster / lamp wall distance, and the penumbra's half width
            /// on the caster's plane in px. Well-formed for EVERY lamp, a lamp
            /// that casts nothing included (t 0, half width floored), so the
            /// mask stays finite if it is ever evaluated for one - today
            /// spotlight.frag only reaches it for a weight above 0. See
            /// spotShadowMask.
            float shadow[4];
            /// How much of the caster this lamp honours: @c ShadowCaster::opacity
            /// for a lamp that casts a shadow, 0 for one that does not. A
            /// WEIGHT, for the reason @c clipWeight is one.
            float shadowWeight;
            /// The frame's rails shading the picture inside them, per lamp:
            /// t_in = frameDepth / (lamp distance - picture depth), and the
            /// penumbra's half width at the rails' front in px. (0, floored
            /// width) when the caster has no recess. See
            /// docs/spotlight-solid-frame-plan.md section 3.2.
            float recess[2];
        } StripVertex;

        static_assert(sizeof(StripVertex) == 24 * sizeof(float),
                      "StripVertex must be tightly packed - ensureBuffer's "
                      "attribute pointers use sizeof(StripVertex) as the stride.");

        bool setupShaders();

        /// The uniforms every strip program reads - the projection, the
        /// dither switch, the clip area and (through
        /// @ref uploadShadowUniforms) the shadow caster. One function for the
        /// plain program and the fill variant, so the two passes of a filled
        /// frame cannot draw the strips under different values.
        void uploadStripUniforms(ShaderProgram &program, const glm::mat4 &mvp,
                                 float dither, const Config &config);

        void setupBlitGeometry();

        /// Allocate @c mScaledBuffer at @p width x @p height in the best
        /// format the driver will render to - half float, else RGBA8 - and
        /// leave @c mScaledFormat on it. See SCALED_FORMATS in the .cpp.
        /// @return false only if no format allocated, in which case there is
        ///         no attachment and nothing may be bound or cleared.
        bool resizeScaledBuffer(int width, int height);

        /// Allocate the VBO at its CEILING size and record the attribute
        /// layout, once. Never sized to the live lamp count: that would put a
        /// reallocation on the animation path, which is the mistake the neon
        /// emission table's sizing comment already records.
        void ensureBuffer();

        /// Solve every enabled lamp's support and upload the strips.
        ///
        /// Runs from @ref OnConfigChanged (and @ref Initialize), not from
        /// @ref Render, because its inputs are exactly @c Config::spotlight -
        /// the same invariant the lens flare's ghost table rests on. Under an
        /// animation driving any lamp scalar it therefore runs EVERY FRAME,
        /// so it is worth knowing what one costs.
        ///
        /// Measured over 2,000 rebuilds, best of nine runs: **~20 us for one
        /// lamp, ~32 us for eight**, or 0.1 - 0.2% of a 16.7 ms frame. That
        /// includes the <= 37 KB @c glBufferSubData into the ceiling-sized
        /// allocation @ref ensureBuffer made once. (Those numbers predate the
        /// clip attribute, which added one float per vertex and no
        /// transcendental calls at all, and the shadow's seven, which take the
        /// ceiling upload to 54 KB and add one divide per lamp.)
        ///
        /// The flatness across lamp counts is the useful part of that number.
        /// The arithmetic is **998 transcendental calls for one lamp and 7,880
        /// for eight** (counted: 705 / 5,688 @c log, 290 / 2,168 @c sqrt), so
        /// eight lamps do eight times the maths for 1.6x the time - most of
        /// what is left is the upload and the call. A profile that lands here
        /// is more likely to be showing the driver than the solve.
        ///
        /// If the solve ever IS the cost, ~88% of those calls are the
        /// @c WIDEN_SUBSAMPLES loop - 208 @ref SupportAt evaluations per lamp
        /// against 13 for the sampling it corrects. Read the comment there
        /// first: it buys a guarantee, not an image, and it has never changed
        /// a pixel in any verification scene.
        void buildStrips(const SpotlightConfig &spotlight);

        // --- THE SHADOW CASTER --------------------------------------------
        // The GL side of SpotlightConfig::shadowCaster. Its CPU derivation
        // (DeriveShadow, LampCastsShadow, DeriveLampShadow, UsesFillPass,
        // UsesDarkenPass) is
        // the THE SHADOW CASTER block of the .cpp's anonymous namespace; these
        // are defined together at the end of the .cpp.

        /// The caster's uniforms - its rectangle, the solid switch, the
        /// feather and the recessed picture. Called by
        /// @ref uploadStripUniforms, so both strip programs get them.
        void uploadShadowUniforms(ShaderProgram &program, const Config &config);

        /// The fill's FIRST pass target: allocate @c mFillBuffer at the
        /// viewport's size, bind it and clear it. Render then draws the plain
        /// strips into it.
        /// @return false if the buffer would not allocate - nothing is bound
        ///         then, and the frame must not draw.
        bool bindFillTarget(int width, int height);

        /// The fill's SECOND pass: the strips again, onto the caller's target
        /// (which Render has already put back), through @c mFillShader, which
        /// reads @c mFillBuffer to lift each lamp's shadow by the light the
        /// OTHER lamps put there.
        void renderFillPass(const glm::mat4 &mvp, float dither, const Config &config,
                            int viewportHeight);

        /// The fill's own uniforms - its source buffer on unit 0, the amount
        /// and the buffer's height - for whichever program reads it: the fill
        /// pass, and the darkening pass of a filled frame.
        void uploadFillUniforms(ShaderProgram &program, const Config &config, int viewportHeight);

        /// Build @c mDarkenShader and @c mDarkenFillShader on the first frame
        /// that darkens. Lazily, unlike the fill: most hosts never set
        /// @c ShadowCaster::darkness, and should not pay two more compiles at
        /// startup. Tried once; a failure is logged and the frame draws the
        /// light-only shadow.
        void ensureDarkenPrograms();

        /// The DARKENING pass (@c ShadowCaster::darkness): the casting lamps'
        /// strips, before the light, onto the caller's target, through the
        /// SPOT_SHADOW_DARKEN variant - each lamp's shadow written as
        /// premultiplied colour OVER the destination, so a bright backdrop
        /// darkens under it and the umbra records coverage. @p withFill picks
        /// the variant that also reads @c mFillBuffer, so the fill lifts the
        /// darkening with the shadow. Sets its own blend and puts the layer's
        /// screen back.
        void renderShadowDarkenPass(const glm::mat4 &mvp, float dither, const Config &config,
                                    int viewportHeight, bool withFill);

        /// A run of strip vertices whose lamps cast a shadow - what the
        /// darkening pass draws. Neighbouring casting lamps share one run.
        typedef struct CasterRange
        {
            int first; ///< First vertex in the VBO.
            int count; ///< Vertices in the run.
        } CasterRange;

    private:
        ShaderProgram mShaderProgram;
        /// Scaled path only: composites the scaled buffer back at full res,
        /// and dithers it on the way (spotlight-blit.frag). Built
        /// unconditionally rather than lazily - a shader compile in the
        /// middle of a frame, the first time someone drags the scale slider
        /// off 1.0, is a stall exactly where it will be blamed on the scale.
        ShaderProgram mBlitShader;
        /// Allocated only when the scaled path first runs; at scale 1.0 this
        /// stays empty and costs nothing, and it is released again as soon as
        /// a config stops asking for it. Half float where the driver renders
        /// to it, RGBA8 otherwise - see @ref resizeScaledBuffer.
        Framebuffer mScaledBuffer{"Spotlight.Scaled"};
        /// Index into SCALED_FORMATS (the .cpp) of the best format the driver
        /// has not refused for @c mScaledBuffer, where the walk resumes. Kept
        /// beside the buffer rather than in it because the buffer is released
        /// between uses. Also picks spotlight.frag's @c uDither on the scaled
        /// path: 0 in half float, 1 in RGBA8.
        size_t mScaledFormat = 0;
        VertexArray mVertexArray{"SpotlightRenderer"};
        /// Fullscreen NDC quad for the blit. A second VAO rather than a reuse
        /// of @c mVertexArray: that one carries the five-attribute strip
        /// layout, and the blit wants a bare vec2 at location 0.
        VertexArray mBlitQuad{"Spotlight.Blit"};

        /// The spotlight config behind the current VBO contents. A whole
        /// @c SpotlightConfig rather than hand-picked fields: it carries its
        /// own @c operator== and so cannot fall out of step with itself.
        /// Deliberately NOT a whole @c Config - nothing outside
        /// @c Config::spotlight reaches the strips.
        SpotlightConfig mCurrentSpotlight;

        /// Staging for @ref buildStrips, a member rather than a local for the
        /// reason @ref ensureBuffer gives about the VBO: under an animation
        /// that method runs every frame, and a local would heap-allocate and
        /// free its 54 KB ceiling on each one. Cleared, never shrunk, so it
        /// allocates exactly once.
        std::vector<StripVertex> mStripVerts;

        int mVertexCount = 0;      ///< Vertices @ref buildStrips last wrote.
        bool mBufferReady = false; ///< Whether @ref ensureBuffer has run.
        /// Whether the VBO has been filled for @c mCurrentSpotlight. Starts
        /// false so the first build is unconditional however the renderer is
        /// brought up - @ref Initialize and @ref OnConfigChanged both honour it.
        bool mBuilt = false;

        // --- THE SHADOW CASTER --------------------------------------------

        /// spotlight.frag with SPOT_FILL defined: the second pass of a frame
        /// with @c ShadowCaster::fill above 0, reading @c mFillBuffer to find
        /// how much of each pixel's light came from the OTHER lamps. Built in
        /// @ref setupShaders with the others, but NOT required by
        /// @ref Initialize - if it fails, a filled frame draws the physical
        /// shadow instead, and the layer keeps working.
        ShaderProgram mFillShader;
        /// The fill pass's source: every lamp, shadows applied, at the
        /// viewport's size. Same lifetime rule as @c mScaledBuffer - allocated
        /// the first frame a fill needs it, released when a config stops
        /// asking. Never needed alongside it: a fill needs a shadow-casting
        /// lamp, and that pins the scale to 1.0.
        Framebuffer mFillBuffer{"Spotlight.Fill"};

        /// spotlight.frag with SPOT_SHADOW_DARKEN - the darkening pass of a
        /// frame with @c ShadowCaster::darkness above 0 - and the same with
        /// SPOT_FILL as well, for a frame that also fills. Built by
        /// @ref ensureDarkenPrograms; optional like @c mFillShader.
        ShaderProgram mDarkenShader;
        ShaderProgram mDarkenFillShader;
        bool mDarkenProgramsTried = false; ///< Whether @ref ensureDarkenPrograms has run.

        /// The casting lamps' vertex runs, rebuilt by @ref buildStrips with
        /// the strips. Reserved to @c SPOT_MAX_LAMPS once, so a rebuild never
        /// allocates.
        std::vector<CasterRange> mCasterRanges;
    };
}

#endif // _EDGE_LIGHTING_SPOTLIGHT_RENDERER_H_
