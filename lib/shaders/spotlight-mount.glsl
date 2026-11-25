precision highp float;

// The spotlight's MOUNT SHADOW: how a framed artwork hung on the wall shades
// the cone that falls on it. A 2.5D screen-space term - no depth buffer, no
// shadow map, only the artwork's rounded-box distance and the direction to the
// lamp - that multiplies the CONE (never the bloom, which is glare at the
// lens):
//
//   cone *= (1 - mountShadow) * bevelFacing
//
// Two mounts, by uArtLevels.z (0 flush, 1 raised, 2 recessed):
//
//   RAISED   - the panel stands off the wall. The wall OUTSIDE the art
//              receives; the art itself occludes. Shadow on the side away
//              from the lamp, the panel face stays lit.
//   RECESSED - the art sits behind an opening. The art INSIDE receives; the
//              wall around it occludes, so the edge nearest the lamp shades a
//              band of the art. A 45 degree bevel round the opening is lit by
//              how it faces the lamp (bevelFacing).
//
// NO LOOP. The mockup marched 8 points toward the lamp and kept the most
// occluded one, so the shadow was a union of 8 offset copies of the art: at
// softness 0 its edges were a staircase (a raised panel's corners) and its tip a
// set of nested contours (a recessed one's band). This is the same march's
// CONTINUOUS limit, in closed form: where the ray enters (raised) or leaves
// (recessed) the rounded box, and the best point of the soft ramp about it.
// Against a 1500-sample march of the same expression the 8 samples were off by
// 0.39 (p99, occlusion) at softness 0 and 0.21 at 0.1; this is off by 0.02 and
// 0.035. It also costs less: no loop, and a square root only where it is needed.
//
// Injected via @SPOTLIGHT_MOUNT@ into the SPOT_FX_MOUNT and
// SPOT_FX_MOUNT_DARKEN variants of spotlight.frag ONLY (SPOTLIGHT_MOUNT_FRAG_SRC
// and SPOTLIGHT_MOUNT_DARKEN_FRAG_SRC in shaders.h.in), after
// @SPOTLIGHT_DITHER@ and ahead of spotlight.frag's own text. So it declares
// its own precision, and PROTOTYPES of sdRoundBox and spotClipMask, which
// spotlight.frag defines further down - one copy of each, not two. Not a
// shader: no main(). Its call sites are the `#if defined(SPOT_FX_MOUNT...)`
// blocks in spotlight.frag's main().
//
// ASCII only, like every shader source here.

#define SPOT_FX_BEVEL_MAX_GAIN 1.8   ///< Ceiling on a bevel face's Lambert ratio.
#define SPOT_FX_EDGE_AA        0.7   ///< Half width of every edge ramp, px.
#define SPOT_FX_TAPER_START    0.65  ///< The shadow's tip fades over [0.65, 1] of its length.
#define SPOT_FX_TAPER_INV      2.857143 ///< 1 / (1 - SPOT_FX_TAPER_START).
#define SPOT_FX_MIN_INCIDENCE  0.1   ///< Floor of the cosine a ray meets an edge at; below it the ramp would run on forever.
#define SPOT_FX_DARKEN_KNEE    0.04  ///< Cone light (0..1) at which the darkening is at half strength.

/// The lamp's own position, app px (SpotLight::position) - constant over a
/// lamp's strip. Written by spotlight.vert.
flat in vec2 vLampPos;

/// Artwork centre.xy and half size.zw, app px (top-left origin, +y down).
uniform vec4 uArtRect;
/// .x corner radius px. .y feather, .z feather shift, .w protect belong to the
/// art protection, which this file does not implement.
uniform vec4 uArtShape;
/// .z mount mode (0 flush, 1 raised, 2 recessed), .w shadow strength [0, 1].
/// .x / .y are not read here.
uniform vec4 uArtLevels;
/// .x shadow length px, .y shadow softness (penumbra px per px of distance),
/// .z bevel width px (recessed only), .w tan of the lamp's elevation.
uniform vec4 uArtMount;

