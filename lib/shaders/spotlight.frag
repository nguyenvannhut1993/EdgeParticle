precision highp float;

// Spotlight fragment stage: one lamp's cone plus its aperture bloom, evaluated
// in that lamp's own frame.
//
// THE FALLOFF, in full, because spotlight-renderer.cpp inverts exactly this to
// decide where to stop drawing:
//
//   halfW(a) = apertureWidth + max(a, 0) * tanHalfBeam
//
//   cone = exp(-(c / halfW)^2 * softK)                 // soft across the beam
//        * smoothstep(-apertureWidth,
//                     SPOT_NEAR_FADE * apertureWidth, a)  // fade in at the lamp
//        * exp(-max(a, 0) / throwLength)               // fade along the throw
//        * pow(apertureWidth / halfW, spreadFalloff)   // energy spreads as it widens
//
//   bloom = bloomStrength * r^2 / (a^2 + c^2 + r^2)    // inverse-square core
//         * (1 - smoothstep(S * SPOT_BLOOM_WINDOW_INNER, S, d))   // windowed off
//
// The shading then passes through a HIGHLIGHT SHOULDER - see the block around
// `litPeak` below, and SPOT_HIGHLIGHT_KNEE. That is the one part of this
// program the solve in SolveConeAcross does NOT invert, and does not have to:
// it only ever scales a fragment DOWN, and only above SPOT_HIGHLIGHT_KNEE of
// full scale, three orders of magnitude above the half-step the strip bound is
// solved against.
//
// Three of those terms are not obvious:
//
//   The lateral falloff is a GAUSSIAN, not a smoothstep cut. The look this
//   renderer targets has no visible beam edge anywhere, so softness sets how
//   broad the gaussian is and there is deliberately no setting that produces
//   an edge.
//
//   `apertureWidth / halfW` is what stops a wide beam reading as brighter than
//   a narrow one at equal intensity. Drop it and beamAngle becomes a second,
//   non-linear brightness control.
//
//   Its exponent, SpotLight::spreadFalloff, is there because that same term is
//   what limits REACH. It decays as 1/distance and so outruns the throw's
//   exponential by a wide margin: at the default aperture and a 26 degree
//   beam it is at 5% by 1000 px, where a 3000 px throw is still at 72%. The
//   exponent is 1.0 by default, which is exactly the expression above and the
//   only value that conserves energy; below that the beam carries further.
//   pow, not a branch: the exponent is flat, so this is uniform across the
//   draw either way, and a branch would only trade one instruction for two.
//
//   The bloom's window has no visual job at all - the inverse-square core has
//   no natural end, so without it the term's support is the whole framebuffer
//   and there is no finite strip to draw. See spotlight-tuning.h.
//
// This shader never reads gl_FragCoord: everything arrives interpolated in the
// lamp's frame - or, for the clip area and the shadow, in app space through
// vApp. That is why the y-flip in Render is free, why
// SpotlightConfig::resolutionScale still needs no uniform of its own, and why
// the sub-viewport caveat in BaseRenderer's doc comment does not apply here.
//
// One caveat of its own, shared by the clip and the shadow: reading app space
// rather than gl_FragCoord keeps a mask's GEOMETRY identical at any scale, but
// the mask is still resolved one fragment at a time, so a reduced buffer
// resolves its boundary at that buffer's pitch. SpotlightRenderer pins the
// scale to 1.0 while any clipped or shadow-casting lamp is enabled rather than
// let that ship - see GetClampedSpotScale.
//
// THE CLIP, and why it multiplies rather than reshaping anything. A lamp with
// SpotLight::clipped set is cut off by a rounded-rectangle area in app
// coordinates (SpotlightConfig::clipArea), on whichever side the mode names. That
// cut is COVERAGE applied to the finished shading - `lit *= mask` - not a term
// folded into the falloff, so a clipped beam is the same beam with part of it
// missing: moving the area cannot make the light that survives brighter, dimmer
// or a different shape. The alpha follows for free, because the coverage below
// is derived from `lit` after the multiply rather than alongside it.
//
// The mask is applied BEFORE the dither, which is the order that matters and
// is safe for exactly the reason the dither comment gives: the offset is
// strictly under half a destination step, so a fragment the clip took to zero
// still rounds to zero and the cut region stays black rather than acquiring a
// speckle. Dithering after the mask also means the clip's own soft edge - one
// more shallow gradient - gets the same treatment as the falloff it crosses.
//
// THE SHADOW, and why it is a line test rather than anything that looks at the
// scene. A lamp with SpotLight::castsShadow set is blocked by one flat rounded
// rectangle, SpotlightConfig::shadowCaster, standing off the wall. Put the
// wall at z = 0, the caster's face at z = d and the lamp at z = h over its app
// position L: the line from a wall fragment p to the lamp crosses the caster's
// plane at
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
// THE SOLID BLOCK AND THE FRAME (docs/spotlight-solid-frame-plan.md). With
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
// THE FILL VARIANT (SPOT_FILL), which is a look and not physics. With two
// lamps, a point in lamp A's shadow receives exactly lamp B's light, so where
// B is weaker than A the shadow still shows, lighter. ShadowCaster::fill
// washes it out instead. This program runs SECOND: the plain one has already
// drawn every lamp - shadows applied - into uFillSource, so each fragment can
// read how much light reaches its pixel (R) and take its own share back out of
// the screen to find what the OTHER lamps put there:
//
//   others = 1 - (1 - R) / (1 - own),   own = this lamp's light after its shadow
//
// The shadow's coverage is then lifted towards 1 by
// uFillAmount * smoothstep(0, SPOT_FILL_FULL_RATIO, ratio), ratio =
// peak(others) / peak(what this lamp would have put here unshadowed): gone
// where the others reach half of it (see spotlight-tuning.h for why half),
// untouched where they put nothing. The lamp's own light is excluded by
// construction, which is why this cannot be done on the composite: from R
// alone, a lamp's own penumbra looks like light from elsewhere, and filling
// with it would shrink every shadow.
//
// It only ever raises a coverage that is below 1, so outside every shadow this
// program writes exactly what the plain one does.
//
// Output is premultiplied colour plus a COVERAGE ALPHA, the same
// max-of-channels rule neon.frag and lens-flare.frag use. The renderer pairs
// it with a separate-alpha SCREEN (GL_ONE_MINUS_DST_COLOR / GL_ONE, and the
// same on alpha): light only adds, lamp order still cannot change the image,
// and the alpha channel accumulates a record of where light was written - but
// overlapping lamps approach full scale instead of summing past it. The
// highlight shoulder below keeps ONE lamp under 1.0; it cannot see the others,
// and the screen is what stops two of them clipping where they cross.
//
// That alpha is NOT decoration and this shader is the reason the whole layer
// once vanished on a device. Every other layer here writes a coverage alpha;
// this one wrote a literal 0.0, which is invisible on a desktop demo (the
// window is opaque, so nothing ever reads the framebuffer's alpha back) and
// fatal on an embedded surface that a compositor or hardware video plane
// blends: `out = ui.rgb * ui.a + video * (1 - ui.a)` multiplies every lit
// spotlight pixel by zero. Measured offscreen at 640x360 over a transparent
// clear, a single lamp lit 72,615 pixels of colour and exactly 0 pixels of
// alpha, and composited to nothing at all over a background. See
// SpotlightRenderer::Render.

