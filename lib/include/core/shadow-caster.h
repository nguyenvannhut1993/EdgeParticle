#ifndef _EDGE_LIGHTING_SHADOW_CASTER_H_
#define _EDGE_LIGHTING_SHADOW_CASTER_H_

#include <glm/glm.hpp>

// The spotlight shadow caster's configuration - SpotlightConfig::shadowCaster.
//
// Its own header rather than a block of config.h for the reason the whole
// shadow is its own module (renderer/spotlight-shadow.h): the light's files
// are edited by several people, and the shadow should not move with them.
// config.h includes it, so every host still sees ShadowCaster through
// core/config.h. What the shadow adds to config.h itself is two fields,
// SpotLight::castsShadow and SpotlightConfig::shadowCaster, and their lines in
// operator==. The reference is docs/guides/spotlight-shadow.md.

namespace EdgeLighting
{
    /// A flat rounded-rectangle picture hung IN FRONT of the wall the lamps
    /// light, in APP coordinates - the thing that throws a shadow.
    ///
    /// The spotlight layer draws its cones on one plane, the wall. This puts a
    /// second plane between the wall and the lamps: the caster's face sits
    /// @c wallDistance px off the wall, and every lamp opted in with
    /// @c SpotLight::castsShadow is taken to stand SPOT_LAMP_WALL_DISTANCE
    /// (150) px off it - one distance for the whole rig, because only the
    /// ratio of the two reaches the picture (see @ref wallDistance). A
    /// fragment on the wall is in shadow when the straight line from it to the
    /// lamp crosses the caster - so the shadow is the caster's rectangle
    /// scaled up about the lamp's position by `150 / (150 - wallDistance)`,
    /// pushed AWAY from the lamp. A lamp above a picture puts a thick band under it
    /// and thin bands down both sides, and nothing above it, because the top
    /// of the shadow lands behind the picture itself.
    ///
    /// The face of the caster is in front of the plane it blocks, so it stays
    /// LIT: the light is removed only from the wall around it. That is what
    /// lets a host draw its picture into this rectangle, under the effect, and
    /// get both a lit picture and its shadow from one layer.
    ///
    /// **LIGHT ONLY by default, like the rest of the layer.** A shadow here is
    /// this layer's own light taken away - a coverage multiply on the lamp's
    /// shading, as @ref ClipArea is - not a darkening of what the host drew.
    /// Over a dark backdrop that reads exactly as a shadow; over a backdrop
    /// already brighter than the lamp, there is little light to take away and
    /// so little shadow to see. @ref darkness is the opt-in that darkens the
    /// backdrop as well.
    ///
    /// **Inert by default.** A 0 x 0 caster, or one at @c wallDistance 0 (flush
    /// with the wall, where its shadow lies entirely behind it), blocks nothing
    /// - unlike a zero-size @c ClipMode::KEEP_INSIDE area, which blanks a lamp.
    /// So opting a lamp in before the rectangle is set changes nothing.
    ///
    /// Its own rectangle rather than @c Config::geometry, for the reason
    /// @ref ClipArea gives: the spotlight layer reads nothing but
    /// @c Config::spotlight. A host that wants the shadow under its rect copies
    /// the numbers across.
    typedef struct ShadowCaster
    {
        /// TOP-LEFT corner in APP coordinates - the same space as
        /// @c RectGeometry::position and @c SpotLight::position.
        glm::vec2 position = glm::vec2(0.0f, 0.0f);
        float width = 0.0f;  ///< Size in px. Zero on either axis casts nothing.
        float height = 0.0f; ///< See @ref width.
        /// Corner rounding in px, clamped by the renderer to half the shorter
        /// side. The shadow is rounded to match.
        float cornerRadius = 0.0f;

        /// How far the caster's face stands off the wall, in px - the depth
        /// that turns a flat rectangle into something with a shadow, and the
        /// ONE distance the shadow has. 0 is flush and casts nothing visible.
        ///
        /// It is measured against lamps SPOT_LAMP_WALL_DISTANCE (150) px off
        /// the wall, the same for every lamp, as `t = wallDistance / 150`:
        /// the shadow is the caster scaled by `1 / (1 - t)` about each
        /// casting lamp's position, and its penumbra is
        /// `2 * apertureWidth * t / (1 - t)` px wide plus @ref softness. So a
        /// larger value throws the shadow further and softens its edge. Only
        /// that ratio reaches the picture - scaling every distance by one
        /// factor changes nothing - which is why the lamps carry no distance
        /// of their own.
        ///
        /// As it nears 150 a PANEL's shadow grows out of view, and at or
        /// past it the panel casts nothing (the lamps are behind its face).
        /// A SOLID caster that deep throws a shadow that never ends: the
        /// lamps graze its side, and light along the wall stops at the block
        /// - except a lamp inside the block's footprint, which casts nothing.
        float wallDistance = 0.0f;

        /// Extra feather across the shadow's edge, in px ON THE WALL, on top
        /// of the penumbra the lamp's own size produces. 1.0 - the default -
        /// is a single pixel, enough to antialias a lamp small enough to cast
        /// a hard shadow. Larger values are a look.
        float softness = 1.0f;

        /// Fraction of a lamp's light the caster stops, in [0, 1]. 1 is an
        /// opaque picture; below that some light reaches the wall behind it -
        /// a translucent panel, or a way to keep the shadow from going all the
        /// way down to the bare backdrop.
        float opacity = 1.0f;

        /// How far the OTHER lamps' light washes a lamp's shadow out, in
        /// [0, 1]. **0 - the default - is physical**: a point in lamp A's
        /// shadow receives exactly the light of every other lamp that reaches
        /// it, so where lamp B is weaker than A the shadow still shows,
        /// lighter. Above 0 this is a LOOK, not physics: A's shadow fades
        /// wherever the other lamps put light on it, and at 1 it is gone
        /// wherever they reach half of what A would have put there
        /// (SPOT_FILL_FULL_RATIO).
        ///
        /// It never fills a shadow with the shadowed lamp's own light
        /// alone - a lamp cannot wash out its own penumbra - and it only
        /// counts light that actually REACHES the point: a second lamp that is
        /// itself blocked there fills nothing.
        ///
        /// **Costs a second pass** while above 0 and a lamp casts a shadow:
        /// the lamps are drawn once into a viewport-sized RGBA8 buffer to find
        /// how much light reaches each point, then again onto the target
        /// reading it - every strip rasterised twice, plus clearing and
        /// holding that buffer. At 0, or with no lamp casting, there is no
        /// second pass and no buffer.
        float fill = 0.0f;

        /// How far the shadow darkens what is BEHIND this layer, in [0, 1].
        /// **0 - the default - is light only**: a shadow is just the lamp's
        /// light taken away, so over a backdrop brighter than the lamp it all
        /// but disappears (5 levels over 0.85 grey where it is 28 over 0.1),
        /// and over a transparent surface the umbra writes alpha 0, so a
        /// compositor shows what is behind it at full brightness.
        ///
        /// Above 0 each casting lamp also darkens the destination inside its
        /// shadow, BEFORE the light is screened on, by
        /// `darkness * (1 - visibility) * reach`: reach rises from 0 where the
        /// lamp puts no light to 1 where it puts SPOT_SHADOW_FULL_REACH of
        /// full scale, so there is no shadow where there is no light and the
        /// shadow fades out with the beam. It is written as premultiplied
        /// @ref color over the destination, alpha included, so the umbra
        /// carries coverage and a video plane behind the surface darkens too.
        /// The other lamps' light still lands on top, and @ref fill lifts the
        /// darkening with the shadow. It darkens whatever the framebuffer
        /// already holds there.
        ///
        /// Costs one more pass over the casting lamps' strips while above 0.
        float darkness = 0.0f;

        /// The colour @ref darkness paints the shadow with, linear RGB in
        /// [0, 1]. Black - the default - scales the backdrop down; a dark warm
        /// tone reads as a shadow on a lit warm wall. Exact under a
        /// premultiplied compositor; black is exact under any.
        glm::vec3 color = glm::vec3(0.0f, 0.0f, 0.0f);

        /// Whether the caster is a SOLID BLOCK from the wall out to
        /// @c wallDistance - a framed picture - rather than a flat panel
        /// floating that far in front of it. Default false: the panel.
        ///
        /// A block's shadow is everything its whole depth projects, so it is
        /// attached to the caster (under a lamp above it, the side shadows
        /// start at its top corners and widen downward) where a panel's is a
        /// detached copy. And a block at least SPOT_LAMP_WALL_DISTANCE deep -
        /// as deep as the lamps are far from the wall - throws a shadow that
        /// never ends: light along the wall stops at the block, where a
        /// panel that deep casts nothing. The caster's face stays lit either
        /// way. See
        /// docs/design/spotlight-solid-frame-plan.md.
        ///
        /// Costs a short fixed search per fragment of each casting lamp
        /// (SPOT_SOLID_SEARCH_STEPS signed-distance evaluations) instead of
        /// the panel's one.
        bool solid = false;

        /// Width of a FRAME's rails, in px. The picture is the caster's
        /// rectangle inset by this much, and with @c frameDepth above 0 the
        /// rails shade it. 0 - the default - is no frame. Clamped to the
        /// shorter half side.
        float frameWidth = 0.0f;

        /// How far the picture sits behind the rails' front, in px. Lit from
        /// above, the top rail then casts a band of shadow across the top of
        /// the picture, deeper as this grows. 0 -
        /// the default - is flush: no rail shadow. Clamped to
        /// @c wallDistance. Works for a panel or a block.
        float frameDepth = 0.0f;

        bool operator==(const ShadowCaster &o) const
        {
            return position == o.position &&
                   width == o.width &&
                   height == o.height &&
                   cornerRadius == o.cornerRadius &&
                   wallDistance == o.wallDistance &&
                   softness == o.softness &&
                   opacity == o.opacity &&
                   fill == o.fill &&
                   darkness == o.darkness &&
                   color == o.color &&
                   solid == o.solid &&
                   frameWidth == o.frameWidth &&
                   frameDepth == o.frameDepth;
        }
        bool operator!=(const ShadowCaster &o) const { return !(*this == o); }
    } ShadowCaster;
}

#endif // _EDGE_LIGHTING_SHADOW_CASTER_H_