/// How far the shadow darkens what is BEHIND the light, [0, 1]: the
/// SPOT_FX_MOUNT_DARKEN variant's one extra uniform (SpotMount::darkness).
uniform float uArtDarkness;

/// Defined in spotlight.frag, after this file.
float sdRoundBox(vec2 p, vec2 b, float r);
float spotClipMask(vec2 app);

/// Signed distance to the artwork, px, negative inside.
float artSd(vec2 app) {
    return sdRoundBox(app - uArtRect.xy, uArtRect.zw, uArtShape.x);
}

/// The most occluded point of the soft ramp along the ray, with the tip's
/// taper: max over z in [0, zmax] of S(z) * W(t0 + (2 z - 1) q), S and W
/// smoothsteps. t0 = te / s is where the ray meets the edge (the ramp's
/// centre), q the ramp's half width along the ray. Three samples, placed in
/// the part of the ramp where W is still above 0 - the maximum is always in
/// its upper half - and no branch on their values, so the result is
/// continuous in every input.
float mountPeak(float te, float c, float s, float y, float zmax) {
    float q = (1.0 + y * clamp(te, 0.0, s)) / (max(c, SPOT_FX_MIN_INCIDENCE) * s);
    float u0 = (te / s - q - SPOT_FX_TAPER_START) * SPOT_FX_TAPER_INV;
    float k = 2.0 * q * SPOT_FX_TAPER_INV;
    float zh = min(zmax, (1.0 - u0) / k);
    if (zh <= 0.0) {
        return 0.0;
    }
    vec3 z = zh * vec3(0.6, 0.8, 1.0);
    vec3 u = clamp(u0 + k * z, 0.0, 1.0);
    vec3 f = z * z * (3.0 - 2.0 * z) * (1.0 - u * u * (3.0 - 2.0 * u));
    return max(f.x, max(f.y, f.z));
}

