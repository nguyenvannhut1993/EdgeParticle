precision highp float;

// ===========================================================================
// THE SHADOW - everything about SpotlightConfig::shadowCaster on the GPU's
// fragment side: the model, the per-lamp varyings, the caster's uniforms,
// the functions and the SPOT_FILL and SPOT_SHADOW_DARKEN variants.
//
// Injected into spotlight.frag via @SPOTLIGHT_SHADOW@ in shaders.h.in, after
// @SPOTLIGHT_SHADOW_TUNING@ and @SPOTLIGHT_DITHER@ and AHEAD of the file's own
// text - so it declares its own precision, and it reaches nothing of
// spotlight.frag's but sdRoundBox, through the prototype below. The fragment's
// app position arrives as a parameter rather than as vApp for the same
// reason. spotlight.frag's main() calls into it at ONE place, right after the
// clip: spotShadowLight, or spotShadowDarken in the darkening variant. Not a
// shader: no main().
//
// Its own file, like the rest of the shadow, so the light's shader can be
// edited without touching it: the vertex half is spotlight-shadow-vert.glsl,
// the constants spotlight-shadow-tuning.h, the CPU half SpotlightShadow
// (spotlight-shadow.{h,cpp}). docs/guides/spotlight-shadow.md is the
// reference; docs/design/spotlight-shadow-plan.md and
// docs/design/spotlight-solid-frame-plan.md are the measured record.
// ===========================================================================
//
// THE SHADOW, and why it is a line test rather than anything that looks at the
// scene. A lamp with SpotLight::castsShadow set is blocked by one flat rounded
// rectangle, SpotlightConfig::shadowCaster, standing off the wall. Put the
// wall at z = 0, the caster's face at z = d and the lamp at z = h over its app
// position L - h is SPOT_LAMP_WALL_DISTANCE, the same for every lamp, since
// only d / h reaches the picture: the line from a wall fragment p to the lamp
// crosses the caster's plane at
//
//   q = mix(p, L, t),   t = d / h
//
// and p is in shadow exactly when q is inside the caster. The CPU folds d and
// h into t per lamp (vShadow.z), so the fragment pays one mix and one
// rounded-box SDF. The shadow that falls out is the caster scaled by
// 1 / (1 - t) about L - pushed AWAY from the lamp, which is the photograph
// this was built against: a band under a picture lit from above, thinner bands
// down both sides, nothing over the top.
//
// The penumbra is the lamp's own size. Take the emitter as a disc of radius
// apertureWidth: seen from p it projects onto the caster's plane as a disc of
// radius apertureWidth * t about q, and the fraction of that disc the caster
// covers is the fraction of light p loses. smoothstep across the SDF stands in
// for that fraction - exact where the edge crosses the disc's centre, close
// elsewhere - over the half width vShadow.w, which the CPU computes as
// apertureWidth * t plus half of ShadowCaster::softness carried from the wall
// back to the plane (one wall px is (1 - t) plane px).
//
// The caster's FACE is in front of the plane it blocks, so a fragment on the
// face is lit: the picture a host draws into that rectangle takes the light,
// and only the wall around it loses it. Hence the second SDF, at p itself, and
// the `1 - blocked * (1 - onFace)`.
//
// Like the clip it is COVERAGE on the finished shading - after the shoulder,
// before the coverage alpha and the dither - for every reason the clip's block
// gives: what survives is the unshadowed light times a fraction, a fully
// shadowed fragment records no coverage, and the dither cannot round it back
// up. And it only ever REMOVES light, which is why the strip bound
// SpotlightRenderer solves needs nothing new.
//
// THE SOLID BLOCK AND THE FRAME (docs/design/spotlight-solid-frame-plan.md). With
// ShadowCaster::solid the caster is not a panel at depth d but a block filling
// everything from the wall to d, so p is in shadow when the line to the lamp
// meets the caster ANYWHERE along the block's depth - the whole segment from p
// to mix(p, L, t), t = min(d / h, 1), not just its far end. Its soft edge is
// the minimum along that segment of
//
//   u(s) = sd(mix(p, L, s)) / (apertureWidth * s + softness / 2 * (1 - s))
//
// - the caster's SDF over the emitter's window at that point, which grows from
// the feather at the wall to the panel's half width at s = t. So far edges
// match the panel, and the shadow is sharp where it meets the block. sd is
// convex along a line and the window is linear, so u is quasiconvex: one
// minimum, found by a fixed golden-section search (SPOT_SOLID_SEARCH_STEPS).
// A minimum at s = 0 means the line leaves the block straight away - p is on
// the lit side - and is read as fully lit; taken at face value, the feather
// there would draw a dark outline round the lit side of the block.
//
// ShadowCaster::frameWidth / frameDepth make the caster a FRAME: rails of that
// width, around a picture recessed that far behind their front. A picture
// fragment is lit when the line to the lamp clears the rails, i.e. when
// mix(p, L, t_in) is still inside the picture's opening, t_in = frameDepth /
// (h - picture depth) from the CPU (vRecess.x). The opening is convex and
// contains p, so the end point decides it. The rails' front itself is lit
// wherever the cone reaches, like the panel's face.
//
// THE PICTURE'S LIGHT (ShadowCaster::lightOnPicture), a look. This layer
// SCREENS its light over what the host drew, so a bright lamp lifts every dark
// part of a picture towards the lamp's colour and the picture washes out. For
// a lamp the CPU opts in (vPictureWeight: castsShadow, the option below 1, a
// caster of some size), two things change on the FACE - the picture and its
// frame, the rectangle the host draws into - and nothing else does:
//
//   - the LIGHT the lamp shows there is scaled by uPictureLight, after the
//     shadow (spotFaceGain);
//   - the rails' shadow on the recessed picture, the one shadow that falls on
//     the face, DARKENS the picture with strength mix(1, darkness,
//     uPictureLight) (spotShadowStrength): the wall's darkness at 1, all of
//     the lamp's share at 0. A shadow that only took light away would vanish
//     with the light; this one stays.
//
// So at 0 the face shows exactly what the host drew, darkened where the rails
// shade it. Everything the shadow is made of - the mask, the fill's lift, the
// darkening's reach - is still computed from the lamp's REAL light, which is
// why SpotlightShadow::RenderOffscreen draws the fill's source buffer with
// uPictureLight at 1: the option decides only how much of that light the face
// shows and how the face's shadow is drawn. A weight of its own, apart from
// vShadowWeight, because dimming the light needs no caster that casts: a
// flush or transparent caster still keeps its picture out of the light.
//
// THE FILL VARIANT (SPOT_FILL), which is a look and not physics. With two
// lamps, a point in lamp A's shadow receives exactly lamp B's light, so where
// B is weaker than A the shadow still shows, lighter. ShadowCaster::fill
// washes it out instead. A copy of the plain program has already drawn every
// lamp - shadows applied - into uFillSource, so each fragment can read how much
// light reaches its pixel (R) and take its own share back out of the screen to
// find what the OTHER lamps put there:
//
//   others = 1 - (1 - R) / (1 - own),   own = this lamp's light after its shadow
//
// The shadow's coverage is then lifted towards 1 by
// uFillAmount * smoothstep(0, SPOT_FILL_FULL_RATIO, ratio), ratio =
// peak(others) / peak(what this lamp would have put here unshadowed): gone
// where the others reach half of it (see spotlight-shadow-tuning.h for why half),
// untouched where they put nothing. The lamp's own light is excluded by
// construction, which is why this cannot be done on the composite: from R
// alone, a lamp's own penumbra looks like light from elsewhere, and filling
// with it would shrink every shadow.
//
// This program runs AFTER the light, as a CORRECTION, so the light's own draw
// stays as it is: the light has screened y = lit * cover, and this screens on
//
//   x = (z - y) / (1 - y),   z = lit * lifted cover
//
// which the screen, being associative, adds up to z exactly: 1 - (1 - y)(1 - x)
// = z. y and z are lit times scalars, so x is largest in lit's largest
// channel, and spotlight.frag's max-channel coverage alpha is the same
// correction for the alpha. Outside every shadow, and for a lamp that casts
// nothing, x is exactly 0, which the half-step dither cannot round up: the
// light pass's output stands.
//
// THE DARKENING VARIANT (SPOT_SHADOW_DARKEN), ShadowCaster::darkness. Light
// only, a shadow is this lamp's light taken away, and the screen leaves
// little light to take over a bright backdrop: the contrast is
// L * (1 - backdrop), and the umbra writes alpha 0. This program runs
// FIRST, over the casting lamps' strips only, and writes no light at all -
// it writes the shadow, as premultiplied colour over the destination:
//
//   D = darkness * (1 - cover) * smoothstep(0, SPOT_SHADOW_FULL_REACH, peak(lit))
//   out = (uShadowColor * D, D)   under GL_ONE / GL_ONE_MINUS_SRC_ALPHA
//
// so the destination becomes dst * (1 - D) + colour * D, colour and alpha
// alike: the backdrop darkens under the shadow and the umbra records
// coverage. (On a picture kept out of the light, darkness rises - see THE
// PICTURE'S LIGHT; this pass then runs whatever darkness is.) `cover` is the
// light pass's own (fill lift included under SPOT_FILL), and `lit` the
// lamp's unshadowed light - no light, no shadow,
// and the shadow fades out with the beam inside the strip, which is what
// keeps the strip bound free (see SPOT_SHADOW_FULL_REACH). One colour for
// every lamp makes the chain of OVERs commutative, so lamp order cannot
// change it. The light pass then screens every lamp on top, so another
// lamp's light still lands inside the shadow. Outside a shadow D is an
// exact 0 and the fragment writes an exact 0 - the blend leaves the
// destination bit for bit.