in vec2 vLocal;      ///< (along, across) px in this lamp's frame.
in vec2 vApp;        ///< App px, top-left origin, +y down. Clip, shadow and fill.
flat in vec4 vP0;    ///< tanHalfBeam, throwLength, softK, intensity.
flat in vec4 vP1;    ///< apertureWidth, bloom, bloomRadius, bloomWindow.
flat in vec3 vColor; ///< Linear RGB.
flat in float vClipWeight; ///< 1 where this lamp honours the clip area, else 0.
flat in float vSpreadFalloff; ///< Exponent on the spread term, already clamped.
/// Lamp app position (xy), t = caster / lamp wall distance (z), penumbra half
/// width on the caster's plane in px (w, never 0 - the CPU floors it).
flat in vec4 vShadow;
flat in float vShadowWeight; ///< ShadowCaster::opacity if this lamp casts a shadow, else 0.
/// The rails over the recessed picture: t_in (x) and the penumbra half width
/// at the rails' front in px (y, never 0).
flat in vec2 vRecess;

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
uniform vec4 uShadowInner;

#ifdef SPOT_FILL
/// What the plain pass drew: every lamp, shadows applied, screened into a
/// buffer the size of the viewport under the same y-flipped ortho - so app px
/// (x, y) is texel (x, uFillHeight - y).
uniform sampler2D uFillSource;
/// ShadowCaster::fill, clamped to [0, 1].
uniform float uFillAmount;
/// Height of uFillSource in texels, for the y flip.
uniform float uFillHeight;
#endif