/// How much of the lamp the mount hides at @p app, in [0, strength].
///
/// Looks along the ray from @p app toward the lamp, over
/// s = min(shadow length, distance to the lamp) - never past the lamp. RAISED:
/// the ray ENTERS the rounded box at te. RECESSED: it LEAVES the box at te. The
/// ramp about te widens with the distance from the edge (1 + softness * te px),
/// and the last third of the ray fades out, so the tip dies away rather than
/// ending in a cut at length s.
///
/// A raised shadow also has sides. Seen along the ray the rounded box is a
/// segment of half width a.y * inner.x + a.x * inner.y + r - exact, a disc
/// adds r to a polygon's - so `miss`, the ray's distance from that segment, is
/// the signed distance to the shadow's straight side: it bounds the ramp
/// (zmax), and a ray that misses by more than the widest ramp needs no more.
///
/// The box is met in closed form, per axis (slab): the ray is mirrored into
/// +x +y, where entry and exit are one max and one min. Where that point lies
/// in a corner's square, the corner's circle replaces it.
float mountShadow(vec2 app, vec2 toLamp, float distLamp) {
    float s = min(uArtMount.x, distLamp);
    if (uArtLevels.z < 0.5 || uArtLevels.w <= 0.0 || s < 0.5) {
        return 0.0;
    }
    bool raised = uArtLevels.z < 1.5;
    float y = uArtMount.y;
    vec2 b = uArtRect.zw;
    float r = uArtShape.x;
    vec2 p = app - uArtRect.xy;
    // A recessed pixel outside the opening receives nothing, and neither does a
    // raised one on the art's face (past the edge's ramp, away from a corner).
    vec2 ap = abs(p);
    if (raised ? all(lessThan(ap, b - SPOT_FX_EDGE_AA)) && any(lessThan(ap, b - r))
               : any(greaterThan(ap, b + SPOT_FX_EDGE_AA))) {
        return 0.0;
    }

    vec2 flip = 2.0 * step(vec2(0.0), toLamp) - 1.0;
    vec2 a = max(abs(toLamp), vec2(1.0e-4));
    vec2 q = p * flip;
    vec2 inner = b - r;
    vec2 inv = 1.0 / a;
    vec2 tf = (b - q) * inv; // where the ray crosses each far plane
    float te;
    float c;
    float zmax = 1.0;
    float exitT = min(tf.x, tf.y);

    if (raised) {
        if (exitT <= 0.0) {
            return 0.0; // the art is behind the pixel
        }
        float cross2 = a.x * q.y - a.y * q.x;
        float miss = abs(cross2) - (a.y * inner.x + a.x * inner.y + r);
        if (miss >= 1.0 + y * s) {
            return 0.0;
        }
        vec2 tn = (-b - q) * inv; // where it crosses each near plane
        float enterT = max(tn.x, tn.y);
        c = tn.x > tn.y ? a.x : a.y;
        // The closest approach to the corner the ray passes, for the side's
        // ramp width; a ray that misses meets its peak there.
        float tcRaw = sign(cross2) * (a.y * inner.y - a.x * inner.x) - dot(a, q);
        float tc = clamp(tcRaw, 0.0, s);
        zmax = clamp(0.5 - 0.5 * miss / (1.0 + y * tc), 0.0, 1.0);
        if (miss >= 0.0) {
            te = tc;
            // Past the corner already: it is behind. And the lamp may come
            // before the closest approach; the distance is then to the lamp.
            zmax = clamp(0.5 - 0.5 * length(vec2(miss, max(tcRaw - s, 0.0))) / (1.0 + y * tc), 0.0, 1.0) *
                   clamp(tcRaw, 0.0, 1.0);
        } else {
            te = max(enterT, 0.0);
            if (r > 0.0) {
                vec2 e = q + a * te;
                if (all(greaterThan(abs(e), inner))) {
                    vec2 m = q - sign(e) * inner;
                    float bm = dot(a, m);
                    float disc = bm * bm - dot(m, m) + r * r;
                    if (disc <= 0.0 || bm > 0.0) {
                        return 0.0; // the corner's circle is behind the pixel
                    }
                    float root = sqrt(disc);
                    te = max(-bm - root, 0.0);
                    c = root / r;
                }
            }
            // A pixel inside the box (the art's own AA fringe, or the notch
            // outside a rounded corner) that leaves it within a pixel, measured
            // square to the side it leaves by, is on a lit edge: it must not
            // shade itself.
            if (enterT < 0.0) {
                zmax = min(zmax, 0.5 + 0.5 * (exitT * (tf.x < tf.y ? a.x : a.y) - 2.0));
            }
            // The ramp of the edge it enters by is the distance to that edge's
            // LINE, and before the ray is level with the edge the art is farther
            // than that. The art's distance at the ray's end bounds the ramp: a
            // lamp beside the art cannot shade past itself.
            vec2 beyond = (tn - s) * a;
            float end = max(beyond.x, beyond.y) > 0.0 ? -length(max(beyond, 0.0)) : min(-beyond.x, -beyond.y);
            zmax = min(zmax, 0.5 + 0.5 * end / (1.0 + y * s));
        }
    } else {
        te = exitT;
        c = tf.x < tf.y ? a.x : a.y;
        if (r > 0.0) {
            vec2 e = q + a * te;
            if (all(greaterThan(abs(e), inner))) {
                vec2 m = q - sign(e) * inner;
                float bm = dot(a, m);
                float disc = bm * bm - dot(m, m) + r * r;
                if (disc > 0.0) {
                    float root = sqrt(disc);
                    te = -bm + root;
                    c = root / r;
                }
            }
        }
    }

    float occ = mountPeak(te, c, s, y, zmax);
    if (occ <= 0.0) {
        return 0.0;
    }

    // Who receives: raised, the wall outside the art; recessed, the art
    // inside. The two edges below are the mockup's, and exact: the distance is
    // 1-Lipschitz, so an art farther than s plus the widest ramp cannot shade
    // the pixel - SpotlightRenderer bounds the region it draws by them.
    // The distance is the larger axis overhang except in the corner squares, so
    // the square root is taken only there.
    float dA = all(greaterThan(ap, inner)) ? artSd(app) : max(ap.x - b.x, ap.y - b.y);
    float receive = smoothstep(-SPOT_FX_EDGE_AA, SPOT_FX_EDGE_AA, raised ? dA : -dA);
    if (receive <= 0.0 || (raised ? dA - s >= 1.0 + y * s : dA + s <= -(1.0 + y * s))) {
        return 0.0;
    }
    return occ * receive * uArtLevels.w;
}