/// Lamp app position (xy), t = caster / lamp wall distance (z), penumbra half
/// width on the caster's plane in px (w, never 0 - the CPU floors it).
flat in vec4 vShadow;
flat in float vShadowWeight; ///< ShadowCaster::opacity if this lamp casts a shadow, else 0.
/// 1 if ShadowCaster::lightOnPicture scales this lamp's light on the picture,
/// else 0 - see THE PICTURE'S LIGHT above.
flat in float vPictureWeight;
/// The rails over the recessed picture: t_in (x) and the penumbra half width
/// at the rails' front in px (y, never 0).
flat in vec2 vRecess;

/// spotlight.frag's rounded-box SDF, declared here and DEFINED there: this
/// chunk lands ahead of that file's text, and a prototype lets it use the one
/// copy rather than carry a second. Rename it there and this stops linking,
/// which is the loud failure wanted.
float sdRoundBox(vec2 p, vec2 b, float r);

/// The shadow caster, in APP coordinates: centre.xy, half extent.xy - the same
/// form as uClipRect, collapsed on the CPU for the same reason.
uniform vec4 uShadowRect;

/// The caster's corner radius in px, already clamped to the shorter half
/// extent.
uniform float uShadowCorner;

/// 1 for a solid block, 0 for a panel. A uniform branch.
uniform float uShadowSolid;