/// The clip area, in APP coordinates: centre.xy, half extent.xy. Already
/// collapsed from ClipArea's top-left + size on the CPU, because a
/// rounded-box SDF wants a centred box and the conversion is the same two
/// adds every fragment would otherwise repeat.
uniform vec4 uClipRect;

/// cornerRadius px, edgeSoftness px, and 1 for KEEP_INSIDE / 0 for
/// KEEP_OUTSIDE. The mode is a LERP WEIGHT rather than a branch: the two
/// answers are each other's complement, so mixing between them is one
/// instruction and costs nothing in divergence.
uniform vec3 uClipParams;

out vec4 fragColor;

/// Signed distance to a rounded box centred at the origin, negative inside.
/// @p b is the half extent, @p r the corner radius (already clamped on the CPU
/// to at most the shorter half extent, so the `- r` below cannot invert the
/// box).
///
/// Byte-identical to the sdRoundBox in black-rect.frag, neon.frag,
/// neon-blit.frag and droplets.frag, and named the same on purpose: it is a
/// generic primitive, not a clip-specific one. spotClipMask below is the part
/// that knows about the clip area.
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

/// Coverage the clip area leaves at @p app, in [0, 1].
///
/// Independent of vClipWeight on purpose - the caller mixes this against 1.0
/// with that weight, so an unclipped lamp costs the same arithmetic instead of
/// a branch that would diverge inside a draw call covering both kinds of lamp.
float spotClipMask(vec2 app) {
    float d = sdRoundBox(app - uClipRect.xy, uClipRect.zw, uClipParams.x);
    // Half the feather either side of the boundary, so edgeSoftness is the
    // full width of the fade and 0 collapses to a hard step. The floor keeps
    // smoothstep's two edges apart at edgeSoftness 0, where equal edges are
    // undefined rather than a step on some drivers.
    float h = max(uClipParams.y, 1.0e-4) * 0.5;
    float inside = 1.0 - smoothstep(-h, h, d);
    return mix(1.0 - inside, inside, uClipParams.z);
}

/// u(s) of the solid search: the caster's SDF at the point a fraction @p s of
/// the way from @p p to @p lamp, over the emitter's window there - @p feather
/// at the wall, @p aperture * s once the window is the emitter's projection.
float spotSolidRatio(vec2 p, vec2 lamp, float s, float aperture, float feather) {
    float d = sdRoundBox(mix(p, lamp, s) - uShadowRect.xy, uShadowRect.zw, uShadowCorner);
    return d / max(aperture * s + feather * (1.0 - s), 1.0e-4);
}

/// Visibility of wall point @p p past a SOLID caster, for a lamp at @p lamp
/// whose line is inside the block for s in [0, @p tMax] - see THE SOLID BLOCK
/// at the top of this file.
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
    // docs/spotlight-solid-frame-plan.md.
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
/// SHADOW and THE SOLID BLOCK at the top of this file.
///
/// Called only for a lamp whose weight is above 0 (a branch on a flat value,
/// coherent per lamp): with the solid search in it this is a dozen SDFs, and a
/// lamp that casts nothing has no use for any of them. The CPU still writes
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

/// Interleaved gradient noise, in [-0.5, 0.5]. One fract, one dot: the whole
/// dither costs about as much as the bloom's sqrt.
///
/// Deliberately NOT the fract(sin(dot(...))) hash. That one feeds sin an
/// argument in the tens of thousands, and how much of it survives is a
/// precision question every GPU answers differently - on a part that rounds
/// it coarsely the "noise" degenerates into a second set of bands, which is
/// the artefact this function exists to remove.
///
/// @p p is in PIXELS, and vLocal is the right pixel-valued thing to hand it:
/// the lamp frame is a rotation plus a translation of the framebuffer, so a
/// step of one fragment is a step of one unit here too, whatever the lamp's
/// angle. That also keeps the promise below - the pattern is anchored to the
/// lamp, so under an animation it travels WITH the beam instead of crawling
/// across it, and gl_FragCoord still never appears in this shader.
///
/// NOT vApp, which is also in pixels but is anchored to the SCREEN: feeding it
/// here would give every lamp the same pattern in the same place, so two
/// overlapping beams would dither in lockstep and correlate exactly where the
/// noise is meant to be independent.
float spotDither(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5;
}

