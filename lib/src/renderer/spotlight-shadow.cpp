#include "renderer/spotlight-shadow.h"
#include "renderer/spotlight-tuning.h"
#include "renderer/spotlight-shadow-tuning.h"
#include "util/log-util.h"
#include "util/gl-utils.h"
#include "shaders.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace EdgeLighting
{
    namespace
    {
        // ===================================================================
        // THE SHADOW CASTER's CPU derivation of SpotlightConfig::shadowCaster.
        // Pure functions of the config; the GL side is SpotlightShadow below,
        // the GLSL spotlight-shadow.glsl.
        // ===================================================================

        /// Floor under the shadow penumbra's half width on the caster's plane.
        /// spotShadowMask feeds it to smoothstep as both edges, and equal
        /// edges are undefined - possibly a NaN, which the zero weight of a
        /// lamp that casts nothing would NOT cancel (mix(1, NaN, 0) is NaN).
        constexpr float MIN_SHADOW_HALF_WIDTH = 1.0e-4f;

        /// How far every casting lamp stands off the wall - see
        /// SPOT_LAMP_WALL_DISTANCE for why it is one number, not a field per
        /// lamp.
        constexpr float LAMP_WALL_DISTANCE = static_cast<float>(SPOT_LAMP_WALL_DISTANCE);

        /// Ceiling on a PANEL's t = caster / lamp wall distance. The shadow
        /// is the caster scaled by 1 / (1 - t), so as the caster comes out to
        /// the lamps' distance the shadow grows without bound; this stops it
        /// at 100x, where it has long since left any viewport.
        constexpr float MAX_SHADOW_RATIO = 0.99f;

        /// Ceiling on the rails' ratio t_in = frameDepth / (lamp - picture
        /// depth). At or past 1 the line from the picture to the lamp never
        /// clears the rails - the rails' front is at least as far out as
        /// the lamps - and any value past 1 puts the test point beyond the
        /// lamp and out of the opening; this only keeps it finite as the
        /// picture's own depth nears the lamps'.
        constexpr float MAX_RECESS_RATIO = 64.0f;

        /// @c ShadowCaster resolved for the shader and the per-lamp bake: a
        /// CENTRED box with a clamped radius, as the clip area's ClipSolve
        /// is, plus whether the caster can throw anything at all, plus its
        /// frame.
        typedef struct ShadowSolve
        {
            glm::vec2 center;   ///< App px.
            glm::vec2 half;     ///< Half extent, app px, never negative.
            float radius;       ///< Corner radius, clamped to the shorter half extent.
            float softness;     ///< Extra feather on the wall in px, never negative.
            float opacity;      ///< Clamped to [0, 1].
            float wallDistance; ///< The caster's face off the wall, px.
            bool solid;         ///< A block from the wall to the face, not a panel.
            /// False for a caster that blocks nothing whichever lamp looks
            /// at it - no area, flush with the wall, or fully transparent - so
            /// every lamp's weight can be written as 0 rather than paying for
            /// a mask that is 1 everywhere.
            bool casts;
            /// The picture inside the rails: the box inset by
            /// @c ShadowCaster::frameWidth, its radius reduced to match.
            glm::vec2 innerHalf;
            float innerRadius;
            /// @c ShadowCaster::frameDepth, clamped to [0, wallDistance].
            float frameDepth;
            /// Whether the rails shade the picture: a frame with width AND
            /// depth, leaving a picture of some size inside it.
            bool hasRecess;
        } ShadowSolve;

        /// Signed distance to a rounded box centred at the origin, negative
        /// inside - the CPU twin of sdRoundBox in spotlight.frag, used only to
        /// ask whether a lamp sits inside a solid caster.
        float SdRoundBox(glm::vec2 p, glm::vec2 b, float r)
        {
            const glm::vec2 q = glm::abs(p) - b + glm::vec2(r);
            return std::min(std::max(q.x, q.y), 0.0f) +
                   glm::length(glm::max(q, glm::vec2(0.0f))) - r;
        }

        /// Derive the shadow caster. One function for
        /// @ref SpotlightShadow::UploadUniforms (the uniforms) and
        /// @ref SpotlightShadow::OnStripsBuilt (the per-lamp attributes), for
        /// the reason the light's DeriveClip is one.
        ShadowSolve DeriveShadow(const ShadowCaster &caster)
        {
            ShadowSolve out{};
            out.half = glm::vec2(std::max(caster.width, 0.0f) * 0.5f,
                                 std::max(caster.height, 0.0f) * 0.5f);
            out.center = caster.position + out.half;
            out.radius = std::min(std::max(caster.cornerRadius, 0.0f),
                                  std::min(out.half.x, out.half.y));
            out.softness = std::max(caster.softness, 0.0f);
            out.opacity = std::min(std::max(caster.opacity, 0.0f), 1.0f);
            out.wallDistance = caster.wallDistance;
            out.solid = caster.solid;
            out.casts = out.half.x > 0.0f && out.half.y > 0.0f &&
                        out.wallDistance > 0.0f && out.opacity > 0.0f;

            const float rail = std::min(std::max(caster.frameWidth, 0.0f),
                                        std::min(out.half.x, out.half.y));
            out.innerHalf = out.half - glm::vec2(rail);
            out.innerRadius = std::min(std::max(out.radius - rail, 0.0f),
                                       std::min(out.innerHalf.x, out.innerHalf.y));
            out.frameDepth = std::min(std::max(caster.frameDepth, 0.0f),
                                      std::max(out.wallDistance, 0.0f));
            out.hasRecess = rail > 0.0f && out.frameDepth > 0.0f &&
                            out.innerHalf.x > 0.0f && out.innerHalf.y > 0.0f;
            return out;
        }

        /// Whether @p lamp throws a shadow of this caster at all. ONE rule for
        /// @ref DeriveLampShadow and @ref UsesFillPass, so the fill never runs
        /// for a rig whose strips all carry a zero weight, or the reverse.
        ///
        /// Every lamp stands LAMP_WALL_DISTANCE off the wall, so this is the
        /// lamp's opt-in and the caster alone, with one exception: a SOLID
        /// block at least that deep has the lamps grazing its side - the
        /// case whose shadow never ends - and a lamp INSIDE its footprint
        /// then has nothing to say. A PANEL that deep casts nothing: the
        /// lamps are behind its face, and nothing is lost on screen by
        /// dropping them - as its distance nears theirs the shadow grows as
        /// 1 / (1 - t) and has left the viewport well before (byte-identical
        /// to holding it at MAX_SHADOW_RATIO, without the pass).
        bool LampCastsShadow(const ShadowSolve &shadow, const SpotLight &lamp)
        {
            if (!lamp.castsShadow || !shadow.casts)
            {
                return false;
            }
            if (shadow.wallDistance < LAMP_WALL_DISTANCE)
            {
                return true;
            }
            if (!shadow.solid)
            {
                return false;
            }
            return SdRoundBox(lamp.position - shadow.center, shadow.half, shadow.radius) > 0.0f;
        }

        /// What one lamp's strip carries for the shadow: @p outShadow is the
        /// lamp's app position, t, and the penumbra's half width on the
        /// caster's plane; @p outRecess is the rails' t_in and their penumbra
        /// half width; the return value is the weight.
        ///
        /// The half width is the lamp's emitter - a disc of radius
        /// @p apertureWidth - projected onto the caster's plane from the wall,
        /// `apertureWidth * t`, plus half of @c ShadowCaster::softness carried
        /// from the wall to that plane, where one wall px is `1 - t` plane px.
        /// On the wall, then, the penumbra is
        /// `2 * apertureWidth * t / (1 - t) + softness` wide.
        ///
        /// A SOLID caster reads t as how far along the line to the lamp the
        /// block still reaches, `min(d / h, 1)`, and recomputes the half width
        /// per sample in the shader; the panel's value here is the far end.
        ///
        /// A lamp that casts nothing still gets well-formed values - t 0, the
        /// floored half widths - so the mask stays finite if it is ever
        /// evaluated for one. Today spotlight-shadow.glsl only reaches it for
        /// a weight above 0 (see spotShadowLight).
        float DeriveLampShadow(const ShadowSolve &shadow, const SpotLight &lamp,
                               float apertureWidth, float outShadow[4], float outRecess[2])
        {
            outShadow[0] = lamp.position.x;
            outShadow[1] = lamp.position.y;
            outShadow[2] = 0.0f;
            outShadow[3] = MIN_SHADOW_HALF_WIDTH;
            outRecess[0] = 0.0f;
            outRecess[1] = MIN_SHADOW_HALF_WIDTH;

            if (!LampCastsShadow(shadow, lamp))
            {
                return 0.0f;
            }

            const float h = LAMP_WALL_DISTANCE;
            // A panel stops short of 1, where its projection would leave every
            // viewport; a block reaches 1 - the whole line to the lamp - once
            // it is as deep as the lamps are far out, and the shader's search
            // handles that end without any division by 1 - t.
            const float t = shadow.solid ? std::min(shadow.wallDistance / h, 1.0f)
                                         : std::min(shadow.wallDistance / h, MAX_SHADOW_RATIO);
            const float tFar = std::min(t, MAX_SHADOW_RATIO);
            outShadow[2] = t;
            outShadow[3] = std::max(apertureWidth * tFar + 0.5f * shadow.softness * (1.0f - tFar),
                                    MIN_SHADOW_HALF_WIDTH);

            if (shadow.hasRecess)
            {
                // The picture's surface sits frameDepth behind the rails'
                // front. A surface at least as far out as the lamps cannot see
                // them at all: the largest ratio, which puts the test point
                // past the lamp and so (for a lamp outside the opening) out of
                // it.
                const float pictureDepth = shadow.wallDistance - shadow.frameDepth;
                const float tIn = (h > pictureDepth)
                                      ? std::min(shadow.frameDepth / (h - pictureDepth), MAX_RECESS_RATIO)
                                      : MAX_RECESS_RATIO;
                const float tNear = std::min(tIn, 1.0f);
                outRecess[0] = tIn;
                outRecess[1] = std::max(apertureWidth * tNear + 0.5f * shadow.softness * (1.0f - tNear),
                                        MIN_SHADOW_HALF_WIDTH);
            }
            return shadow.opacity;
        }

        /// Whether any lamp this pass will DRAW throws a shadow of the caster -
        /// @ref LampCastsShadow, the test @ref DeriveLampShadow makes, over the
        /// same drawn lamps @ref SpotlightShadow::PinsResolutionScale counts.
        /// What both shadow passes below are gated on, so neither runs for a
        /// rig whose strips all carry a zero weight.
        bool AnyDrawnLampCasts(const SpotlightConfig &spotlight)
        {
            const ShadowSolve shadow = DeriveShadow(spotlight.shadowCaster);
            if (!shadow.casts)
            {
                return false;
            }

            const int lampCount = std::min(static_cast<int>(spotlight.lamps.size()),
                                           static_cast<int>(SPOT_MAX_LAMPS));
            for (int i = 0; i < lampCount; i++)
            {
                const SpotLight &lamp = spotlight.lamps[static_cast<size_t>(i)];
                if (lamp.enable && lamp.intensity > 0.0f && LampCastsShadow(shadow, lamp))
                {
                    return true;
                }
            }
            return false;
        }

        /// Whether this config's frame takes the fill's two passes: the layer
        /// on, @c ShadowCaster::fill above 0, and a drawn lamp that actually
        /// throws a shadow, so a fill with nothing to fill costs nothing.
        ///
        /// ONE predicate for the passes (which to draw) and
        /// @ref SpotlightShadow::OnConfigChanged (whether to keep the buffer),
        /// for the reason the light's UsesScaledBuffer is one.
        ///
        /// Never true alongside the light's scaled path: a lamp that throws a
        /// shadow has @c castsShadow set, and that pins the scale to 1.0.
        bool UsesFillPass(const Config &config)
        {
            const SpotlightConfig &spotlight = config.spotlight;
            return spotlight.enable && spotlight.shadowCaster.fill > 0.0f &&
                   AnyDrawnLampCasts(spotlight);
        }

        /// Whether this config's frame takes the darkening pass: the layer
        /// on, @c ShadowCaster::darkness above 0, and a drawn lamp that throws
        /// a shadow. Never alongside the scaled path, for the fill's reason.
        bool UsesDarkenPass(const Config &config)
        {
            const SpotlightConfig &spotlight = config.spotlight;
            return spotlight.enable && spotlight.shadowCaster.darkness > 0.0f &&
                   AnyDrawnLampCasts(spotlight);
        }

        /// @p src with a `#define` line per space-separated name in
        /// @p defines, inserted after its first line - the `#version` line,
        /// which has to stay first. How the shadow builds its variants of
        /// spotlight.frag: from the light's own SPOTLIGHT_FRAG_SRC at run
        /// time, rather than as sources of their own in shaders.h.in, so a
        /// variant can never miss a chunk someone adds to the light's program.
        std::string WithDefines(const char *src, const char *defines)
        {
            std::string lines;
            const std::string names(defines);
            size_t start = 0;
            while (start < names.size())
            {
                const size_t end = names.find(' ', start);
                const std::string name = names.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!name.empty())
                {
                    lines += "#define " + name + "\n";
                }
                start = (end == std::string::npos) ? names.size() : end + 1;
            }
            std::string out(src);
            const size_t eol = out.find('\n');
            out.insert(eol == std::string::npos ? out.size() : eol + 1, lines);
            return out;
        }
    }

    SpotlightShadow::SpotlightShadow(SpotShadowHost &host)
        : mHost(host)
    {
    }

    bool SpotlightShadow::PinsResolutionScale(const SpotlightConfig &spotlight)
    {
        // The same drawn-lamp test as the light's HasClippedLamp: enable &&
        // intensity > 0, over the first SPOT_MAX_LAMPS entries.
        const int lampCount = std::min(static_cast<int>(spotlight.lamps.size()),
                                       static_cast<int>(SPOT_MAX_LAMPS));
        for (int i = 0; i < lampCount; i++)
        {
            const SpotLight &lamp = spotlight.lamps[static_cast<size_t>(i)];
            if (lamp.castsShadow && lamp.enable && lamp.intensity > 0.0f)
            {
                return true;
            }
        }
        return false;
    }

    void SpotlightShadow::AttachAttributes(const VertexArray &strips, int maxVertices)
    {
        mMaxVertices = std::max(maxVertices, 0);
        // Ceiling-sized once, for the light's own reason: the strips are
        // rebuilt every frame under an animation, and a reallocation per frame
        // is what the ceiling is for. The staging is reserved to match.
        mAttribs.SetData(nullptr, static_cast<size_t>(mMaxVertices) * sizeof(ShadowVertex), GL_DYNAMIC_DRAW);
        mShadowVerts.reserve(static_cast<size_t>(mMaxVertices));
        mCasterRanges.reserve(static_cast<size_t>(SPOT_MAX_LAMPS));

        const GLsizei stride = static_cast<GLsizei>(sizeof(ShadowVertex));
        mAttribs.AttachAttrib(strips, SPOT_SHADOW_ATTRIB_SHADOW, 4, GL_FLOAT, stride, offsetof(ShadowVertex, shadow));
        mAttribs.AttachAttrib(strips, SPOT_SHADOW_ATTRIB_WEIGHT, 1, GL_FLOAT, stride, offsetof(ShadowVertex, weight));
        mAttribs.AttachAttrib(strips, SPOT_SHADOW_ATTRIB_RECESS, 2, GL_FLOAT, stride, offsetof(ShadowVertex, recess));
    }

    void SpotlightShadow::OnStripsBuilt(const SpotlightConfig &spotlight, const std::vector<SpotLampStrip> &strips,
                                        int vertexCount)
    {
        mVertexCount = std::min(std::max(vertexCount, 0), mMaxVertices);
        mCasterRanges.clear();

        // Every vertex starts as a lamp that casts nothing - weight 0, t 0,
        // the half widths floored - so a vertex no strip claims stays finite
        // and inert. resize stays inside the reserve: no allocation.
        ShadowVertex none{};
        none.shadow[3] = MIN_SHADOW_HALF_WIDTH;
        none.recess[1] = MIN_SHADOW_HALF_WIDTH;
        mShadowVerts.assign(static_cast<size_t>(mVertexCount), none);

        // Resolved once for the whole rig, as the light resolves its clip.
        // Nothing narrows the strips to it, and nothing has to: a shadow only
        // removes light from the support the strip was solved for.
        const ShadowSolve shadowSolve = DeriveShadow(spotlight.shadowCaster);

        for (const SpotLampStrip &strip : strips)
        {
            if (strip.lampIndex < 0 || static_cast<size_t>(strip.lampIndex) >= spotlight.lamps.size())
            {
                continue;
            }
            const int first = std::max(strip.firstVertex, 0);
            const int last = std::min(strip.firstVertex + strip.vertexCount, mVertexCount);
            if (last <= first)
            {
                continue;
            }

            // This lamp's share of the caster, constant along its strip like
            // the clip weight.
            ShadowVertex v{};
            v.weight = DeriveLampShadow(shadowSolve, spotlight.lamps[static_cast<size_t>(strip.lampIndex)],
                                        strip.apertureWidth, v.shadow, v.recess);
            std::fill(mShadowVerts.begin() + first, mShadowVerts.begin() + last, v);

            // A casting lamp's strip joins the darkening pass's runs, merged
            // with the run before it when they touch.
            if (v.weight > 0.0f)
            {
                const int count = last - first;
                if (!mCasterRanges.empty() &&
                    mCasterRanges.back().first + mCasterRanges.back().count == first)
                {
                    mCasterRanges.back().count += count;
                }
                else
                {
                    mCasterRanges.push_back(CasterRange{first, count});
                }
            }
        }

        if (mVertexCount > 0)
        {
            mAttribs.SetSubData(0, mShadowVerts.size() * sizeof(ShadowVertex), mShadowVerts.data());
        }
    }

    void SpotlightShadow::OnConfigChanged(const Config &config)
    {
        // Viewport-sized, so not one to hold through a session that turned the
        // fill off. Pre-Initialize, Release no-ops on the buffer it finds
        // unallocated.
        if (!UsesFillPass(config))
        {
            mFillBuffer.Release();
        }
    }

    void SpotlightShadow::UploadUniforms(ShaderProgram &program, const Config &config)
    {
        // On the same terms as the clip area: app coordinates, uploaded
        // unconditionally, inert where no lamp's weight reads it. Only the
        // caster's shape is here - the rest of it (distance, softness,
        // opacity) is folded per lamp into the attributes, and the fill's and
        // the darkening's own uniforms are set by the passes that read them.
        const ShadowSolve shadowSolve = DeriveShadow(config.spotlight.shadowCaster);
        program.SetUniform("uShadowRect",
                           glm::vec4(shadowSolve.center.x, shadowSolve.center.y,
                                     shadowSolve.half.x, shadowSolve.half.y));
        program.SetUniform("uShadowCorner", shadowSolve.radius);

        // The solid block and the frame. Uniform branches in the shader, so a
        // panel without a frame runs exactly the panel test. The softness
        // rides here as well as folded into the per-lamp half widths because
        // the solid search rebuilds its window at every sample.
        program.SetUniform("uShadowSolid", shadowSolve.solid ? 1.0f : 0.0f);
        program.SetUniform("uShadowSoftness", shadowSolve.softness);
        program.SetUniform("uShadowInner",
                           glm::vec4(shadowSolve.innerHalf.x, shadowSolve.innerHalf.y,
                                     shadowSolve.innerRadius, shadowSolve.hasRecess ? 1.0f : 0.0f));
    }

    void SpotlightShadow::RenderOffscreen(int viewportWidth, int viewportHeight, const Config &config)
    {
        // A new frame. What phase 2 reads of it is set below, and stays "no
        // fill" on every early return.
        mFrame = FrameState{};
        mFrame.prepared = true;
        mFrame.viewport = glm::ivec2(viewportWidth, viewportHeight);

        // Never alongside the light's scaled path - UsesFillPass needs a
        // casting lamp, and that pins the scale - and never without its
        // program: a variant that failed to compile leaves a filled frame
        // drawing the physical shadow, which is the right thing to degrade to.
        if (mVertexCount <= 0 || viewportWidth <= 0 || viewportHeight <= 0 ||
            !UsesFillPass(config) || !ensureFillProgram())
        {
            return;
        }

        // The render target the renderer was handed - framebuffer AND
        // viewport, saved as a pair because this pass has to put both back.
        // Not always the window's: an offscreen frame capture binds a real
        // FBO. Same shape, same reasoning, as the light's scaled path.
        const RenderTargetState prevTarget = RenderTargetState::Capture();

        // The fill buffer has to hold every lamp's light, wherever a host
        // scissor box is - see GLUtils::NoScissorScope. The host's clip is
        // back when this returns, before the passes on the caller's target.
        GLUtils::NoScissorScope noScissor(true);

        if (!bindFillTarget(viewportWidth, viewportHeight))
        {
            return;
        }

        // THE FILL'S FIRST PASS: every lamp, shadows applied, screened - how
        // much light reaches each pixel. The light's own program and blend,
        // so this is exactly the light the caller's target would get.
        mHost.UseLightBlend();
        mHost.DrawLamps(viewportWidth, viewportHeight, config);

        prevTarget.Restore();
        mFrame.fillReady = true;

        // The blend state a Render leaves, for the layers that composite
        // before this one: glBlendFunc sets the RGB and alpha factors to the
        // same pair, so it also undoes the light's split.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void SpotlightShadow::RenderBeforeLight(int viewportWidth, int viewportHeight, const Config &config)
    {
        if (mCasterRanges.empty() || !UsesDarkenPass(config))
        {
            return;
        }
        // Its programs are built the first frame they are wanted: most hosts
        // never set ShadowCaster::darkness, and should not pay two compiles.
        ensureDarkenPrograms();

        // A filled frame darkens through the variant that reads the fill
        // buffer, so the fill lifts the darkening with the shadow.
        const bool withFill = UsesFillPass(config) && ensureFillProgram() &&
                              isFillReady(viewportWidth, viewportHeight, config);
        ShaderProgram &program = withFill ? mDarkenFillShader : mDarkenShader;
        if (!program.IsValid())
        {
            return;
        }

        // Premultiplied OVER, colour and alpha alike: dst * (1 - D) + colour * D,
        // and the alpha gains D. The program writes (colour * D, D), and an
        // exact 0 outside every shadow, which this blend leaves bit for bit.
        // One colour for every lamp, so the chain of OVERs commutes. Left set:
        // the caller sets the light's blend after this.
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        program.Use();
        mHost.UploadStripUniforms(program, viewportWidth, viewportHeight, config);
        if (withFill)
        {
            uploadFillUniforms(program, config, viewportHeight);
        }
        const ShadowCaster &caster = config.spotlight.shadowCaster;
        program.SetUniform("uShadowDarkness", std::min(std::max(caster.darkness, 0.0f), 1.0f));
        program.SetUniform("uShadowColor", glm::clamp(caster.color, glm::vec3(0.0f), glm::vec3(1.0f)));
        for (const CasterRange &range : mCasterRanges)
        {
            mHost.DrawStrips(range.first, range.count);
        }
        program.Unuse();
    }

    bool SpotlightShadow::RenderLight(int viewportWidth, int viewportHeight, const Config &config)
    {
        const bool fill = UsesFillPass(config) && ensureFillProgram() &&
                          isFillReady(viewportWidth, viewportHeight, config);
        // This frame is over either way.
        mFrame.prepared = false;
        if (!fill)
        {
            return false;
        }

        // THE FILL'S SECOND PASS. RenderOffscreen's went into mFillBuffer:
        // every lamp, shadows applied, screened. Now the same strips again,
        // onto the caller's target this time, through the SPOT_FILL variant,
        // which reads that buffer to find how much of each pixel's light came
        // from the OTHER lamps and lifts this lamp's shadow by it. See THE
        // FILL VARIANT in spotlight-shadow.glsl.
        //
        // Two programs, one per target, rather than one program drawing
        // both: the plain one draws the buffer and the variant draws the
        // caller's framebuffer, in the same blend state either way. Same
        // split NeonRenderer keeps between its shade and ring programs.
        //
        // The light's blend set HERE, although the caller has just set it:
        // when nothing ran RenderOffscreen this frame, isFillReady above ran
        // it, and it leaves the library's default blend behind it.
        mHost.UseLightBlend();
        mFillShader.Use();
        mHost.UploadStripUniforms(mFillShader, viewportWidth, viewportHeight, config);
        uploadFillUniforms(mFillShader, config, viewportHeight);
        mHost.DrawStrips(0, mVertexCount);
        mFillShader.Unuse();
        return true;
    }

    bool SpotlightShadow::ensureFillProgram()
    {
        if (!mFillProgramTried)
        {
            mFillProgramTried = true;
            const std::string fillSource = WithDefines(ShaderSource::SPOTLIGHT_FRAG_SRC, "SPOT_FILL");
            mFillShader = ShaderProgram(ShaderSource::SPOTLIGHT_VERT_SRC, fillSource.c_str(),
                                        "SpotlightRenderer.Fill");
            // Optional on purpose: a fill is a look on top of a working layer,
            // so a driver that rejects it costs the look, not the lamps.
            if (!mFillShader.IsValid())
            {
                LOG_E("SpotlightRenderer: the fill variant failed to compile/link - "
                      "ShadowCaster::fill will draw the physical shadow.");
            }
        }
        return mFillShader.IsValid();
    }

    void SpotlightShadow::ensureDarkenPrograms()
    {
        if (mDarkenProgramsTried)
        {
            return;
        }
        mDarkenProgramsTried = true;

        const std::string darkenSource = WithDefines(ShaderSource::SPOTLIGHT_FRAG_SRC, "SPOT_SHADOW_DARKEN");
        mDarkenShader = ShaderProgram(ShaderSource::SPOTLIGHT_VERT_SRC, darkenSource.c_str(),
                                      "SpotlightRenderer.Darken");
        const std::string darkenFillSource =
            WithDefines(ShaderSource::SPOTLIGHT_FRAG_SRC, "SPOT_SHADOW_DARKEN SPOT_FILL");
        mDarkenFillShader = ShaderProgram(ShaderSource::SPOTLIGHT_VERT_SRC, darkenFillSource.c_str(),
                                          "SpotlightRenderer.DarkenFill");
        if (!mDarkenShader.IsValid() || !mDarkenFillShader.IsValid())
        {
            LOG_E("SpotlightRenderer: a darkening variant failed to compile/link - "
                  "ShadowCaster::darkness will draw the light-only shadow.");
        }
    }

    bool SpotlightShadow::isFillReady(int viewportWidth, int viewportHeight, const Config &config)
    {
        // The offscreen phase for this frame, if nothing ran it - a host that
        // drives the renderer without EdgeLightingEffect, which runs every
        // layer's RenderOffscreen before any layer's Render.
        if (!mFrame.prepared || mFrame.viewport != glm::ivec2(viewportWidth, viewportHeight))
        {
            RenderOffscreen(viewportWidth, viewportHeight, config);
        }
        return mFrame.fillReady;
    }

    bool SpotlightShadow::bindFillTarget(int width, int height)
    {
        // The fill reads its buffer with texelFetch, one texel per app px,
        // so it is the viewport's size and NEAREST.
        //
        // Resize destroys the attachment on its failure path, so a failure
        // would leave the buffer holding id 0 and Bind would then bind the
        // CALLER'S framebuffer - with ClearBuffer about to wipe it. Report it
        // instead: nothing has been drawn.
        if (!mFillBuffer.Resize(width, height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST))
        {
            return false;
        }
        // Bind, then clear to transparent black. Keep the two adjacent:
        // ClearBuffer acts on whatever is BOUND.
        mFillBuffer.Bind();
        mFillBuffer.ClearBuffer();
        return true;
    }

    void SpotlightShadow::uploadFillUniforms(ShaderProgram &program, const Config &config, int viewportHeight)
    {
        mFillBuffer.BindTexture(0);
        program.SetUniform("uFillSource", 0);
        program.SetUniform("uFillAmount",
                           std::min(std::max(config.spotlight.shadowCaster.fill, 0.0f), 1.0f));
        program.SetUniform("uFillHeight", static_cast<float>(viewportHeight));
    }
}