/// ShadowCaster::softness in wall px. The solid search rebuilds its window at
/// every sample, so it needs the feather itself, not a per-lamp result.
uniform float uShadowSoftness;

/// The picture inside the rails: half extent (xy), corner radius (z), and 1
/// when the rails shade it (w) - a uniform branch. Centred on uShadowRect.xy.
/// Without rails xyz is the face itself.
uniform vec4 uShadowInner;

/// ShadowCaster::lightOnPicture, clamped to [0, 1]: the fraction of an
/// opted-in lamp's light the face shows. 1 while the fill's source buffer is
/// drawn, which has to hold the real light.
uniform float uPictureLight;

#ifdef SPOT_FILL
/// What the fill's source pass drew: every lamp, shadows applied, screened
/// into a buffer the size of the viewport under the same y-flipped ortho - so
/// app px (x, y) is texel (x, uFillHeight - y).
uniform sampler2D uFillSource;
/// ShadowCaster::fill, clamped to [0, 1].
uniform float uFillAmount;
/// Height of uFillSource in texels, for the y flip.
uniform float uFillHeight;
#endif

/// u(s) of the solid search: the caster's SDF at the point a fraction @p s of
/// the way from @p p to @p lamp, over the emitter's window there - @p feather
/// at the wall, @p aperture * s once the window is the emitter's projection.
float spotSolidRatio(vec2 p, vec2 lamp, float s, float aperture, float feather) {
    float d = sdRoundBox(mix(p, lamp, s) - uShadowRect.xy, uShadowRect.zw, uShadowCorner);
    return d / max(aperture * s + feather * (1.0 - s), 1.0e-4);
}