#if defined(SPOT_FX_MOUNT_DARKEN)
/// Coverage of the darkening at @p app, [0, darkness]: the black laid over
/// whatever is behind this layer where the shadow falls, so that the shadow
/// reads on a background the light cannot brighten (screened onto white, a lamp
/// adds nothing and its shadow takes nothing away). The shadow's own mask,
/// times how much of the cone's light is there (@p coneLight, 0..1 of full
/// scale, the unshadowed cone): a soft saturation, so it fades with the light
/// without an edge and is below one 8-bit step where the strip it is drawn in
/// ends.
float mountDarken(float shadow, float coneLight, float clipCover) {
    float lit = coneLight * coneLight;
    return uArtDarkness * shadow * (lit / (lit + SPOT_FX_DARKEN_KNEE * SPOT_FX_DARKEN_KNEE)) * clipCover;
}
#endif

/// Brightness of the recessed mount's bevel at @p app, relative to the flat
/// wall: 1 everywhere off the bevel band.
///
/// The band is [0, bevel] px OUTSIDE the art's SHARP rectangle (no corner
/// radius), so its corners are mitred like a picture frame; `seam` blends the
/// x face into the y face across the mitre. Each face is a 45 degree slope
/// whose normal is (-sign(p) along its axis, 1) / sqrt(2), lit by a lamp at
/// elevation e in direction toLamp, (toLamp, tan e). Lambert against the flat
/// wall's normal (0, 0, 1):
///
///   ratio = (dot(-sign(p) axis, toLamp) + tan e) / (sqrt(2) tan e)
///
/// which is `norm` below. Clamped to [0, SPOT_FX_BEVEL_MAX_GAIN]: a face
/// turned toward the lamp brightens, one turned away darkens, and one past
/// grazing (dot < -tan e) is fully self-shadowed.
float bevelFacing(vec2 app, vec2 toLamp) {
    if (uArtLevels.z < 1.5 || uArtMount.z <= 0.0) {
        return 1.0;
    }
    vec2 p = app - uArtRect.xy;
    vec2 q = abs(p) - uArtRect.zw;
    float dA = max(q.x, q.y);
    if (dA < -SPOT_FX_EDGE_AA || dA > uArtMount.z + SPOT_FX_EDGE_AA) {
        return 1.0;
    }
    float tanElev = uArtMount.w;
    float norm = 1.0 / (1.41421 * max(tanElev, 1.0e-3));
    float ratioX = clamp((dot(vec2(-sign(p.x), 0.0), toLamp) + tanElev) * norm,
                         0.0, SPOT_FX_BEVEL_MAX_GAIN);
    float ratioY = clamp((dot(vec2(0.0, -sign(p.y)), toLamp) + tanElev) * norm,
                         0.0, SPOT_FX_BEVEL_MAX_GAIN);
    float seam = smoothstep(-SPOT_FX_EDGE_AA, SPOT_FX_EDGE_AA, q.x - q.y);
    float ratio = mix(ratioY, ratioX, seam);
    float w = smoothstep(-SPOT_FX_EDGE_AA, SPOT_FX_EDGE_AA, dA) *
              (1.0 - smoothstep(uArtMount.z - SPOT_FX_EDGE_AA,
                                uArtMount.z + SPOT_FX_EDGE_AA, dA));
    return mix(1.0, ratio, w);
}