void main() {
    float along = vLocal.x;
    float across = vLocal.y;

    // UNPACK ALL EIGHT SLOTS, named, before any of them is used.
    //
    // vP0 and vP1 are packing, not meaning: the eight per-lamp scalars ride in
    // two vec4s because that is how a vertex attribute travels. Half of them
    // used to be read inline as raw swizzles - `exp(-lat * lat * vP0.z)` for
    // softK, `vColor * vP0.w` for intensity - which made the term stack below
    // impossible to check against the falloff derivation at the top of this
    // file without counting components against a comment. Naming them here
    // costs nothing (every one is a flat varying folded at compile time) and
    // makes the rest of this function read like that derivation.
    float tanHalfBeam  = vP0.x;
    float softK        = vP0.z;
    float intensity    = vP0.w;
    float bloomStrength = vP1.y;
    float bloomRadius  = vP1.z;
    // The same floors the renderer's solve applies, so the two agree about
    // what a degenerate lamp means rather than disagreeing near zero.
    float apertureWidth = max(vP1.x, SPOT_MIN_APERTURE);
    float throwLength   = max(vP0.y, SPOT_MIN_THROW);
    float bloomWindow   = max(vP1.w, 1.0);

    float halfW = apertureWidth + max(along, 0.0) * tanHalfBeam;

    float lat = across / halfW;
    float cone = exp(-lat * lat * softK);
    cone *= smoothstep(-apertureWidth, SPOT_NEAR_FADE * apertureWidth, along);
    cone *= exp(-max(along, 0.0) / throwLength);
    // Both operands are strictly positive - halfW >= apertureWidth >=
    // SPOT_MIN_APERTURE, and the renderer clamps the exponent to [0, 2] - so
    // pow is defined here for every lamp a host can configure.
    //
    // The exponent 1 case is the plain divide, not pow(x, 1.0), and that is
    // not pedantry: pow is exp2(y * log2(x)) on every GPU this ships to, so it
    // returns x only to within its own precision. Measured at 1920x1080 on one
    // default lamp, pow(x, 1.0) moved 15 channels out of 8.3 million by 1/255
    // against the divide - small, but it would make EVERY existing lamp render
    // differently to buy a knob most of them will never set.
    //
    // A uniform branch, so it costs a predicted compare rather than
    // divergence: vSpreadFalloff is flat, and the renderer writes an exact
    // 1.0f into the attribute. Safe in this shader for the same reason the
    // highlight shoulder's branch is - nothing here calls a derivative (the
    // note in lens-flare.frag's ghost loop is the opposite case).
    cone *= (vSpreadFalloff == 1.0)
                ? apertureWidth / halfW
                : pow(apertureWidth / halfW, vSpreadFalloff);

    float d2 = along * along + across * across;
    float r2 = bloomRadius * bloomRadius;
    float bloom = bloomStrength * r2 / (d2 + r2);
    bloom *= 1.0 - smoothstep(bloomWindow * SPOT_BLOOM_WINDOW_INNER,
                              bloomWindow, sqrt(d2));

    vec3 lit = vColor * intensity * (cone + bloom);

    // HIGHLIGHT SHOULDER. Nothing above bounds `lit`, and an RGBA8 target
    // clips each channel on its own - which is a HUE change, not a brightness
    // one, and is what turns a bright amber lamp's core white. The reasoning,
    // and why this is a shoulder rather than a clamp, is in
    // spotlight-tuning.h beside SPOT_HIGHLIGHT_KNEE.
    //
    // Applied to the PEAK CHANNEL and folded back onto the whole vector, so
    // the ratio between the channels - the hue - survives the compression.
    //
    // BEFORE the cut below, which is what keeps the clip's promise: the light
    // that survives a clip is the unclipped lamp's light times a coverage. Were
    // this to run after the mask, a half-covered fragment would sit lower on
    // the shoulder and so be compressed less, and moving the clip area would
    // change the shape of the shading it is only supposed to reveal.
    //
    // Reinhard above the knee: at litPeak == K this is exactly K and its
    // derivative is exactly 1, so it joins the untouched region with no
    // contour; as litPeak grows it approaches 1.0 and never arrives.
    //
    // Divergence is free here: this shader calls no derivative, which is what
    // makes a per-fragment branch safe (the note in lens-flare.frag's ghost
    // loop is the opposite case).
    float litPeak = max(max(lit.r, lit.g), lit.b);
    if (litPeak > SPOT_HIGHLIGHT_KNEE) {
        float room = 1.0 - SPOT_HIGHLIGHT_KNEE;
        float over = litPeak - SPOT_HIGHLIGHT_KNEE;
        float shouldered = SPOT_HIGHLIGHT_KNEE + room * over / (over + room);
        lit *= shouldered / litPeak;
    }

    // The cut. mix rather than an `if`, so a draw call carrying both clipped
    // and unclipped lamps shades them at the same cost - vClipWeight is flat,
    // so this is one lerp against a value constant across the triangle.
    lit *= mix(1.0, spotClipMask(vApp), vClipWeight);

    // The shadow, in the same place: a lerp against a flat weight - but
    // behind a BRANCH on that weight, unlike the clip, because the solid
    // caster's search makes the mask a dozen SDFs and a lamp that casts
    // nothing should not pay them. The branch is on a flat value, so it is
    // coherent per lamp, and this shader calls no derivative. A lamp that
    // skips it is multiplied by nothing at all, which is what keeps it
    // byte-identical to the renderer before the caster. The weight is
    // ShadowCaster::opacity, so a translucent caster lets that much through.
    float shadowCover = 1.0;
    if (vShadowWeight > 0.0) {
        shadowCover = mix(1.0, spotShadowMask(vApp, vShadow, vRecess, apertureWidth), vShadowWeight);
    }
#ifndef SPOT_FILL
    if (vShadowWeight > 0.0) {
        lit *= shadowCover;
    }
#else
    // THE FILL - see the block at the top of this file.
    vec3 own = lit * shadowCover;
    // vApp is at the fragment's centre, so truncation lands on its own texel.
    vec3 reaching = texelFetch(uFillSource, ivec2(vApp.x, uFillHeight - vApp.y), 0).rgb;
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
    shadowCover += (1.0 - shadowCover) * uFillAmount * smoothstep(0.0, SPOT_FILL_FULL_RATIO, ratio);
    lit *= shadowCover;
#endif

    // Coverage = brightest channel, exactly as in neon.frag and
    // lens-flare.frag: a bright aperture core reads as solid to whatever
    // composites this surface, the dim spill stays as good as additive, and an
    // unlit fragment leaves the alpha it found alone (0 screened onto
    // anything leaves it unchanged). Read from `lit` AFTER the clip, so a cut fragment
    // records no coverage either - light that was removed must not go on
    // claiming the surface it would have lit.
    float cov = clamp(max(max(lit.r, lit.g), lit.b), 0.0, 1.0);

    // DITHER, and the reason this layer is the one that needs it.
    //
    // Everything above is a smooth function, and every bit of that smoothness
    // is then thrown away by an 8-bit framebuffer. That is normally harmless
    // because a gradient crossing a step in a pixel or two quantises into an
    // edge nobody can see. The outer cone is the opposite case: it is the
    // flattest gradient in this library, crossing one step every 13 to 50 px
    // at default settings, so the rounding lays down a handful of very wide
    // bands - and because the cone's iso-contours are near-straight rays, the
    // band edges are long straight lines. THAT is what reads as a hard-edged
    // strip sitting on top of whatever is behind this layer, and it is not a
    // clipped strip, a seam, or a blend mode: it is the last few 1/255 steps
    // of the falloff, drawn exactly.
    //
    // Half a step of noise, added BEFORE the framebuffer rounds, decorrelates
    // that rounding from the signal: a pixel near a band edge lands on either
    // side of it with a probability that follows the true value, so the edge
    // spreads across the whole width of the band and stops being an edge. The
    // noise itself is under one destination step and invisible.
    //
    // Rectangular +/- half a step, not the triangular +/- one step a
    // convolution textbook would reach for, for one reason that matters here:
    // `fract` returns strictly under 1, so this offset is strictly under half
    // a step, and GL's round-to-nearest therefore cannot take a fragment the
    // falloff left at ZERO up to 1. This pass draws over a strip that is
    // mostly dark - the region behind the lamp, the corners the bloom disc
    // does not fill - and speckle across all of it would be a worse artefact
    // than the banding. Fragments that DO light up where an undithered pass
    // rounded them away are the ones carrying real sub-step signal, which is
    // exactly how the outermost band edge stops being an edge.
    //
    // Same offset on all three channels and on the coverage, so the dither
    // moves the value and never the hue or the premultiplication.
    float dither = spotDither(vLocal) * (SPOT_DITHER_STEPS / 255.0);

    fragColor = vec4(max(lit + dither, 0.0), max(cov + dither, 0.0));
}