/// Visibility of wall point @p p past a SOLID caster, for a lamp at @p lamp
/// whose line is inside the block for s in [0, @p tMax] - see THE SOLID BLOCK
/// at the top of this section.
///
/// Golden-section search for the minimum of the quasiconvex u(s): a FIXED
/// number of steps, so no loop runs longer for some fragments than others.
/// The far end is checked separately (the search never lands on it exactly,
/// and it is where the panel's edge is), and a minimum no lower than u(0) -
/// the line leaving the block at once - is read as fully lit.
float spotSolidWall(vec2 p, vec2 lamp, float tMax, float aperture) {
    const float INV_PHI = 0.6180340;
    float feather = max(0.5 * uShadowSoftness, 1.0e-4);

    // EARLY OUT, and an exact one. The caster lies inside the circle through
    // its corners, so its SDF anywhere on the segment is at least the
    // segment's clearance from that circle; the window is linear in s, so it
    // is never wider than at one end. Clearance >= the wider end puts every
    // u(s) at or above 1, where the smoothstep below is already exactly 1 -
    // so this returns what the search would, for most of the lamp's strip, at
    // the price of one projection. Measured: see the cost section of
    // docs/design/spotlight-solid-frame-plan.md.
    vec2 toFar = mix(p, lamp, tMax) - p;
    vec2 toCentre = uShadowRect.xy - p;
    float along = clamp(dot(toCentre, toFar) / max(dot(toFar, toFar), 1.0e-6), 0.0, 1.0);
    float clearance = length(toCentre - toFar * along) - length(uShadowRect.zw);
    if (clearance >= max(aperture * tMax + feather * (1.0 - tMax), feather)) {
        return 1.0;
    }

    float lo = 0.0;
    float hi = tMax;
    float s1 = hi - INV_PHI * (hi - lo);
    float s2 = lo + INV_PHI * (hi - lo);
    float u1 = spotSolidRatio(p, lamp, s1, aperture, feather);
    float u2 = spotSolidRatio(p, lamp, s2, aperture, feather);
    for (int i = 0; i < SPOT_SOLID_SEARCH_STEPS; i++) {
        if (u1 < u2) {
            hi = s2;
            s2 = s1;
            u2 = u1;
            s1 = hi - INV_PHI * (hi - lo);
            u1 = spotSolidRatio(p, lamp, s1, aperture, feather);
        } else {
            lo = s1;
            s1 = s2;
            u1 = u2;
            s2 = lo + INV_PHI * (hi - lo);
            u2 = spotSolidRatio(p, lamp, s2, aperture, feather);
        }
    }
    float uMin = min(min(u1, u2), spotSolidRatio(p, lamp, tMax, aperture, feather));
    if (uMin >= spotSolidRatio(p, lamp, 0.0, aperture, feather)) {
        return 1.0;
    }
    return smoothstep(-1.0, 1.0, uMin);
}

/// Light the shadow caster leaves at @p app for one lamp, in [0, 1] - see THE
/// SHADOW and THE SOLID BLOCK at the top of this section.
///
/// Called only through spotShadowCover, so only for a lamp whose weight is
/// above 0: with the solid search in it this is a dozen SDFs, and a lamp that
/// casts nothing has no use for any of them. The CPU still writes
/// well-formed values for such lamps - t 0 and floored half widths - so a
/// smoothstep with equal edges cannot produce a NaN if one ever is evaluated.
float spotShadowMask(vec2 app, vec4 shadow, vec2 recess, float aperture) {
    // THE WALL around the caster.
    float wallVis;
    if (uShadowSolid > 0.5) {
        wallVis = spotSolidWall(app, shadow.xy, shadow.z, aperture);
    } else {
        // Where the line to the lamp crosses the caster's plane.
        vec2 onPlane = mix(app, shadow.xy, shadow.z);
        float dRay = sdRoundBox(onPlane - uShadowRect.xy, uShadowRect.zw, uShadowCorner);
        wallVis = smoothstep(-shadow.w, shadow.w, dRay);
    }

    // THE FACE: lit, except a recessed picture where the rails hide the lamp.
    float faceVis = 1.0;
    if (uShadowInner.w > 0.5) {
        float dPicture = sdRoundBox(app - uShadowRect.xy, uShadowInner.xy, uShadowInner.z);
        float onPicture = 1.0 - smoothstep(-0.5, 0.5, dPicture);
        vec2 atRails = mix(app, shadow.xy, recess.x);
        float dOpening = sdRoundBox(atRails - uShadowRect.xy, uShadowInner.xy, uShadowInner.z);
        faceVis = mix(1.0, 1.0 - smoothstep(-recess.y, recess.y, dOpening), onPicture);
    }

    // Which of the two this fragment is, one pixel of feather: the face is the
    // picture the host drew, and its edge is that picture's edge, not a
    // penumbra.
    float dFace = sdRoundBox(app - uShadowRect.xy, uShadowRect.zw, uShadowCorner);
    float onFace = 1.0 - smoothstep(-0.5, 0.5, dFace);

    return mix(wallVis, faceVis, onFace);
}

/// How much of an opted-in lamp's light the face shows at app px @p app:
/// uPictureLight on the face, 1 off it, across the face's own one-pixel edge -
/// see THE PICTURE'S LIGHT above. Written so uPictureLight 1 gives exactly 1,
/// which is how the fill's source pass draws the real light. Called only for a
/// lamp whose vPictureWeight is above 0, so a lamp it does not apply to pays
/// nothing and stays bit for bit what it was.
float spotFaceGain(vec2 app) {
    float dFace = sdRoundBox(app - uShadowRect.xy, uShadowRect.zw, uShadowCorner);
    return 1.0 - (1.0 - uPictureLight) * (1.0 - smoothstep(-0.5, 0.5, dFace));
}

/// What the shadow caster leaves of this lamp's light at app px @p app, in
/// [0, 1] - reached through spotShadowLight (and spotShadowDarken), and only
/// for a lamp whose vShadowWeight is above 0 (a branch on a flat value, so
/// coherent per lamp: with the solid search in it the mask is a dozen SDFs,
/// and a lamp that casts nothing pays none of them and stays byte-identical to
/// the renderer before the caster). The weight is ShadowCaster::opacity, so a
/// translucent caster lets that much through.
///
/// The fill's lift comes after, from spotFillLift.
float spotShadowCover(vec2 app, float apertureWidth) {
    return mix(1.0, spotShadowMask(app, vShadow, vRecess, apertureWidth), vShadowWeight);
}

#ifdef SPOT_FILL
/// @p cover lifted by the other lamps' light at app px @p app - see THE FILL
/// VARIANT above. @p lit is the lamp's light BEFORE the shadow - after the
/// shoulder and the clip, and never dimmed by THE PICTURE'S LIGHT - which the
/// fill needs as the light this lamp would have put here unshadowed. For a
/// lamp that casts nothing, with a coverage of 1, the lift is (1 - 1) times a
/// finite value: exactly the coverage it was given.
float spotFillLift(vec2 app, vec3 lit, float cover) {
    vec3 own = lit * cover;
    // app is the fragment's centre, so truncation lands on its own texel.
    vec3 reaching = texelFetch(uFillSource, ivec2(app.x, uFillHeight - app.y), 0).rgb;
    // The screen undone for this lamp. Clamped: the buffer is 8 bits and
    // dithered, so where nothing else lights the pixel this can land a hair
    // under zero.
    vec3 others = clamp(1.0 - (1.0 - reaching) / max(1.0 - own, 1.0e-3), 0.0, 1.0);
    float ownPeak = max(max(lit.r, lit.g), lit.b);
    // A DEAD ZONE the size of the buffer's own error. Each lamp's write into
    // it was dithered by up to half a step and rounded, so with nothing else
    // lighting the pixel `reaching` still differs from `own` by a step or two -
    // which the division above amplifies by 1 / (1 - own). Without this a lone
    // lamp read its own rounding as light from elsewhere and nudged its own
    // penumbra.
    float noise = (2.0 / 255.0) / max(1.0 - max(max(own.r, own.g), own.b), 1.0e-3);
    float othersPeak = max(max(max(others.r, others.g), others.b) - noise, 0.0);
    float ratio = othersPeak / max(ownPeak, 1.0e-4);
    return cover + (1.0 - cover) * uFillAmount * smoothstep(0.0, SPOT_FILL_FULL_RATIO, ratio);
}
#endif

/// The light pass's entry point: @p lit - the lamp's light after the shoulder
/// and the clip - with the shadow caster applied at app px @p app: first the
/// shadow, from the real light, then THE PICTURE'S LIGHT. The ONE call
/// spotlight.frag's main() makes into this chunk outside the darkening
/// variant, after the clip and before the coverage alpha and the dither, for
/// the clip's reasons.
///
/// Behind branches on the flat weights rather than lerps: see
/// spotShadowCover. A lamp that casts nothing and dims no picture gets @p lit
/// back untouched.
///
/// Under SPOT_FILL it returns the fill's CORRECTION instead - what to screen
/// over the light pass's y so that the two make the lifted light; see THE
/// FILL VARIANT above. y is computed exactly as the plain pass computes it.
vec3 spotShadowLight(vec2 app, vec3 lit, float apertureWidth) {
#ifdef SPOT_FILL
    if (vShadowWeight <= 0.0) {
        return vec3(0.0);
    }
    float cover = spotShadowCover(app, apertureWidth);
    vec3 y = lit * cover;
    vec3 z = lit * spotFillLift(app, lit, cover);
    if (vPictureWeight > 0.0) {
        float gain = spotFaceGain(app);
        y *= gain;
        z *= gain;
    }
    return clamp((z - y) / max(1.0 - y, 1.0e-3), 0.0, 1.0);
#else
    if (vShadowWeight > 0.0) {
        lit *= spotShadowCover(app, apertureWidth);
    }
    if (vPictureWeight > 0.0) {
        lit *= spotFaceGain(app);
    }
    return lit;
#endif
}

#ifdef SPOT_SHADOW_DARKEN
/// ShadowCaster::darkness, clamped to [0, 1].
uniform float uShadowDarkness;
/// ShadowCaster::color, linear RGB clamped to [0, 1].
uniform vec3 uShadowColor;

/// How strongly a shadow darkens the destination at app px @p app: darkness,
/// except on the recessed picture of a lamp with vPictureWeight, where it
/// rises towards 1 as uPictureLight falls - see THE PICTURE'S LIGHT above. The
/// picture inside the rails only, not the whole face: the rails' shadow is the
/// one shadow that falls there, while the face's own edge is where the WALL's
/// shadow meets it, which keeps the wall's strength (rails a pixel or more
/// wide keep the two apart).
float spotShadowStrength(vec2 app) {
    if (vPictureWeight > 0.0 && uShadowInner.w > 0.5) {
        float dPicture = sdRoundBox(app - uShadowRect.xy, uShadowInner.xy, uShadowInner.z);
        float onPicture = 1.0 - smoothstep(-0.5, 0.5, dPicture);
        return mix(uShadowDarkness, mix(1.0, uShadowDarkness, uPictureLight), onPicture);
    }
    return uShadowDarkness;
}

/// The darkening variant's output for the fragment at app px @p app - see THE
/// DARKENING VARIANT above, and the one call spotlight.frag's main() makes
/// into this chunk in that variant. @p lit is the lamp's light after the shoulder and the clip,
/// before the shadow; @p dither the output dither main() computes.
///
/// An exact 0 wherever the lamp casts nothing, the fragment is outside its
/// shadow or the lamp puts no light there, so the dither cannot speckle the
/// rest of the strip: under GL_ONE / GL_ONE_MINUS_SRC_ALPHA a 0 leaves the
/// destination exactly as it was.
vec4 spotShadowDarken(vec2 app, vec3 lit, float apertureWidth, float dither) {
    if (vShadowWeight <= 0.0) {
        return vec4(0.0);
    }
    float cover = spotShadowCover(app, apertureWidth);
#ifdef SPOT_FILL
    cover = spotFillLift(app, lit, cover);
#endif
    float reach = smoothstep(0.0, SPOT_SHADOW_FULL_REACH, max(max(lit.r, lit.g), lit.b));
    float d = spotShadowStrength(app) * (1.0 - cover) * reach;
    if (d <= 0.0) {
        return vec4(0.0);
    }
    // The same half-step dither as the light, on the coverage: the colour is
    // premultiplied by it, so the two move together.
    d = clamp(d + dither, 0.0, 1.0);
    return vec4(uShadowColor * d, d);
}
#endif
