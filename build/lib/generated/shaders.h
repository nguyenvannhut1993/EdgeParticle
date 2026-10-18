#ifndef _EDGE_LIGHTING_SHADERS_H_
#define _EDGE_LIGHTING_SHADERS_H_

// Auto-generated from lib/shaders/*.{vert,frag}.
// The GLSL version line is injected by CMake based on platform:
//   macOS  -> #version 330 core
//   Win/Linux -> #version 300 es
// The .frag/.vert files themselves contain no #version directive.
// neon-common.glsl is not a shader: it is the gather loop and its inputs,
// injected below into the two neon programs that run it. neon-pieces.glsl is
// not one either: the corner development and the glow coverage table's layout,
// injected into neon.frag and the bake that writes that table. neon-sdf.glsl
// (the rounded box's distance and the band's boundaries) goes into the three
// programs that draw that edge - neon.frag, the blit and the opaque fill - and
// neon-grade.glsl (the tone map) into neon.frag and the field's composite.
// spotlight-dither.glsl (the spotlight's output noise) goes into the two
// programs that write that layer to the caller's framebuffer - spotlight.frag
// and the spotlight's blit.
// (No configure placeholder may appear in these comments - configure_file
// expands it here.)

namespace EdgeLighting
{
        namespace ShaderSource
        {
                static const char *const WIREFRAME_VERT_SRC = R"shadersrc(#version 330 core
precision highp float;

layout(location = 0) in vec2 aPos;

uniform mat4 uMVP;

void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}

)shadersrc";

                static const char *const WIREFRAME_FRAG_SRC = R"shadersrc(#version 330 core
precision highp float;

uniform vec4 uColor;

out vec4 fragColor;

void main() {
    fragColor = uColor;
}

)shadersrc";

                static const char *const NEON_VERT_SRC = R"shadersrc(#version 330 core
precision highp float;

// INVARIANT, because two programs have to agree on it to the bit. Below
// resolutionScale 1.0, NeonRenderer splits the frame between the blit
// (neon-blit.frag) and the edge ring (neon.frag's ring program): two
// vertex arrays that share their boundary vertices, so the rasteriser gives
// every pixel to exactly one of the two draws. That holds only if both programs
// compute the same gl_Position from the same aPos and uMVP - and GLSL does not
// promise that across separately compiled programs without this qualifier. The
// spec's own example of what goes wrong without it is exactly this: geometry
// misaligned between the passes of a multi-pass algorithm. Here that would be a
// 1 px seam composited twice or not at all along the ring's edge. See
// NeonRenderer::setupRingGeometry.
//
// Every renderer compiles this file, so the qualifier reaches them all. It
// constrains how the compiler may evaluate the one expression below and is not
// meant to move a pixel: measured on desktop GL, 36 captures across all five
// layers and both resolution paths are byte-identical with and without it.
invariant gl_Position;

layout(location = 0) in vec2 aPos;

out vec2 vPos;

uniform mat4 uMVP;

void main() {
    vPos = aPos;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}

)shadersrc";

                static const char *const NEON_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - the neon shaders (neon.frag, neon-emission.frag), where CMake
//     text-injects this file via @NEON_TUNING@ in shaders.h.in, and
//   - the C++ renderer (neon-renderer.cpp), which #includes it for the
//     glow-reach quad-sizing factors.
//
// The px constants below are written in FULL-RESOLUTION pixels. Where the
// renderer draws at a reduced NeonConfig::resolutionScale, the shader converts
// each one with uResolutionScale at the point of use; the notes on the
// individual constants say which need it and which do not.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a `const float X = 0.9f;` cannot
// be written once and stay valid in both languages. Plain #define is the only
// form that compiles identically as GLSL and as C++ from a single file.
// (For C++-only constants, prefer constexpr - macros are used here strictly
// because these values must also live inside the shaders.)
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
//
//     FILAMENT_MIN_HALF_WIDTH is a STATED-width floor: it is what makes
//     lineWidth 0 mean "no line" rather than a single bright dot, and it is
//     paired with lineGate, which fades the filament in over the same span. It
//     describes the width the caller asked for, so it is a FULL-RES px constant
//     and the shader converts it with uResolutionScale like every other one.
//
//     The SAMPLING floor is the pair below, and the distinction from the
//     stated floor is the whole point of having two of them. Below
//     resolutionScale 1.0 the gather rasterises into a reduced buffer and
//     neon-blit.frag bilinearly upsamples it, so a filament the buffer cannot
//     sample is not reconstructed - it is resampled, and what comes out
//     depends on where the rect edge happens to fall between buffer texel
//     centres. That is a property of the BUFFER, so these are in BUFFER px and
//     are NOT converted with uResolutionScale.
//
//     Getting that backwards is what made a 1 px line at scale 0.5 disagree
//     with the same line at 1.0. sigma was floored only by the converted
//     stated constant, which at scale 0.5 is 0.25 buffer px, so the line sat
//     under the buffer's Nyquist limit and its peak swung with the rect's
//     sub-pixel position - 217 on a texel centre, 180 half a texel off,
//     against a 1.0 reference that holds 230-241 at every position.
//
//     THE FLOOR IS NOT A FIXED HALF WIDTH. It was one - a flat 0.5 buffer px -
//     and a fixed half width asks the wrong question. What survives the blit
//     is not decided by the profile's width at half maximum; it is decided by
//     how much signal the NEIGHBOURING buffer texel still carries, because
//     that is what the bilinear filter rebuilds the peak from. So the floor
//     says exactly that: the profile must still be at
//     FILAMENT_NYQUIST_MIN_SHARE of its peak, FILAMENT_NYQUIST_SAMPLE_PX out.
//     Inverting the generalized Gaussian for sigma gives
//
//         sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
//
//     which neon.frag and NeonRenderer::setupGeometry both evaluate.
//
//     0.0625 and 1.0 are today's behaviour restated, not a retune: at the
//     DEFAULT falloff (N = 2) the expression is exactly 0.5 buffer px, the
//     constant it replaces, so nothing at or near the default moves.
//
//     The two questions diverge at a SOFT falloff, and that is what the change
//     is for. sigma does not only set the core - it multiplies reachSigmas, so
//     it sets the whole tail. At filamentFalloff 0.27 (N = 0.54) reachSigmas
//     clamps at FILAMENT_REACH_MAX_SIGMAS and the tail is 64 sigmas, so a flat
//     0.5 floor stretched a 31 px filament to 61 px at scale 0.5 and past
//     120 px at 0.25 - to buy 5 levels of peak accuracy (max per-phase error
//     13 -> 8) on a profile 31 px wide that the buffer was sampling perfectly
//     well. The share form asks for 0.082 buffer px there, below what the
//     caller already supplies, so it does not engage.
//
//     Where it DOES still engage it is doing the work it was added for: at the
//     default falloff, without any floor, the peak collapses to 153 against a
//     235 reference, and at filamentFalloff 1.5 to 24. The known cost of the
//     share form is the band around filamentFalloff 0.4 to 0.5, where it stops
//     engaging while the unfloored peak error is still 12 to 18 levels;
//     tightening MIN_SHARE to cover that would stop the default case being
//     bit-identical, so the crossover sits here deliberately. See
//     docs/corner-crease-and-filament-nyquist.md section 2.8.
//
//     The line does read WIDER at a reduced scale wherever the floor engages,
//     and that part is not a defect to tune away: a line the buffer cannot
//     sample cannot be reconstructed from it. The floor buys a stable,
//     correctly-bright line of the narrowest width the buffer can actually
//     carry. Deliberately NOT paired with an amplitude compensation to
//     conserve the line's integral - the grade at the end of neon.frag
//     tonemaps per-fragment INSIDE the buffer and FILAMENT_GAIN puts the core
//     deep in saturation, so scaling the linear amplitude barely moves the
//     8-bit value while it does measurably dim the peak (error 15-23/255
//     against 0-10 without it). See
//     docs/corner-crease-and-filament-nyquist.md section 2.
//
//     GATED to scales below 1.0 in both consumers. At scale 1.0 the reduced
//     buffer is full size and the blit copies it 1:1, so there is nothing to
//     survive, and the ring always shades at 1.0. The old flat constant was a
//     no-op at 1.0 by arithmetic
//     coincidence - it equalled the converted stated floor; this expression
//     would not be, above N = 2, so the gate is explicit now. ---
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_NYQUIST_SAMPLE_PX 1.0
#define FILAMENT_NYQUIST_MIN_SHARE 0.0625
#define FILAMENT_GAIN             12.0

// --- Continuous-arc filament gate feathers (neon.frag).
//     INWARD FEATHER: the smooth ramp sits INSIDE the arc's own perimeter
//     span, so nothing outside the arc gets lit -> no corner bleed regardless
//     of width, no perpendicular spike, and the profile is a plain smoothstep
//     that reads the same on straight edges and at corners. Trade-off: the
//     visible arc appears inset by these widths (arc lights up at
//     start + TAIL_FEATHER_PX and ends at start + length - HEAD_FEATHER_PX).
//     Values are pixel-space spans, divided by the current perimeter at the
//     call site, and capped per-arc by ARC_FEATHER_MAX_SHARE below. ---
#define HEAD_FEATHER_PX           14.0
#define TAIL_FEATHER_PX           14.0

// --- Cap on each feather, as a share of the arc's own length.
//
//     The feathers above are a fixed pixel span while an arc's length is a
//     FRACTION of the perimeter, so the same arc config is a different pixel
//     length on every rect. Below ~(HEAD + TAIL) px the two ramps overlapped
//     and ate into the arc's peak: an L = 0.02 arc peaked at 1.00 on an
//     800x600 rect but only 0.21 on a 200x150 one - the last place rect size
//     still reached brightness. Capping each feather at a share of the arc
//     length holds the peak at exactly 1.0 for any length at any size; a short
//     arc gets a proportionally shorter ramp instead of a truncated top.
//
//     0.4 leaves a 0.2 * length plateau at full brightness between the two
//     ramps. Up to 0.5 also holds the peak, but with no plateau the arc reads
//     as a spike rather than a flat-topped segment. Arcs longer than
//     (HEAD + TAIL) / 0.4 px never reach the cap, so the normal case is
//     bit-identical to before. ---
#define ARC_FEATHER_MAX_SHARE     0.4

// --- Halo (sharp coloured glow).
//
//     The halo and bloom are evaluated ANALYTICALLY from the rounded-box SDF
//     distance, not summed over the perimeter gather. For a locally straight
//     emitter of unit density the old sums converge exactly to:
//
//       sum g*sqrt(g) * spacing*kh^2*HALO_NORM  -> HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
//       sum 1/(dd+bw^2) * spacing*bw*BLOOM_NORM -> BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
//
//     so the NORM factors keep their meaning and their calibration: peak
//     values at ad = 0 are unchanged (0.86 and 1.005 respectively).
//
//     The closed form is what makes the effect geometry-independent. The
//     gather had to floor its kernel at a multiple of sampleSpacing to stop
//     128 discrete samples beading into dots, and sampleSpacing is
//     perimeter / NEON_MAX_LOOP_SAMPLES - so halo width, and via the gate its
//     brightness, both tracked the rect size, and glowRadius did nothing at
//     all until it exceeded the floor (~56 px on a 1920x1080 rect, i.e. most
//     of its usable range). An analytic profile cannot bead at any radius, so
//     no floor is needed and glowRadius sets the width directly.
//
//     Those two limits are of an INFINITE emitter, and evaluating one of them
//     at ad = the nearest-edge distance is what used to produce the interior
//     medial-axis creases - the dark wedges running in from each corner, plus
//     the central spine. A fragment on a corner diagonal has two edges equally
//     near and was lit by exactly one of them: measured against a fragment with
//     a single edge at the same distance, the ratio was 1.000 at every distance
//     tested, where physics says roughly 2.
//
//     neon.frag now sums the FINITE-SEGMENT form of the same two integrals over
//     the four straight edges instead (haloSegment / bloomSegment). Integrating
//     the same kernels from t1 to t2 along a segment at perpendicular distance
//     a gives elementary antiderivatives, and as t1 -> -inf, t2 -> +inf each one
//     reduces to exactly the limit above - so this is a strict generalisation
//     and the NORM factors keep the calibration they already had. The corner
//     sweep went from a V kinked at exactly 45 degrees (21 levels deep at
//     r = 160) to a smooth basin of 12, which is genuine falloff rather than a
//     crease. Cost is ~1.06-1.10x of the neon frame; the gather loop is still
//     the overwhelming majority of it.
//
//     The CREASE is what the four-segment form was adopted for. It is not the
//     largest thing it moves. The old expressions were functions of ad and of
//     nothing else, so the old profile was SYMMETRIC about the line - a
//     fragment 120 px inside the edge and one 120 px outside were lit
//     identically, by construction. Outside, the emitter recedes; inside, it
//     wraps around you. The segment sum knows the difference, so the exterior
//     now falls off slightly faster (mid-edge peak itself moves by 2/255) and
//     the INTERIOR stops falling off past roughly one rect-half and settles on
//     a floor. On the 1000x500 / glowRadius 60 probe the centre goes 162 ->
//     186 and the interior mean 174.5 -> 188.8; at glowRadius 30 on 800x400 it
//     is 81 -> 113. The glow reads as bigger, and the part that grew is the
//     interior - the exterior mean goes slightly DOWN.
//
//     Intended, and not tunable back out: a real rectangular tube does light
//     its own interior, and HALO_GAIN cannot be lowered to undo it without
//     dimming the line with it. The knobs for a perimeter-hugging glow are the
//     ones that always meant that - insideCutoff, and glowSide OUTSIDE.
//
//     THE CORNERS ARE A FIFTH THROUGH EIGHTH SEGMENT, not an extension of the
//     four. The straights are trimmed to their TANGENT POINTS, and each quarter
//     arc contributes one more segment: the arc DEVELOPED onto its own tangent
//     at whichever arc point is nearest the fragment, carrying the arc's full
//     length and split about that point, so it abuts the straights exactly in
//     arclength and the emitter is continuous. See neon.frag's
//     arcTangentSegment.
//
//     DEVELOPED AT RATE sqrt(rho * min(rho, r)), NOT AT RATE r, with the
//     measure put back by scaling the segment by r/rate - `rho` being the
//     fragment's distance from the arc centre. Laying an arc out one unit of
//     tangent per unit of arc is only right for a fragment ON it; the exact
//     distance to the point dphi away is a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,
//     whose slope at the foot is sqrt(rho*r). Rate r cost two things:
//
//       - at the CENTRE OF CURVATURE the whole arc ran off to one side of the
//         foot, and both kernels peak at t = 0, so it under-counted - to 54%
//         of the true value on a circle, where all four centres coincide at
//         the middle of the shape;
//       - and it CREASED, because the arc's endpoints slide at the tangent
//         rate on one side of the clamp and at rate 1 on the other, which
//         agree only where the rate is length(w). Every other point of the two
//         lines through an arc centre carried a C1 kink: an L-shaped seam per
//         corner, and on a circle an unmistakable dark cross. See
//         docs/review-findings.md V12.
//
//     rho == r gives rate r and weight 1, so a fragment on the tube is
//     bit-identical to the rate-r form and this calibration is untouched. The
//     whole emitter's worst error against a numerically integrated perimeter
//     falls on every geometry tested (16.5 -> 9.8% on a 600x400 r=40,
//     37.3 -> 26.3% on a circle).
//
//     The straights used to run to the SHARP corner instead, on the grounds
//     that over-extending them past the tangent point stood in for the arc that
//     has no elementary closed form. It does not stand in for it. The
//     over-extension is a phantom emitter a few px from a fragment that is tens
//     of px from the real tube: on an 800x400 rect at cornerRadius 40, a point
//     20 px outside the arc on the diagonal sits 2.4 px from EACH of the two
//     phantoms, and rendered 98 -> 146 across the commit that introduced them.
//     Against a numerically integrated rounded-rect perimeter it was 40 levels
//     too dark ON the tube and 50 too bright just outside it, which is what
//     made a rounded corner read square at a narrow glowRadius.
//
//     The developed arc costs one extra segment per corner - one atan and a
//     length on top - and takes the worst error over a quadrant from 76 to 6
//     levels at glowRadius 5, 112 to 6 at cornerRadius 120, 130 to 8 at 200.
//     A full circle (cornerRadius == halfMin) is still the residual case,
//     since its perimeter is four developed arcs and no straights at all.
//
//     Gated on uCornerRadius > 0, which is a UNIFORM - safe branching, and the
//     reason a sharp rect pays nothing and stays bit-identical (nine scenes
//     cmp-equal, not argued, and still cmp-equal after the rate fix above).
//     Rounded costs 1.19x of the neon pass against no arcs at all, and the rate
//     fix adds 1.03x on top - but the sharp path pays 1.026x of THAT for code
//     it never runs, because what decides this block is REGISTER PRESSURE, not
//     structure: with a per-arc bloom pedestal it was heavy enough to cost the
//     sharp path 1.14x. Re-time a cornerRadius 0 scene as well as a rounded one
//     after touching this. See docs/corner-crease-and-filament-nyquist.md
//     sections 1.7 to 1.9, and docs/review-findings.md V4, V10 and V12. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Width of the COLOUR gather kernel, as a FRACTION OF THE PERIMETER.
//
//     The perimeter colour blend is the one part of the shader that is still a
//     discrete sum over the loop samples, and it is the only place a length has
//     to be expressed this way rather than in pixels. The reason is that the
//     signal it filters - the gradient LUT - is itself parameterised by
//     perimeter fraction: sample i contributes LUT(i / NEON_MAX_LOOP_SAMPLES).
//     A kernel measured in pixels therefore covers a DIFFERENT span of the
//     gradient on every geometry, and the same colour stops render washed out
//     on a small rect and crisp on a large one. Measured on the stock 4-stop
//     ring, the colour sampled exactly on the red stop ran (0.95, 0.33, 0.06)
//     at 200x150 against (0.96, 0.04, 0.00) at 2800x2200 - roughly 8x the hue
//     bleed from geometry alone. Scaling the kernel with the perimeter is what
//     makes the gradient read identically at any size.
//
//     Two further properties fall out of the same choice:
//
//       - A constant anti-bead margin. The kernel has to stay >= about one
//         sample spacing, or it collapses between samples and the blend beads
//         into dots. Spacing is perimeter / NEON_MAX_LOOP_SAMPLES, so a
//         perimeter fraction pins the ratio at
//         COLOR_BLEND_PERIM_FRAC * NEON_MAX_LOOP_SAMPLES = 1.13 spacings on
//         every geometry, where the previous fixed 24 px span met the bound
//         only up to a ~3000 px perimeter and was down to 0.31 spacings by
//         9900 px. (No beading was actually measurable there - a DFT of the
//         hue around the perimeter put the 128-cycle component at the noise
//         floor - because the gradient varies slowly over one spacing on a
//         rect that large. The margin is a guarantee for dense colour stops,
//         not a fix for an observed artifact.)
//
//       - Sample-count independence. The kernel comes from the
//         NEON_MAX_LOOP_SAMPLES-based fraction rather than from the runtime
//         sample count, so the numSamples knob does not move the colour
//         blend. (An earlier sampleSpacing-derived floor divided by the live
//         count and landed 2x wider at reduced counts.)
//
//     Deliberately NOT coupled to glowRadius. The gather produces colour only -
//     the halo and bloom have been closed-form since they stopped riding on it -
//     so a wide glow has no reason to desaturate the gradient, and the old
//     max(glowRadius, floor) reintroduced exactly the pixel-space dependence
//     this constant exists to remove.
//
//     0.0088 puts the kernel at 24.0 px on the stock 800x600 / radius 40
//     geometry, matching the previous fixed span, so default-sized output is
//     unchanged.
//
//     NOTE: unit-free, unlike the px constants around it. The shader
//     multiplies it by a perimeter that is already in scaled px, so it needs
//     no uResolutionScale correction - applying one would double-apply. ---
#define COLOR_BLEND_PERIM_FRAC    0.0088

// --- Emission on/off ramp. glowRadius = 0 must read as "filament only", but
//     an analytic profile at radius 0 is a sub-pixel spike of full height
//     rather than nothing, so the halo and bloom fade in over
//     glowRadius = [0, this]. A FIXED pixel width: gating against the
//     sampleSpacing-derived floor instead would re-couple brightness to the
//     rect size, which is the whole thing this design removes.
//
//     NOTE: full-res pixel span, compared against a uGlowRadius that arrives
//     already scaled, so the shader multiplies this by uResolutionScale at
//     the point of use. Identity at scale 1.0. ---
#define GLOW_GATE_FADE_PX         2.0

// --- Lower bound on the analytic emission widths. Guards the divides only;
//     anything this small is already multiplied out by GLOW_GATE_FADE_PX. ---
#define EMISSION_MIN_WIDTH        1e-3

// --- Degenerate guard for the corner arc's tangent frame (neon.frag,
//     arcTangentSegment). The frame is built by normalising the fragment's
//     offset from the arc centre, clamped into the quarter the arc occupies;
//     that offset is the zero vector only for a fragment sitting exactly on an
//     arc centre, where the direction is undefined and atan(0, 0) is undefined
//     with it. Below this the frame falls back to the nearer of the two tangent
//     points, which is the correct clamp for the whole region the guard covers.
//
//     It floors `rho` in the same function, for the same fragment: the
//     development rate divides into the segment's weight, so an exactly-zero
//     rho would be a division by zero. The floored value is nowhere near a
//     pixel and the limit it lands on is the exact answer anyway, so this only
//     keeps a NaN out.
//
//     Unit-free: compared against a length in the shader's own px space, and
//     small enough that nothing but the exact degeneracy reaches it. ---
#define ARC_FRAME_EPSILON         1e-6

// --- Bloom (wide background spill). See the halo note above for the closed
//     form; BLOOM_SPACING_FLOOR is gone with the gather. ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_NORM_FACTOR         0.32

// --- Travelling-segment array size (shared by C++ vector cap + GLSL uniform array) ---
#define MAX_SEGMENT_BOOSTS        8

// --- Arc range array size (shared by C++ vector cap + GLSL uniform array).
//     Each arc gates a slice of the perimeter with its own colour + intensity.
//     Overlap resolves winner-take-all (max mask*intensity wins). ---
#define MAX_ARCS                  8

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO and
//     the shader's array. The gather iterates only uNumSamples of them
//     (NeonConfig::numSamples, which defaults to this), so it is a ceiling,
//     not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Glow coverage table (neon-glow-cover.frag, V20 and V21): for each piece
//     of the emitter and each fragment position round it, the arcs' and the
//     segments' coverage of that piece as the halo and the bloom see it. Laid
//     out in neon-pieces.glsl: four bands of ROWS rows, each holding one
//     straight at its left and one corner at its right. A band's columns are
//     shared between the two in proportion to their lengths (uGlowCoverSplit,
//     NeonRenderer::GetGlowCoverSplit), so a circle's corners get the columns
//     its straights do not need and a long rect's straights get its corners':
//     the straight has OVERHANG columns past each end, the corner
//     CORNER_OVERHANG on each side of its arc and a guard texel at each end of
//     its block, and the columns left over (the layout's `shared`) are split
//     between the two interiors, each keeping at least MIN_INTERIOR. ROWS 32 is half of them
//     within one halo width of a straight, and 16 inside and 16 outside a
//     corner's arc. 1024 x 128 RGBA16F is 1.0 MB - half V20's single table per
//     perimeter. Sizing it, against an exact per-piece reference (V21 in
//     docs/review-findings.md): 48 rows (1.5 MB) read at most 2 levels off where
//     32 reads 3 on a few pixels of a segment scene, for 1.3x the bake's cost
//     under animation; 768 columns read 6 levels off a 1 px halo on a 4K-sized
//     rect.
//
//     The WIDTH is not a constant: the table's layout is a parameter
//     (NeonRenderer's GlowCoverLayout, the shaders' uGlowCoverLayout), between
//     MIN_WIDTH and MAX_WIDTH in steps of WIDTH_STEP. ROWS and HEIGHT are. ---
#define GLOW_COVER_MAX_WIDTH      1024
#define GLOW_COVER_MIN_WIDTH      256
#define GLOW_COVER_WIDTH_STEP     64
#define GLOW_COVER_ROWS           32
#define GLOW_COVER_OVERHANG       64
#define GLOW_COVER_CORNER_OVERHANG 32
#define GLOW_COVER_MIN_INTERIOR   16
#define GLOW_COVER_HEIGHT         (4 * GLOW_COVER_ROWS)

// --- The glow coverage table's width (NeonRenderer's GetGlowCoverWidth):
//     enough interior columns that the longest band - its straight plus its
//     quarter arc, L full-res px - gets one every S px, S growing with the
//     halo width kh (max(glowRadius, EMISSION_MIN_WIDTH), px):
//
//         S    = clamp(PX_PER_COLUMN_PER_KH * kh, MIN_PX_PER_COLUMN, MAX_PX_PER_COLUMN)
//         need = 2 + 2 OVERHANG + 2 CORNER_OVERHANG + ceil(L / S)
//         W    = clamp(need rounded up to WIDTH_STEP, MIN_WIDTH, MAX_WIDTH)
//
//     In full-res px because the edge ring reads the table at full
//     resolution; lengths enter the layout only as ratios, so every scale
//     shares one table.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the 1024-wide
//     table (docs/neon-glow-cover-resolution-plan.md step 2): 250 configs -
//     rects 200 x 120 to 3600 x 2000, glowRadius 1 / 2 / 5 / 10 / 20,
//     cornerRadius 0 / 40 / a circle, both windings, three partial arcs with
//     and without colour stops and with two segments, at scales 1.0 / 0.5 /
//     0.25 - each at every width from 256 to 960, the frame after a light
//     change, against the same frame at 1024. The widest spacing that still
//     met the plan's criterion (max 2 levels, 99.9% of lit pixels within 1)
//     on every config was ~2.2 px per column at kh 1, 2.5 at 2, 4.6 at 5, 6.8
//     at 10 and 10 at 20. These constants sit under that envelope - 12% at
//     kh 5, 27% or more elsewhere - and at the widths they give, every config
//     read at most 2 levels with at least 99.988% of lit pixels within 1.
//     Spacing 20% wider failed at kh 5 (4 levels on a 200 px circle). A
//     narrow glow on a large rect still gets MAX_WIDTH: at kh 1-2 a
//     3600 x 2000 rect needed all 1024 columns. ---
#define GLOW_COVER_PX_PER_COLUMN_PER_KH 0.8
#define GLOW_COVER_MIN_PX_PER_COLUMN    1.6
#define GLOW_COVER_MAX_PX_PER_COLUMN    5.0

// --- The segment table (NeonRenderer's GetGlowCoverSegmentWidth,
//     neon-glow-cover-fill.frag): the segments' two channels of the glow
//     coverage table, baked at a width of their own and copied in. A bell's
//     standard deviation is 0.3536 x its length x the perimeter - ~100 px for
//     a length-0.05 segment on a 1840 x 1000 rect, against an arc's 14 px
//     feather - so it needs far fewer columns than the arcs:
//
//         sigma = 0.3536 x the shortest lit segment's length x perimeter   (full-res px)
//         need  = 2 + 2 SEG_OVERHANG + 2 SEG_CORNER_OVERHANG + ceil(L / (sigma / SEG_COLUMNS_PER_SIGMA))
//         W     = clamp(need rounded up to WIDTH_STEP, SEG_MIN_WIDTH, the main table's width)
//
//     with L the longest band, as for the main table. Where W would be at
//     least SEG_DIRECT_SHARE of the main table's width (very short segments)
//     the separate table saves nothing, and the segments are baked into the
//     main table directly, as before it existed.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the segments
//     baked into the main table directly (plan step 3): 135 configs - rects
//     200 x 120 to 3600 x 2000, glowRadius 2 / 5 / 10, cornerRadius 0 / 40 /
//     a circle, scales 1.0 / 0.5 / 0.25, segments of length 0.01-0.2 with
//     boosts 0.5-2, overlapping, abutting, over the corners, travelling, and
//     under a partial arc - the frame after a light change, the segment table
//     forced on. With overhangs 16 / 8, every COLUMNS_PER_SIGMA from 3 to 8
//     read at most 2 levels with 99.9992% of lit pixels within 1; 2 read 3
//     levels on a thin glow at 3840 x 2160. 4 is one step of margin. Overhangs
//     8 / 4 held at every value tried (99.988%), 32 / 16 read at most 1 level
//     from 4 up but do not fit SEG_MIN_WIDTH. ---
#define GLOW_COVER_SEG_COLUMNS_PER_SIGMA 4.0

// --- How far the glow coverage bake integrates a segment's bell either side
//     of its centre, in units of 1 / invSigma (the bell is exp(-(d invSigma)^2),
//     so 3 is 4.24 standard deviations, where it has fallen to 1.2e-4 of its
//     peak). The bake (neon-glow-cover.frag, segmentsOnPiece) cuts the bell
//     here and NeonRenderer's GetGlowCoverDirtySegmentPieces mirrors it to
//     find the pieces a moved segment reaches: change one and the other
//     follows, or a piece the bell reaches is not re-baked. It was 5 / sqrt(2)
//     (5 sigma); at 3 a moved segment dirties fewer pieces, which took the
//     segment table's bake on a band of 8 segments from 0.75 to 0.58 (timer
//     units, AMD Radeon Pro 5300M), at most 1 level, on up to 1.6% of a
//     frame's lit pixels (0.15% on average over the cover set; I61 in
//     docs/review-findings.md). An 8-node rule on top halved it again but
//     read 3 levels off a long, bright segment, and was not taken. ---
#define GLOW_COVER_BELL_REACH            3.0
#define GLOW_COVER_SEG_OVERHANG          16
#define GLOW_COVER_SEG_CORNER_OVERHANG   8
#define GLOW_COVER_SEG_MIN_WIDTH         128
#define GLOW_COVER_SEG_DIRECT_SHARE      0.75

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- The output dither (R7), peak to peak in 8-bit levels: neonDither
//     (neon-grade.glsl) adds noise in +/- half this to every colour the neon
//     writes to the caller's framebuffer - the blit, the edge ring and the
//     ring's field composite, never an offscreen buffer. The halo and bloom
//     are slow gradients that cross a level only every few px, up to tens of
//     px in the dark tail, so an undithered write draws concentric contour
//     rings around the rect. 1.0 is the least that removes them; 0 turns the
//     dither off. It assumes an 8-bit target. ---
#define OUTPUT_DITHER_LSB         1.0

// --- Epsilons ---
#define WSUM_EPSILON              1e-6

// --- One-sided cut guard band, in BUFFER pixels. Scaled path only.
//
//     One of the two constants in this file stated in buffer px rather than
//     full-res px (BLIT_CUTOFF_GUARD_PX below is the other): it describes the
//     BUFFER's own sampling, so it must NOT be converted with uResolutionScale.
//
//     For pass 1b the one-sided cut is not applied by neon.frag at all -
//     neon-blit.frag applies it at DESTINATION resolution, where a pixel is a
//     pixel and the edge can land exactly where the full-resolution shading
//     puts it. What neon.frag still does is cull the dark side, and the bound
//     it culls at cannot be the cut itself: the blit reconstructs each
//     destination pixel from the 2x2 buffer texels around it, so a texel
//     killed at the cut leaves the first LIT destination pixel rebuilt partly
//     from black. Measured at scale 0.5, glowSide OUTSIDE: the first lit pixel
//     came back at 178 against the 239 full resolution puts there, a dark seam
//     hugging the inside of the glow's own edge.
//
//     So the cull runs this far PAST the cut, and the guard band's emission is
//     what the blit's filter reconstructs the boundary from. One buffer texel
//     is the filter's reach; 2.0 covers that with room for the diagonal, where
//     fwidth of an SDF is sqrt(2) rather than 1, and for the half-texel of
//     phase between the cut and the nearest texel centre.
//
//     The band costs a two-texel strip along the perimeter - nothing beside
//     the half-plane the cull still removes - and none of it is visible: the
//     blit's mask is zero everywhere past the cut, including at the hard step
//     where this bound finally discards.
//
//     NOT used by the edge ring, which owns its own cut (uBlitOwnsCut 0). See
//     neon.frag's sideCull and the post-grade cut block. ---
#define BLIT_SIDE_GUARD_PX        2.0

// --- Cutoff guard band, in BUFFER pixels. Pass 1b only.
//
//     BLIT_SIDE_GUARD_PX's argument, applied to the inside/outside cutoffs.
//     For pass 1b neon-blit.frag applies the cutoff masks at
//     DESTINATION resolution, and neon.frag only culls - this far past the end
//     of each ramp, so the blit's bilinear filter rebuilds every boundary from
//     lit texels rather than from black. Same value as the side guard, for the
//     same reasons: one buffer texel of filter reach, plus room for the
//     diagonal and for the phase between the boundary and the nearest texel
//     centre.
//
//     It replaces CUTOFF_SOFT_FLOOR_PX, a feather in buffer px that let a mask
//     drawn INTO the buffer place its boundary sub-texel. That was the best the
//     buffer could do, and it still softened the boundary across a buffer
//     texel and snapped it toward the buffer grid: on the bounded_band scene of
//     docs/neon-resolution-scale-comparison.html it read p99 41/255 against the
//     1.0 render at scale 0.5, and 69 at 0.25, and the band spread past its
//     own cutoffs as the scale fell (50,703 lit px at 0.5 and 53,568 at 0.25,
//     against 47,012 at 1.0). Drawn by the blit instead it reads p99 4 at 0.5
//     and 12 at 0.25, with 47,012 lit px at every scale; what is left is the
//     reduced-resolution glow inside the band, not its edges. See
//     docs/neon-resolution-scale-plan.md, step 2.
//
//     NOT used by the edge ring, whose masks and culls are its own. See
//     neon.frag's cutGuard and NeonRenderer::setupGeometry's cutGuardPx. ---
#define BLIT_CUTOFF_GUARD_PX      2.0

// --- Edge ring width. CPU only.
//
//     At every scale, NeonRenderer redraws a ring around the rect
//     edge at FULL resolution (neon.frag's ring program),
//     reading only the gather's result from the gather buffer. The ring reaches R
//     full-res px either side of the edge (GetRingWidth in neon-renderer.cpp):
//
//         R = max(reach(1.0), reach(scale)) + RING_GUARD_TEXELS / scale
//
//     where reach(s) is the filament's reach as neon.frag computes it at scale
//     s, in full-res px. BOTH, because inside R the reduced pass's filament -
//     floored to what its buffer can sample, so wider than the real one
//     whenever the line is thin - is replaced by the real one, and R has to
//     clear whichever reaches further. Uncapped: the reach is where a filament
//     has fallen to FILAMENT_CUTOFF of its gain, about 2/255 after the grade,
//     which is exactly how far its error can show. RING_GUARD_TEXELS adds the
//     reduced buffer's bilinear footprint past that.
//
//     Calibrated over lineWidth {1, 2, 4, 8, 16} x filamentFalloff
//     {0.5, 1, 2, 4} x glowRadius {0, 2, 5, 20}, each config rendered at
//     scales 0.75, 0.5 and 0.25 and compared against its own 1.0 render
//     (docs/neon-resolution-scale-plan.md, step 5). Worst error over all 80:
//
//       RING_GUARD_TEXELS    scale 0.75    scale 0.5    scale 0.25
//              0.5               3            12            3
//              1.0               4             2            2
//              2.0               2             2            2
//
//     1.0 is the smallest that holds 2/255 at 0.5 and 0.25, the targets. 2.0
//     also brings 0.75 within 2 (its worst at 1.0 is a 1 px flat-top line
//     with no glow) for a ring one buffer texel wider each side.
//
//     The two things that made the first version of this ring fail that sweep,
//     both now handled: R used to cover only the full-res filament (fixed by
//     taking the reduced pass's too, above), and at a small glowRadius pass 1's
//     quad stopped short of R, so the ring read gather texels nothing wrote
//     (fixed by covering the ring with the quad; since the gather was split
//     out, the gather pass's own quad, built in setupRingGeometry, covers
//     it). ---
#define RING_GUARD_TEXELS         1.0

// --- Gather resolution. CPU only.
//
//     At every resolutionScale the gather - ~95% of neon.frag's cost when it
//     ran inline - runs in a pass of its own (neon-gather.frag) at its own
//     scale, and pass 1 and the
//     edge ring shade from its result. Its scale is (GetGatherScale in
//     neon-renderer.cpp)
//
//         clamp(GATHER_TEXELS_PER_KERNEL / kc, GATHER_MIN_SCALE,
//               min(resolutionScale, GATHER_MAX_SCALE))
//
//     with kc the colour kernel's width in full-res px, perimeter *
//     COLOR_BLEND_PERIM_FRAC: the gather's outputs are Lorentzian-weighted
//     means whose kernel is never narrower than kc, so this many texels per kc
//     carries them through a bilinear read.
//
//     Calibrated on Mesa llvmpipe with an RGBA16F gather buffer (worst error
//     against 1.0 over `probe scenes`, and the default scene's frame time):
//
//       GATHER_TEXELS_PER_KERNEL   worst error   default 0.5   default 0.25
//                1.0                   4              -              -
//                1.5                   2           10.5 ms        8.0 ms
//                2.0                  1-2          10.8 ms        7.3 ms
//                4.0                   -           14-17 ms      11-12 ms
//         (gather at resolutionScale)  1-2          37.5 ms       12.3 ms
//
//     2.0 is the knee: 1.5 saves nothing measurable (the gather is no longer
//     the pass that costs) and 4.0 costs half as much again for no visible
//     gain. With an RGBA8 gather buffer - the fallback - 2.0 reads 3/255.
//     GATHER_MIN_SCALE barely matters - full-viewport rects
//     at 720p and 1080p read 1-2 at a floor of 0 and 0.0625 alike - and is
//     kept as a cheap guard: the gather buffer is clipped to the viewport, so
//     at the floor it is at most 128 x 80 texels at 1080p. See
//     docs/neon-resolution-scale-plan.md section 13. ---
#define GATHER_TEXELS_PER_KERNEL  2.0
#define GATHER_MIN_SCALE          0.0625

// --- The gather's ceiling. CPU only.
//
//     GetGatherScale never gathers finer than this, at any resolutionScale.
//     It binds only on a rect whose perimeter is under ~450 px (kc under 4
//     px), which would otherwise gather near full resolution: its gather
//     buffer - RGBA16F over the glow quad's box, two attachments with
//     segments - ran to 16 / 33 MB at 1080p for a 40 x 24 rect whose glow
//     fills the frame. At 0.5 that is a quarter, the same as the scaled path
//     at 0.5 always gathered at, and the check suite's smallest rect (perimeter
//     478 px) is not touched.
//
//     It replaces the split gate at 1.0 (FULL_RES_SPLIT_GATHER_MAX_SCALE /
//     FULL_RES_SPLIT_MIN_AREA_PX, removed - docs/neon-shader-cleanup-plan.md
//     step 3), which ran the gather INLINE in neon.frag where splitting it
//     out did not pay: on an AMD Radeon Pro 5300M, where the first offscreen
//     pass of a frame costs ~0.15 ms, for a still cutoff band under ~140k px
//     of quad and for a gather near full resolution. Since unchanged frames
//     stopped re-running the gather (I40) the still case no longer leaves
//     the caller's framebuffer at all; an Apple M2 Pro measured the split
//     1.1-4.75x faster at every size and mode, and the AMD, re-measured with
//     every rect split against its own gate, 0.98-1.02x in every animated
//     mode with one loss - a cutoff band under an intensity pulse, 0.84x.
//     So every scale splits, and the inline loop went with the gate. ---
#define GATHER_MAX_SCALE          0.5

// --- Stand-in distance for a cutoff that glowSide has already subsumed.
//
//     neon.frag hands this to the band distance on the side the one-sided cut
//     culls, so the discard and the mask there no-op through the arithmetic
//     exactly as a DISABLED cutoff does - same mechanism, same sentinel shape,
//     no extra branch downstream.
//
//     Matches CUTOFF_DISABLED_SIZE in neon-renderer.cpp, which is what a
//     disabled cutoff actually arrives as. It is a distance in the shader's own
//     px space and only has to be far enough that no realistic geometry reaches
//     it; see the note on that constant for the same reasoning. ---
#define CUTOFF_NEUTRALISED        1.0e6

// --- Glow reach (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity).
//
//     Named for the reach, not for an early-out: the per-fragment
//     `ad > earlyOut -> discard` this constant was originally calibrated for
//     no longer exists. Geometry culls the far region instead, which is
//     tiler-friendly, so what the factor sets is how far the glow is allowed to
//     reach before the quad stops covering it. The locals it feeds already say
//     so - `glowReach` in both setupGeometry implementations, `reach` in both
//     shaders.
//
//     glowRadius only: the companion sampleSpacing * SPACING_FACTOR term is
//     gone. sampleSpacing is perimeter / NEON_MAX_LOOP_SAMPLES, so it won on
//     any reasonably large rect at default glowRadius and made both the
//     distance the bloom got truncated at and the brightness it still had
//     there track the rect size. The bloom's reach is a function of glowRadius
//     alone, so that is all that sizes the quad.
//
//     Used by the renderers' setupGeometry AND by the shaders, which recompute
//     the same expression to place the bloom pedestal that lets this margin
//     stay tight without the truncation showing. Keep the two in step. ---
#define GLOW_REACH_RADIUS_FACTOR  48.0

// --- Where the shaders' quad-edge fade begins, as a FRACTION of the quad
//     margin. The emission ramps to zero over [FRAC * margin, margin], so the
//     bloom's 1/D tail - still ~10% of peak out at the quad edge - never clips
//     as a hard rectangle.
//
//     A fraction, not a pixel span, because the margin is proportional to
//     glowRadius (see GLOW_REACH_RADIUS_FACTOR) and so is the bloom profile it
//     hides. A fraction keeps the ramp at a constant proportion of the bloom's
//     reach, so the fade reads the same at every glow radius; a fixed px ramp
//     would vanish on a wide glow and dominate a narrow one. At the stock
//     glowRadius 5 / bloom 0.3 / intensity 1 the margin is 312 px, giving a
//     62 px ramp.
//
//     Nothing derives 0.8 - anything leaving a ramp wide enough to hide the
//     clip behaves the same. What it DOES assume is that the margin was set by
//     the glow, where 20% of it is a long distance. When outsideCutoff clamps
//     the margin instead (to where its fade ends, size + softness, plus 1),
//     20% of ~13 px is 2.6 px and
//     the ramp lands INSIDE the cutoff band, dimming the band's outer edge
//     ahead of the cutoff mask - and only on the exterior, since the fade keys
//     on positive d. Both shaders therefore floor the ramp's start at the
//     cutoff boundary whenever that boundary falls inside the quad; see the
//     fadeStart block in neon.frag.
//
//     Since V23 the bloom no longer relies on this fade to end smoothly - each
//     piece fades by its own distance from BLOOM_FADE_START_FRAC, below, which
//     has it under a level well before this ramp starts. ---
#define QUAD_FADE_START_FRAC      0.8

// --- Where each piece's bloom starts fading out, as a FRACTION of `reach`
//     (neon.frag's bloom end fade, V23).
//
//     The pedestal takes the bloom's 1/a tail to exactly 0 at `reach`, but
//     with its slope still on, so the glow stopped on a crease: inside a large
//     rect, a hard-edged dark rectangle `reach` in. The quad-edge fade above
//     hid it outside, but over [0.8, 1.0] of the margin, where the tail is
//     still ~5 levels up, and a fade that late STEEPENS the tail before it
//     flattens it - a shoulder, then flat, which reads as an edge too. So
//     each piece's bloom is multiplied by 1 - smoothstep(FRAC * reach, reach,
//     a) of its own distance, on both sides.
//
//     0.5 is about the latest start that does not steepen it: the steepest
//     slope inside the fade over the slope where it starts, on the
//     infinite-line bloom at glowRadius 5, bloomStrength 0.52, intensity 0.8,
//     is 1.40 at 0.8, 1.14 at 0.6, 1.035 at 0.5 and 1.00 from 0.4 down. (It
//     is also where c * (reach - a)^2 meets the pedestalled 1/a tail in value
//     and slope.) An earlier start costs more of the tail; at 0.5 the glow
//     reads up to 6 levels dimmer in the outer half of its reach. ---
#define BLOOM_FADE_START_FRAC     0.5

// --- Filament reach for the same quad sizing, expressed in sigmas.
//
//     glowRadius = 0 means "filament only" - the halo and bloom are gated off
//     by GLOW_GATE_FADE_PX - but the filament itself is still there, sized by
//     lineWidth, so the quad has to cover it or the exterior gets clipped.
//
//     The reach is NOT a constant, because the profile is
//     exp2(-(ad/sigma)^N) with N = 2 * filamentFalloff and the slider runs
//     down to falloff 0. Solving core * FILAMENT_GAIN < FILAMENT_CUTOFF gives
//
//         reach = sigma * log2(FILAMENT_GAIN / FILAMENT_CUTOFF) ^ (1/N)
//
//     which is 1.9 sigmas at falloff 2.0, 3.5 at 1.0, 12.6 at 0.5, 67.8 at 0.3
//     and 558 at 0.2 - it diverges as falloff -> 0. A fixed 12 (calibrated for
//     falloff 0.5) left the low end badly under-sized: the interior is
//     unbounded because the quad always covers it, so a soft filament flooded
//     inward while the exterior was chopped at 12 sigmas. That asymmetry is
//     the "no outer glow at glowRadius 0" report.
//
//     MAX_SIGMAS bounds the fill cost where the formula diverges; the shaders
//     pedestal-subtract the core at exactly this reach, so hitting the cap
//     shortens the glow symmetrically on BOTH sides instead of showing a cliff
//     on the outside. MIN_SIGMAS keeps a little room at very high falloff.
//     Keep the expression in step across neon-tuning.h, both setupGeometry
//     implementations, and both fragment shaders. ---
#define FILAMENT_CUTOFF           0.002
#define FILAMENT_REACH_MIN_SIGMAS 2.0
#define FILAMENT_REACH_MAX_SIGMAS 64.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// What neon.frag and neon-gather.frag share
// ---------------------------------------------------------------------------
//
// Not a shader on its own. lib/CMakeLists.txt reads it and shaders.h.in
// injects it into both, after the version line and @NEON_TUNING@ and ahead of
// the file's own source - so everything here is declared before either file's
// first line, and neither declares any of it again.
//
// It is what the gather pass and the shading that reads it back both declare:
// the fragment position, the rect's shape, the segment block and the
// perimeter's length. The gather loop itself is neon-gather.frag's alone -
// it used to live here too, compiled out of neon.frag's reading variant, while
// neon.frag could also run it inline at 1.0; that path is gone
// (docs/neon-shader-cleanup-plan.md step 3).

precision highp float;

in vec2 vPos;

uniform vec2  uRectSize;
uniform float uCornerRadius;

// Travelling segments - up to MAX_SEGMENT_BOOSTS independent coloured lights
// on the perimeter. Each vec4 is packed as (position, invSigma, boost,
// hasStops): when .w > 0.5 the segment's colour comes from row `s` of
// uSegmentLUT (its own head-to-tail gradient); when .w == 0 it inherits the
// current base gradient sample. When uSegmentCount == 0 the whole feature is
// skipped in the gather loop.
//
// Here rather than in neon.frag because the gather branches on uSegmentCount;
// every neon program reads it, whichever file it is built from.
//
// Declared in a std140 uniform block (the DALi PunctualLightBlock pattern):
// DALi writes one element per registered property ("uSegments[0]", ...) into
// the block's UBO at the reflected std140 array stride. On desktop GL the
// block is fed from a UBO in neon-renderer.cpp.
layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

const float PI      = 3.141592653589793;
const float TWO_PI  = 6.283185307179586;
const float HALF_PI = 1.5707963267948966;

// Perimeter of the rounded rect, in the px uRectSize is in - SCALED px below
// resolutionScale 1.0. Sizes the gather's colour kernel, and neon.frag also
// converts the arc feathers from px to perimeter fractions with it.
float rectPerimeter() {
    float r = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    return 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + TWO_PI * r;
}

// ---------------------------------------------------------------------------
// The emitter's pieces, shared by neon.frag and neon-glow-cover.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it, after the tuning
// header, into the two programs that have to agree on it to the texel:
// neon.frag, which sums the halo and bloom over the emitter's eight pieces and
// scales each by that piece's coverage from the glow coverage table, and
// neon-glow-cover.frag, which bakes the table. Three things live here because
// both sides need them:
//
//   - minimaxAtan, the atan both sides develop a corner with.
//   - arcTangentSegment, how a corner arc is developed onto a straight line.
//     neon.frag integrates a corner's halo and bloom along that line, and the
//     bake convolves the corner's coverage along the same line, so what scales
//     a corner's glow is the coverage of the extent that glow was integrated
//     over.
//   - the table's layout, as a forward map (where a piece's coverage is read,
//     for neon.frag) next to its inverse (what each texel holds, for the
//     bake). The two must be exact inverses, which is the reason they sit
//     side by side rather than one in each file.
//
// Every length here is in one unit chosen by the caller - px in neon.frag,
// perimeter fractions in the bake. The layout depends only on ratios of
// lengths (to the halo width kh, to a piece's own length), so the two agree
// whatever the unit and at every resolution scale.
// ---------------------------------------------------------------------------

precision highp float;

// Own names: neon-common.glsl, which neon.frag also includes, declares PI and
// HALF_PI as constants, and the bake does not include it.
#define PIECES_PI      3.141592653589793
#define PIECES_HALF_PI 1.5707963267948966

// --- A cheaper two-argument atan ----------------------------------------
// The shading's two hottest transcendentals are atans: bloomSegment's (every
// straight's bloom and every corner's, in neon.frag) and the corner
// development's `th` below. GLSL's atan(y, x) is the driver's, and measured on
// an AMD Radeon Pro 5300M this octant-reduced minimax polynomial in its place
// made every frame whose config animates 1.07-1.11x faster, at 1.0 and 0.5,
// moving at most 0.016% of a frame's channels by 1 level (I48).
// Its error is at most 1.7e-6 rad over the whole range - a few float ulps of
// PI - so nothing downstream can tell it from the built-in.
//
// Here rather than in neon.frag so the coverage bake develops the corners
// through the same function as the read: the two have to agree on `th`.
// Undefined only where atan(y, x) is (both zero): it returns 0 there.
// Measure on each target GPU before trusting the gain - a cheaper atan is the
// most GPU-specific change this file has.
float minimaxAtan(float y, float x) {
    float ax = abs(x);
    float ay = abs(y);
    float t  = min(ax, ay) / max(max(ax, ay), 1e-30);
    float s  = t * t;
    float r  = t * (0.99997726 + s * (-0.33262347 + s * (0.19354346 + s * (-0.11643287 +
               s * (0.05265332 + s * -0.01172120)))));
    r = (ay > ax) ? PIECES_HALF_PI - r : r;
    r = (x < 0.0) ? PIECES_PI - r : r;
    return (y < 0.0) ? -r : r;
}

// --- Corner arcs, developed onto their tangent -------------------------
// The four straights cover the rect's flat runs. Above cornerRadius 0 the
// emitter also turns through four quarter arcs, and a circular arc has no
// elementary antiderivative under either kernel. This develops each arc onto a
// straight line instead: the tangent at whichever ARC POINT IS NEAREST the
// fragment, carrying the arc's full length PI*r/2 and split about that point.
//
// That choice is what makes it accurate where the naive one is not. The
// perpendicular distance it reports is the true distance to the arc wherever
// the fragment faces it (|length(w) - r|), and the two halves run exactly as
// far as the real arc does in each direction, so the developed arc abuts the
// trimmed straights in arclength and the emitter is continuous - no gap and no
// overlap at the tangent points.
//
// `w` is the fragment's offset from the arc centre in that corner's own frame:
// x along the outward normal of one incident edge, y along the other's, both
// positive pointing away from the rect. So the arc occupies exactly the first
// quadrant of `w`, from +x (one tangent point) to +y (the other), and clamping
// the direction into that quadrant is max(w, 0).
//
// Off the ends the nearest arc point is a tangent point, and which one follows
// from |w - (r,0)|^2 - |w - (0,r)|^2 = 2r*(w.y - w.x): the +x end when
// w.x >= w.y. That is what the fallback picks, so it is the right clamp for the
// whole region it covers and not only for the degenerate point that forces it.
//
// --- THE DEVELOPMENT RATE IS NOT r. ------------------------------------
// Laying the arc out at its own arclength - one unit of tangent per unit of
// arc - is only right for a fragment ON the arc. The exact distance to the
// point at angle dphi from the nearest one is
//
//     D^2 = a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,   rho = length(w)
//
// so the tangent coordinate the kernels actually want is t = 2*sqrt(rho*r)*
// sin(dphi/2), whose slope at the foot is sqrt(rho*r), not r. Develop at rate
// `lam` instead of r and the emitter comes out short or long, so the measure
// is put back by scaling the whole segment by r/lam - `w` of the returned
// vec4. Rate x weight is r either way, so the arc always carries its full
// PI*r/2 of emitter.
//
// Two things follow, and they are the whole reason for the change:
//
//   - AT THE CENTRE OF CURVATURE IT IS NOW EXACT. rho -> 0 collapses the
//     segment to zero length against an infinite weight, and the limit is
//     f(r) * PI*r/2: every point of the arc at distance r, which is what a
//     fragment at the centre actually sees. At rate r the arc ran off to one
//     side of the foot instead, and since both kernels peak at t = 0 that
//     UNDER-counted - on a circle, where all four arc centres coincide at the
//     middle of the shape, to 54% of the true value.
//   - THE CLAMP STOPS CREASING. Crossing w.y = 0 the arc's endpoints slide at
//     -lam * d(th) on the facing side and at -d(off) on the clamped side; the
//     first is -lam/w.x and the second -1, and they agree only where lam is
//     length(w). At rate r they agreed only at w.x == r, so every other point
//     of the lines through the arc centre carried a C1 crease - a dark cross
//     at the centre of curvature, unmistakable on a circle. Measured as the
//     spurious curvature of (model - numerically integrated truth): 19.9% of
//     the local value at rate r, 0.5% here.
//
// lam is min(rho, sqrt(rho*r)), i.e. sqrt(rho * min(rho, r)). The inner
// branch's rate has to be rho for the clamp to join smoothly; outside the arc
// the linearisation above wants sqrt(rho*r), and the two meet at rho == r,
// where lam is r and the weight is 1 - so a fragment on the arc is bit-
// identical to the rate-r form this replaces, and the calibration the NORM
// factors carry is untouched. Against a numerically integrated perimeter the
// whole emitter's worst error drops as well, on every geometry tested: 16.5 ->
// 9.8% on a 600x400 r=40, 37.3 -> 26.3% on a circle.
//
// Cost is 1.03x of the neon pass at 1280x720, and - as the arc pedestal's note
// in neon.frag warns - that includes the cornerRadius 0 path, which never
// executes a line of this and still pays 1.026x for the register pressure.
// Re-time both after touching it. An inversesqrt formulation that trades the
// sqrt and the divide for two inversesqrts was measured and came out inside
// the noise, so the readable form stays.
//
// Returns (a, t1, t2, weight) for haloSegment / bloomSegment; the caller
// multiplies the segment by .w. The coverage bake reads the same four numbers
// to lay the arc's coverage along that line: arclength q from the +x end sits
// at t1 + q / weight.
//
// `u` is the direction of the arc point the arc is developed about, which
// arcTangentSegment below picks; the bake also calls this with each of the two
// fallback directions, behind the centre where the pick flips between them.
vec4 arcTangentSegmentAbout(vec2 w, float r, vec2 u) {
    // Arclength from the +x tangent point to the nearest arc point. u is a unit
    // vector in the first quadrant whenever it came from wq, so th is in
    // [0, HALF_PI] and needs no clamp of its own.
    float th  = minimaxAtan(u.y, u.x);
    float a   = abs(dot(w, u) - r);
    // Offset of the fragment ALONG the tangent, zero whenever u came from wq
    // (the foot of perpendicular is then the tangent point itself) and non-zero
    // only on the fallback, where it correctly pushes the whole arc to one side.
    // A LENGTH, not an arclength, so it is not scaled by the rate below.
    float off = dot(w, vec2(-u.y, u.x));
    // Floored so the exact centre of curvature cannot divide by zero. The
    // floor is far below one px, and the limit it lands on is the exact value
    // anyway - see above.
    float rho = max(length(w), ARC_FRAME_EPSILON);
    float lam = sqrt(rho * min(rho, r));
    return vec4(a, -lam * th - off, lam * (PIECES_HALF_PI - th) - off, r / lam);
}
// The direction arcTangentSegment develops about: the nearest arc point's.
vec2 arcTangentDirection(vec2 w) {
    vec2  wq = max(w, vec2(0.0));
    float ql = length(wq);
    return (ql > ARC_FRAME_EPSILON) ? wq / ql : ((w.x >= w.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
}
vec4 arcTangentSegment(vec2 w, float r) {
    return arcTangentSegmentAbout(w, r, arcTangentDirection(w));
}

// --- The glow coverage table's layout (V21) ----------------------------
// One table per PIECE of the emitter, because the coverage that scales a
// piece's halo and bloom is that piece's own: its coverage over its own extent,
// weighted by the layer's kernel at the fragment. V20's table was one per
// PERIMETER, indexed by the foot's perimeter position, and convolved along a
// straight line through the foot that ran on past the piece's ends - so near a
// corner where a lit stretch met a dark one, each piece borrowed its
// neighbour's coverage (V21 in docs/review-findings.md).
//
// `tableLayout.x` texels across - the table's width - and four bands of
// GLOW_COVER_ROWS rows down. Band b holds straight b at its left and corner b
// at its right:
//
//   straight   `tableLayout.y` overhang columns, the straight's own `inner`
//              columns, `tableLayout.y` more: across, the fragment's projection
//              along the straight -
//              uniform over the piece and rational past each end; down, its
//              distance a from the line, a / (a + kh), as V20's table had it. A
//              straight's clipped extent follows from the projection, so two
//              variables still describe it, and its table is symmetric about
//              the line.
//   corner     a guard texel, `tableLayout.z` overhang columns, the corner's own
//              `tableLayout.w` - inner columns, `tableLayout.z` more, a guard: across, the
//              fragment's direction from the arc's centre - uniform over the
//              arc's own quadrant, then on either side of it to the diagonal
//              behind the centre, where the two sides meet; down, its
//              distance from the centre, the arc's inside in the top half and
//              its outside in the bottom. Polar position determines everything
//              arcTangentSegment returns, inside, outside and behind the centre
//              alike, so two variables describe a corner too.
//
// THE LAYOUT IS A PARAMETER (`tableLayout`, the uniform uGlowCoverLayout): .x the
// columns - the texture's width, so the forward maps divide by the texture
// they read - .y a straight's overhang columns past each end, .z a corner's
// either side of its arc, and .w the columns a band's straight and corner
// share, .x - 2 - 2 .y - 2 .z. NeonRenderer sizes the table per config and
// stores the layout WITH it (GlowCoverLayout), so the bake and every read take
// the same four numbers; all are whole columns.
//
// `inner`, how many of a band's `tableLayout.w` columns its straight gets, is the
// uniform uGlowCoverSplit (.x the vertical straights' bands, .y the
// horizontal ones'), whole columns, set by NeonRenderer::GetGlowCoverSplit in
// proportion to the straight's and the arc's lengths. It comes from the CPU
// rather than from each shader's own arithmetic because the two shaders work
// in different units, and a split that rounded differently in one of them
// would read every texel of the band from the wrong place. So a circle's
// corners get the columns its zero-length straights do not need, and a long
// rect's straights get its small corners': at a fixed size, every piece gets
// a column density in proportion to its length.
//
// THE READ HAS TO BE CHEAP, AND THAT IS WHY EVERY MAP HERE IS A DIVISION.
// neon.frag computes these coordinates for up to eight pieces, and at scale 1.0
// the program also carries the gather loop, which makes it sensitive to the
// size of everything else in it - including code that a frame never runs.
// Measured on an AMD Radeon Pro 5300M, against the build before V21, on a fully
// lit ring that skips every read: with the corner direction an atan, the
// spacing past each end a log and the inside of the arc spaced by two more
// logs, the whole neon pass was 1.16x slower - 1.163x on a SHARP-cornered ring,
// which never enters the corner block at all - and stubbing the corner
// coordinates out took it to 1.04x. So the direction is a diamond angle (one
// division, see glowCoverDiamond), the spacing past the ends rational, and the
// inside a ratio of square roots, which took it to ~1.08x; the rest came from
// running neon.frag's corner block one arc at a time (addCornerPiece there).
// The bake inverts the maps, where cost does not matter. Do not put an atan or
// a log back in the forward maps without re-timing a fully lit ring at scale
// 1.0.
//
// WHICH PROGRAM COMPILES WHICH HALF. The forward maps (where a piece's
// coverage is READ) and their inverses (what each texel HOLDS) are compiled
// only into the programs that call them, since this compiler builds every
// function in a source whether main() reaches it or not: neon.frag takes the
// forward half, the bake (NEON_GLOW_COVER_BAKE) the inverse half, and the
// segment table's fill (NEON_GLOW_COVER_FILL), which decodes a texel of one
// table and reads its value from another, both.
#if !defined(NEON_GLOW_COVER_BAKE)
#define GLOW_COVER_FORWARD_MAPS
#endif
#if defined(NEON_GLOW_COVER_BAKE) || defined(NEON_GLOW_COVER_FILL)
#define GLOW_COVER_INVERSE_MAPS
#endif

// Which straight is which band, by the side of the rect in vPos it lies on.
// The vertical straights (x = -/+ halfSize.x) have length 2 * straight.y, the
// horizontal ones 2 * straight.x. Corner block b is in band b too.
#define GLOW_COVER_BAND_NEG_X  0
#define GLOW_COVER_BAND_POS_X  1
#define GLOW_COVER_BAND_NEG_Y  2
#define GLOW_COVER_BAND_POS_Y  3

// How far a corner's direction coordinate runs past each end of its arc: the
// arc is [0, 1] in it, and both sides reach the diagonal behind the centre at
// 1.5 past an end - 3/8 of a turn.
#define GLOW_COVER_CORNER_REACH 1.5

// Which corner block - and so which band - a corner's table is in: `signs` is
// the corner's quadrant in vPos, +-1 on each axis.
#ifdef GLOW_COVER_FORWARD_MAPS
int glowCoverCornerBlock(vec2 signs) {
    return (signs.x > 0.0 ? 1 : 0) + (signs.y > 0.0 ? 2 : 0);
}
#endif

// Band `band`'s straight interior columns, from uGlowCoverSplit.
float glowCoverInner(int band, vec2 split) {
    return (band < 2) ? split.x : split.y;
}

// A distance from the line as a row coordinate in [0, 1]: half the rows within
// one halo width, the rest out to the far field. V20's spacing.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverDistanceRow(float a, float kh) {
    return a / (a + kh);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverRowDistance(float v, float kh) {
    return kh * v / (1.0 - v);
}
#endif

// Past a straight's end, `e` the distance past it: half the overhang's columns
// within a quarter of them in halo widths, so a column is kh / 4 at the end
// and the spacing grows in proportion to e beyond. Rational rather than
// logarithmic for the reason above.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverStraightOver(float e, float kh, vec4 tableLayout) {
    return e / (e + 0.25 * tableLayout.y * kh);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverStraightPast(float over, float kh, vec4 tableLayout) {
    return 0.25 * tableLayout.y * kh * over / (1.0 - over);
}
#endif

// A direction as a "diamond angle": monotone in the true angle and continuous
// all the way round, without an atan. The arc's own quadrant is [0, 1] - 0 at
// its +x end, 1 at +y - and the rest of the turn runs on from either end to
// -1.5 / 2.5, which are both the diagonal behind the centre. `w` need not be
// normalised; at the centre itself any direction is as good as another.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverDiamond(vec2 w) {
    float l = abs(w.x) + abs(w.y);
    float p = (l > 0.0) ? w.x / l : 1.0;
    return (w.y >= 0.0) ? 1.0 - p : ((p < -0.5) ? 3.0 + p : p - 1.0);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
vec2 glowCoverDiamondDirection(float d) {
    float q = (d < 0.0) ? d + 4.0 : d;
    float p = (q <= 2.0) ? 1.0 - q : q - 3.0;
    return normalize(vec2(p, ((q <= 2.0) ? 1.0 : -1.0) * (1.0 - abs(p))));
}
#endif

// Past a corner's end, `d` the diamond angle past it: half the columns spaced
// evenly to the diagonal behind the centre, which is what a fragment far from
// the corner needs, and half concentrated at the end, a column about kh / 4 of
// arc there, which is what one beside the arc needs.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverCornerOver(float d, float r, float kh, vec4 tableLayout) {
    float k = tableLayout.z * kh / (8.0 * max(r, 1e-30));
    float R = GLOW_COVER_CORNER_REACH;
    return 0.5 * d / R + 0.5 * d * (R + k) / (R * (d + k));
}
#endif
// Its inverse: A d + B d / (d + k) = t is a quadratic in d, solved by whichever
// form of its positive root does not cancel.
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverCornerPast(float t, float r, float kh, vec4 tableLayout) {
    float k    = tableLayout.z * kh / (8.0 * max(r, 1e-30));
    float R    = GLOW_COVER_CORNER_REACH;
    float A    = 0.5 / R;
    float b    = A * k + 0.5 * (R + k) / R - t;
    float disc = sqrt(b * b + 4.0 * A * t * k);
    return (b >= 0.0) ? 2.0 * t * k / max(b + disc, 1e-30) : (disc - b) / (2.0 * A);
}
#endif

// Inside a corner's arc the coverage changes fast in two places - beside the
// arc, where the halo's width is kh, and near the centre, where the developed
// arc's width r / lam grows without bound - and slowly between. A ratio of
// square roots spaces the rows finely at both ends: `rho` the distance from the
// centre, 0 on the arc and 1 at the centre. Its inverse is closed form.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverInsideRow(float rho, float r) {
    float fromArc = sqrt(max(r - rho, 0.0));
    float fromCentre = sqrt(max(rho, 0.0));
    return fromArc / max(fromArc + fromCentre, 1e-30);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverInsideRadius(float v, float r) {
    return r * (1.0 - v) * (1.0 - v) / (v * v + (1.0 - v) * (1.0 - v));
}
#endif

// Texture coordinate of straight `band`'s coverage: `x` is the fragment's
// projection along it from its t1 end (haloSegment's), UNCLAMPED, `len` the
// straight's length, `a` the distance from its line, `kh` the halo width,
// `inner` glowCoverInner, `tableLayout` the table's. Clamped to the straight's own
// texels, since its corner's guard is the next one along. The inverse is
// glowCoverStraightAt.
#ifdef GLOW_COVER_FORWARD_MAPS
vec2 glowCoverStraightUV(int band, float x, float len, float a, float kh, float inner, vec4 tableLayout) {
    float over  = tableLayout.y * glowCoverStraightOver(max(-x, 0.0) + max(x - len, 0.0), kh, tableLayout);
    float along = (len > 0.0) ? clamp(x / len, 0.0, 1.0) : 0.5;
    float col   = tableLayout.y + inner * along + (x < 0.0 ? -over : over);
    col         = clamp(col, 0.5, 2.0 * tableLayout.y + inner - 0.5);
    float row   = clamp(glowCoverDistanceRow(a, kh) * float(GLOW_COVER_ROWS), 0.5, float(GLOW_COVER_ROWS) - 0.5);
    return vec2(col / tableLayout.x,
                (float(band * GLOW_COVER_ROWS) + row) / float(GLOW_COVER_HEIGHT));
}
#endif

// Texture coordinate of a corner's coverage: `w` is the fragment's offset from
// the arc centre in arcTangentSegment's frame, `block` from
// glowCoverCornerBlock, `inner` its band's straight's glowCoverInner,
// `tableLayout` the table's. The inverse is glowCoverCornerAt. The seam is the
// diagonal behind the centre, where the two overhangs meet; the guard column
// at each end of the block holds the other end's value, so a fetch is
// continuous across it.
#ifdef GLOW_COVER_FORWARD_MAPS
vec2 glowCoverCornerUV(int block, vec2 w, float r, float kh, float inner, vec4 tableLayout) {
    float arcCols = tableLayout.w - inner;
    float d       = glowCoverDiamond(w);
    float over    = tableLayout.z * glowCoverCornerOver(max(-d, 0.0) + max(d - 1.0, 0.0), r, kh, tableLayout);
    float col     = tableLayout.z + arcCols * clamp(d, 0.0, 1.0) + (d < 0.0 ? -over : over);
    float rho     = length(w);
    float side    = (rho >= r) ? glowCoverDistanceRow(rho - r, kh) : -glowCoverInsideRow(rho, r);
    float halfRows = 0.5 * float(GLOW_COVER_ROWS);
    float row     = clamp(halfRows * (1.0 + side), 0.5, float(GLOW_COVER_ROWS) - 0.5);
    return vec2((2.0 * tableLayout.y + inner + 1.0 + col) / tableLayout.x,
                (float(block * GLOW_COVER_ROWS) + row) / float(GLOW_COVER_HEIGHT));
}
#endif

// Inverse of glowCoverStraightUV for one texel: the projection `x` and the
// distance `a` its centre holds, from its column and row within the band.
#ifdef GLOW_COVER_INVERSE_MAPS
vec2 glowCoverStraightAt(float col, float row, float len, float kh, float inner, vec4 tableLayout) {
    float over = tableLayout.y;
    float x;
    if (col < over) {
        x = -glowCoverStraightPast((over - col) / over, kh, tableLayout);
    } else if (col > over + inner) {
        x = len + glowCoverStraightPast((col - over - inner) / over, kh, tableLayout);
    } else {
        x = (col - over) / max(inner, 1.0) * len;
    }
    return vec2(x, glowCoverRowDistance(row / float(GLOW_COVER_ROWS), kh));
}
#endif

// Inverse of glowCoverCornerUV for one texel: the offset `w` from the arc
// centre its centre holds, from its column within the corner's block (guards
// included, so 0 and the block's last are the guards) and its row.
#ifdef GLOW_COVER_INVERSE_MAPS
vec2 glowCoverCornerAt(float col, float row, float r, float kh, float inner, vec4 tableLayout) {
    float arcCols = tableLayout.w - inner;
    float over    = tableLayout.z;
    // Past the guard, the column is the same direction the other end's last
    // texel holds: the seam is one direction, approached from both sides.
    float cols    = arcCols + 2.0 * over;
    float c       = col - 1.0;
    c            -= cols * floor(c / cols);
    float d;
    if (c < over || c > over + arcCols) {
        float past = glowCoverCornerPast(((c < over) ? over - c : c - over - arcCols) / over, r, kh, tableLayout);
        d = (c < over) ? -past : 1.0 + past;
    } else {
        d = (c - over) / arcCols;
    }
    float halfRows = 0.5 * float(GLOW_COVER_ROWS);
    float rho = (row >= halfRows) ? r + glowCoverRowDistance((row - halfRows) / halfRows, kh)
                                  : glowCoverInsideRadius((halfRows - row) / halfRows, r);
    return rho * glowCoverDiamondDirection(d);
}
#endif

// Which texel of a table laid out as `tableLayout` and split as `split` the
// fragment at `fragCoord` is: its band, its row within the band (at the
// texel's centre), the band's straight interior columns, and the column its
// corner block starts at - left of it the band's straight, from it on its
// corner. The bake and the segment table's fill both decode a texel here, so
// the two cannot place one differently.
#ifdef GLOW_COVER_INVERSE_MAPS
void glowCoverTexel(vec2 fragCoord, vec2 split, vec4 tableLayout, out int band, out float row, out float inner,
                    out float splitColumn) {
    float y     = floor(fragCoord.y);
    band        = int(y) / GLOW_COVER_ROWS;
    row         = y - float(band * GLOW_COVER_ROWS) + 0.5;
    inner       = glowCoverInner(band, split);
    splitColumn = 2.0 * tableLayout.y + inner;
}
#endif

// ---------------------------------------------------------------------------
// The rounded box's signed distance, its antialiasing width and the cutoff
// band's two boundaries, shared by neon.frag, neon-blit.frag, neon-field.frag
// and black-rect.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it into the four
// programs that have to agree on the shape to the pixel - the glow, the
// scaled path's blit (which re-applies the glow's cut and cutoffs at full
// resolution), the ring field's composite (which re-applies the ring's) and
// the opaque fill (which has to register with the glow's edge) - so the four
// stay one copy. It reads no uniform and no constant.
// ---------------------------------------------------------------------------

precision highp float;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// One pixel's width in sdRoundBox's units, for the box filter across the
// outline: what fwidth(d) returns along a straight stretch of it, taken from
// the distance's analytic gradient instead of from d's own differences.
//
// fwidth(d) differences d across a 2x2 quad, and at a sharp corner that is not
// a derivative. On the vertex pixel the quad can hold BOTH outside neighbours:
// d then steps a whole pixel along each axis, fwidth reads 2, and every ramp
// sized from it doubles there - the corner pixel of a one-sided cut came out
// at 0.84 of the other three corners', the fill's at 0.75. The quad's parity
// decides which corner, so one rect showed it at its top-left and the next at
// no corner at all (V26). Along the inside diagonal it read 0 or 2 the same
// way. The gradient is one-sided by construction - a pixel takes the edge it
// is nearest - so it is 1 on every corner pixel, as along the edges, and
// fwidth's value wherever d is smooth.
//
// |gradient| per axis times that axis's pixel, fwidth(p): exact because every
// caller's p is the rect's own frame through an unrotated orthographic
// transform - vPos, or gl_FragCoord less the centre - so each screen axis
// moves one axis of p. q and length(max(q, 0)) are sdRoundBox's own, so a
// caller taking both pays for the length once. A derivative: take it in
// uniform control flow.
float sdRoundBoxFwidth(vec2 p, vec2 b, float r) {
    vec2  q  = abs(p) - b + r;
    vec2  qp = max(q, 0.0);
    float l  = length(qp);
    vec2  g  = (q.x > 0.0 && q.y > 0.0) ? qp / l
             : ((q.x > q.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    return dot(g, fwidth(p));
}

// --- Band boundary distances -------------------------------------------
// The band's two boundaries, expressed as signed distances: dIn >= 0 means
// "past the inside cutoff", dOut <= 0 means "within the outside cutoff".
//
// INNER: plain Euclidean (d + cut). Inside the shape the rounded-box SDF is
// already per-axis, so the inner boundary is square at cornerRadius 0 and a
// correct parallel curve (radius r - cut) above it. Nothing to fix.
//
// OUTER: Euclidean too whenever cornerRadius > 0, where it is exactly d - cut.
// That is the parallel curve, so the band keeps a uniform width and the opaque
// fill covers precisely as far as the light reaches - no black bulging past
// the glow at the corners.
//
// The one exception is cornerRadius == 0: the parallel curve of a SHARP corner
// is an arc of radius `cut`, so a rect the designer asked to be square comes
// out with rounded outer corners. There, and only there, offset the box
// per-axis instead to keep the corner square. The band is then ~1.41x wider
// measured diagonally across that corner, which is unavoidable - a uniform
// width and a square outer corner cannot both hold at a sharp corner.
//
// Disabled cutoffs arrive as a huge sentinel and still no-op: dIn goes hugely
// positive, dOut hugely negative, so both masks evaluate to 1.
float bandOuterDistance(vec2 p, float d, vec2 halfSize, float r, float cut) {
    if (r > 1e-4) { return d - cut; }
    vec2 b = halfSize + vec2(cut);
    return sdRoundBox(p, b, 0.0);
}
float bandInnerDistance(float d, float cut) {
    return d + cut;
}

// ---------------------------------------------------------------------------
// The neon's grade: the tone map from linear light to the output, shared by
// neon.frag and neon-field.frag, and the output dither, shared by those two and
// neon-blit.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it, after the tuning
// header (TONE_MAP_SHOULDER, GAMMA_EXPONENT, OUTPUT_DITHER_LSB), into the glow,
// the hue-invariant field's composite, which tone-maps col * Fa where neon.frag
// tone-maps its own result - the two have to agree to the bit for the field
// to stand in for pass 1 (I46) - and the blit, which only dithers.
// ---------------------------------------------------------------------------

precision highp float;

// Hue-preserving Reinhard: tonemap the peak channel and scale the others by
// the same ratio. Per-channel tonemap desaturates warm mixes (orange -> peach)
// because R saturates while G/B are still linear; scaling by the peak's
// compression preserves the original R:G:B ratio.
vec3 neonToneMap(vec3 result) {
    float peak = max(max(result.r, result.g), result.b);
    float mapped = peak / (peak + TONE_MAP_SHOULDER);
    result = result * (mapped / max(peak, 1e-6));
    return pow(result, vec3(GAMMA_EXPONENT));
}

// The output dither (R7): interleaved gradient noise in +/- OUTPUT_DITHER_LSB / 2
// levels added to a colour about to be written to the caller's 8-bit target,
// so the rounding there breaks the glow's slow gradients into noise instead of
// contour rings. A pure function of the pixel - no time term - so a frame is
// reproducible, and the blit and the edge ring, which tile one frame, draw one
// noise field across their seam. Colour only: the blend is premultiplied, and
// the same noise on alpha would cancel it over a bright background. Only for
// a write to the caller's framebuffer; an offscreen buffer is read back
// filtered, which averages the noise into blotches.
vec3 neonDither(vec3 c) {
    // A constant test, so OUTPUT_DITHER_LSB 0 compiles the noise out rather
    // than computing it and multiplying it by 0.
    if (OUTPUT_DITHER_LSB <= 0.0) {
        return c;
    }
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    return c + (n - 0.5) * (OUTPUT_DITHER_LSB / 255.0);
}

// precision, vPos, uRectSize / uCornerRadius, the segment block and PI are in
// neon-common.glsl, which is injected ahead of this file and neon-gather.frag;
// the perimeter gather itself (gatherPerimeter) is neon-gather.frag's.

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

// All other tuning constants (FILAMENT_*, HALO_*, BLOOM_*, grading, epsilons)
// are injected from lib/include/renderer/neon-tuning.h via @NEON_TUNING@ in
// shaders.h.in - single source of truth shared with the C++ renderer.
//
// (Far culling lives on the CPU: the draw quad is sized to rect + glowReach,
//  so there's no per-fragment discard here. See neon-renderer.cpp.)

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

// The SHADING: everything except the perimeter gather, which neon-gather.frag
// runs as a pass of its own at every scale and this file reads back with a
// bilinear fetch. Built into TWO program objects, one per target: pass 1b (at
// resolutionScale, into the reduced buffer) and the edge ring (at full
// resolution, onto the target), at every scale - 1.0 included, which takes
// the same path with a full-size reduced buffer. See
// NeonRenderer::ensurePathPrograms. The one variant NeonRenderer
// splices in after the version line (WithDefine) is NEON_FIELD_BAKE, the
// hue-invariant field's bake; see neon-field.frag.
out vec4 fragColor;

uniform float uLineWidth;
uniform float uFilamentFalloff; ///< Generalized-Gaussian exponent (N = value * 2); 1.0 = pure Gaussian, lower = smoother (Laplace-like), higher = flatter top.
uniform float uIntensity;
uniform float uTime;
uniform float uHueRotationRate;
uniform float uGlowRadius;
uniform float uBloomStrength;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;
uniform float uInsideCutoff;          ///< Cutoff::size of the inside cutoff: positive px distance INSIDE the rect edge where its fade STARTS. Disabled sides collapse to a huge sentinel CPU-side so this branch no-ops.
uniform float uInsideCutoffSoftness;  ///< Feather width in px, running on from uInsideCutoff toward the centre.
uniform float uOutsideCutoff;         ///< Cutoff::size of the outside cutoff: positive px distance OUTSIDE the rect edge where its fade STARTS. Disabled sides collapse to a huge sentinel CPU-side.
uniform float uOutsideCutoffSoftness; ///< Feather width in px, running on from uOutsideCutoff away from the rect.
uniform int   uWinding;               ///< 0 = CLOCKWISE, 1 = COUNTER_CLOCKWISE (matches Winding enum).
// Non-zero when nothing below reads the fragment's perimeter position: no
// segments, every lit arc over the whole ring with no stops of its own, and a
// gradient ring whose alpha is 255 at every texel. Then the filament's walk
// over the outline's pieces and the gradient's alpha read are skipped - the
// nearest-point map that walk replaced was ~9% of a default frame on its own
// - and the alpha is the 1.0 the read would have returned. Set by the CPU
// (IsPerimeterUnread), which tests
// the arcs' length a hair more strictly than arcCoverContinuous does, so it
// can never claim this where a read would happen.
uniform int   uPerimeterUnread;
// Non-zero on a ring lit uniformly - one arc over the whole ring, no segments
// - where every piece's own coverage IS the gathered one and the per-piece glow
// fix adds nothing (uniformCover in main()). Set by the CPU from
// IsGlowCoverUnread, the same test that skips baking the coverage table, so the
// two can never disagree about whether the table is current.
uniform int   uUniformCover;

// Ratio between the buffer this pass is drawing into and the viewport the
// caller sees: 1.0 when the gather runs at full resolution straight onto the
// caller's framebuffer, NeonConfig::resolutionScale when it runs into the
// scaled offscreen buffer that gets blitted back.
//
// Every pixel-valued uniform above ALREADY arrives pre-multiplied by it - the
// renderer scales them once on the CPU. This exists for the other direction:
// the px constants baked in from neon-tuning.h (FILAMENT_MIN_HALF_WIDTH,
// HEAD/TAIL_FEATHER_PX, GLOW_GATE_FADE_PX) are written in full-res px, so each
// use below converts it into the space everything else is in. Three sites, all
// marked; forgetting one makes a feather or a gate change width with the
// resolution scale, which reads as the scaled path "looking different" rather
// than as a bug.
uniform float uResolutionScale;

// 1 for a pass whose output the blit composites - pass 1b, and the field bake
// that stands in for it - which leaves the one-sided cut and the cutoffs to
// the blit; 0 for the edge ring and its field bake, which draw the frame's own
// pixels and apply both after the grade. Not the scale: at 1.0 pass 1b still
// goes through the blit.
uniform int uBlitOwnsCut;

// The segment block (uSegmentCount, uSegments) is in neon-common.glsl - the
// gather branches on it too.

// Per-segment gradient atlas (RGBA8, CLAMP-wrapped both axes). One row per
// segment, laid out head-to-tail across the segment's visible span. Sampled
// only when the segment's hasStops flag is set (see uSegments.w above).
uniform sampler2D uSegmentLUT;

// Arc gating - up to MAX_ARCS independent perimeter slices, each with its own
// start, length, intensity, and optional colour stops. Each vec4 is
// (start, length, intensity, hasStops). Overlap resolves winner-take-all:
// per sample, the arc with the largest effective mask (arcInside * intensity)
// contributes its colour and its mask to the emission. Because arcInside is
// smoothstepped 1-sample-wide at each end, adjacent arcs of different colours
// crossfade at the seam rather than snapping.
//
// When uArcCount == 0 the entire perimeter is dark; the default config seeds
// one full-perimeter arc so this only happens if the host wipes the vector.
layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// Per-arc gradient atlas (RGBA8, CLAMP-wrapped both axes). One row per arc,
// same layout convention as uSegmentLUT: a head-to-tail SPAN, baked with
// ColorUtils::SampleSpan, so the end colours hold rather than wrapping round.
//
// Read in TWO places, and the second is easy to miss. The pre-pass samples
// only the WINNING arc's row, for the gather hue. But the pointwise emitCover
// loop in main() samples the row of EVERY arc that covers this fragment, for
// its colour-stop alpha - so a row is live whenever its arc has stops, and the
// non-winning rows cannot be left stale. SpanAtlasLUT::Bake zero-fills the
// whole atlas on every bake (mAtlas.assign, not resize, for exactly this
// reason), which is what currently keeps that true.
uniform sampler2D uArcLUT;

// 1-row 2D LUT (REPEAT-wrapped) holding the precomputed colour ring.
// Replaces the in-shader sampleStops loop + HSV blend on the hot path.
// GLES 3.0 does not support sampler1D, so we use a 1-row 2D texture.
uniform sampler2D uGradientLUT;

// The glow coverage table (neon-glow-cover.frag, baked once per config change):
// for each piece of the emitter and each fragment position round it, how lit
// that piece is as the halo and the bloom see it, encoded c / (1 + c). Read by
// glowCoverAt, one linear fetch per piece; laid out in neon-pieces.glsl.
uniform sampler2D uGlowCover;

// How each band of that table shares its columns between its straight and its
// corner - the same numbers the bake was given. See glowCoverInner.
uniform vec2 uGlowCoverSplit;

// The table's layout - its width, its overhangs, the columns its bands share -
// as it was baked (neon-pieces.glsl, NeonRenderer::GlowCoverLayout).
uniform vec4 uGlowCoverLayout;

// The gather's own inputs - the sample block, uNumSamples and the emission
// table uEmission - are neon-gather.frag's alone: this file reads the gather's
// RESULT instead of running it, so it has no loop to feed.

// The gather - neon-gather.frag, its own pass at its own coarse scale
// (GetGatherScale) - stores four results this file reads back instead of
// running the loop: pass 1 at resolutionScale, the edge ring at full resolution
// (the filament and every hard edge, which a reduced buffer cannot carry).
// They are Lorentzian-weighted means whose kernel
// is never narrower than kc, so they survive a bilinear read from a grid a
// fraction of kc apart.
//
// uGather / uGatherSeg are the gather buffer's attachments 0 and 1 (uGatherSeg
// read only with segments). The buffer covers a region (GetBufferRegion),
// mapped from vPos by an affine uv map set for the space this pass's vPos is
// in.
uniform sampler2D uGather;
uniform sampler2D uGatherSeg;
uniform vec2      uGatherUVScale;
uniform vec2      uGatherUVOffset;

// Distance (in pixels, from the rect edge) to the draw quad's edge. The whole
// emission is faded to zero just before this, so the bloom never shows a hard
// rectangular cutoff where the quad clips it - independent of bloom strength.
uniform float uQuadMargin;

// Distance (in pixels, from a corner arc's circle, outward) past which that
// arc's halo and bloom are skipped: provably under a quarter of half an 8-bit
// level each. See addCornerPiece and NeonRenderer::GetCornerSkip.
uniform float uCornerSkip;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// PI, TWO_PI and HALF_PI are in neon-common.glsl. sdRoundBox and the band
// boundaries (bandOuterDistance, bandInnerDistance) are in neon-sdf.glsl,
// shared with neon-blit.frag and black-rect.frag; the tone map, neonToneMap,
// in neon-grade.glsl.

// --- Finite-segment halo / bloom ---------------------------------------
// The halo and bloom kernels integrated along a STRAIGHT SEGMENT: `a` the
// perpendicular distance to its line, t1 / t2 its endpoints along it from the
// fragment's foot. As t1 -> -inf, t2 -> +inf they reduce to the infinite-line
// forms HALO_NORM_FACTOR / BLOOM_NORM_FACTOR were calibrated against, so the
// sum over the pieces generalises the old nearest-edge term rather than
// retuning it - and removes its medial-axis creases.
float haloSegment(float a, float t1, float t2, float k) {
    float c2 = a * a + k * k;
    return k * k / c2 * (t2 / sqrt(c2 + t2 * t2) - t1 / sqrt(c2 + t1 * t1));
}
// ONE atan, not the difference of two: atan(c * (t2 - t1), c^2 + t1 * t2) is
// the angle the segment subtends, exactly (c > 0 and t2 >= t1 keep it in [0,
// PI]). The costliest term in the shader; the identity halves its atans
// (1.13-1.26x on the shading), and the atan is minimaxAtan (neon-pieces.glsl),
// not the driver's (I48).
float bloomSegment(float a, float t1, float t2, float k) {
    float c = sqrt(a * a + k * k);
    return k / c * minimaxAtan(c * (t2 - t1), c * c + t1 * t2);
}

// The bloom's 1/a tail is pedestal-subtracted to reach zero at the draw quad's
// edge. Each edge carries its OWN pedestal - the same segment evaluated at
// `reach` - so its contribution lands on zero at `reach` from it; one shared
// infinite-line pedestal over-subtracts a finite segment and cut the exterior
// tail short. The two edges of a pair span the same t1 / t2, so main() computes
// each pair's pedestal once.
//
// SKIPPED past `reach`, where it is exactly zero: bloomSegment falls
// monotonically with `a` for fixed t1 / t2, so the max() would clamp it to 0
// anyway. Byte-identical. The callers then fade it out before `reach` - the
// bloom end fade in main() (V23).
float bloomSegmentPedestalled(float a, float t1, float t2, float k, float reach, float pedestal, float fadeLo) {
    return (a < reach) ? max(bloomSegment(a, t1, t2, k) - pedestal, 0.0) * (1.0 - smoothstep(fadeLo, reach, a)) : 0.0;
}

// --- Corner arcs, developed onto their tangent -------------------------
// arcTangentSegment is in neon-pieces.glsl, injected ahead of this file: the
// glow coverage bake (neon-glow-cover.frag) lays each corner's coverage along
// the same line this file integrates the corner's halo and bloom along, so the
// two share one copy.

// --- The outline's pieces, for the filament -----------------------------
// The outline as the filament sees it: eight PIECES numbered in CW order -
// 0 top, 1 top-right, 2 right, 3 bottom-right, 4 bottom, 5 bottom-left, 6 left,
// 7 top-left (+y the top edge) - four straights between the tangent points and
// four quarter arcs, the same split the halo and bloom are summed over. A
// point on a piece is its progress u in [0, 1] along it in the CW direction.

// The perimeter position t in [0, 1] of the point a fraction u along piece
// `seg`, matching the CPU's GeometryUtils::GetPointOnRectangle for BOTH
// windings (uWinding 0 / 1): CCW runs the same pieces with mirrored progress
// (1 - u) and a CCW layout, so both windings stay exact.
float perimeterAt(int seg, float u) {
    float r      = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    float ws     = uRectSize.x - 2.0 * r;
    float hs     = uRectSize.y - 2.0 * r;
    float arcLen = PI * r * 0.5;
    float peri   = 2.0 * ws + 2.0 * hs + 4.0 * arcLen;

    float base;
    float len;
    if (uWinding == 0)
    {
        // Segment starts (cumulative) in CW order: top, TR, right, BR, bottom,
        // BL, left, TL.
        switch (seg)
        {
        case 0: base = 0.0;                                   len = ws;     break;
        case 1: base = ws;                                    len = arcLen; break;
        case 2: base = ws + arcLen;                           len = hs;     break;
        case 3: base = ws + arcLen + hs;                      len = arcLen; break;
        case 4: base = ws + 2.0 * arcLen + hs;                len = ws;     break;
        case 5: base = ws + 2.0 * arcLen + hs + ws;           len = arcLen; break;
        case 6: base = ws + 3.0 * arcLen + hs + ws;           len = hs;     break;
        default: base = peri - arcLen;                        len = arcLen; break;
        }
        return (base + len * u) / peri;
    }

    // Segment starts in CCW order: left, BL, bottom, BR, right, TR, top, TL.
    switch (seg)
    {
    case 0: base = 2.0 * hs + 3.0 * arcLen + ws;              len = ws;     break;
    case 1: base = 2.0 * hs + 2.0 * arcLen + ws;              len = arcLen; break;
    case 2: base = hs + 2.0 * arcLen + ws;                    len = hs;     break;
    case 3: base = hs + arcLen + ws;                          len = arcLen; break;
    case 4: base = hs + arcLen;                               len = ws;     break;
    case 5: base = hs;                                        len = arcLen; break;
    case 6: base = 0.0;                                       len = hs;     break;
    default: base = 2.0 * hs + 3.0 * arcLen + 2.0 * ws;       len = arcLen; break;
    }
    return (base + len * (1.0 - u)) / peri;
}

// The distance from p to piece `seg` and, in `u`, where on it the nearest
// point lies - each piece's OWN nearest point, an end where p lies past it.
// -1.0 for a piece the filament takes nothing from here: one `reach` or more
// away, where its core is exactly 0; a straight of zero length or a corner of
// radius 0, a point both its neighbours end on; or p outside a corner's
// quarter, where the arc's nearest point is the end it shares with a straight
// that already carries it (on that boundary the two agree, so leaving it out
// is continuous).
float filamentPieceDistance(int seg, vec2 p, float reach, out float u) {
    float halfW  = uRectSize.x * 0.5;
    float halfH  = uRectSize.y * 0.5;
    float r      = clamp(uCornerRadius, 0.0, min(halfW, halfH));
    float halfWs = halfW - r;
    float halfHs = halfH - r;
    float ws     = uRectSize.x - 2.0 * r;
    float hs     = uRectSize.y - 2.0 * r;
    u = 0.0;

    float dist;
    if (seg == 0 || seg == 4)
    {
        // Top edge left to right, bottom edge right to left.
        if (ws <= 0.0) return -1.0;
        float x = clamp(p.x, -halfWs, halfWs);
        dist    = length(p - vec2(x, (seg == 0) ? halfH : -halfH));
        u       = (seg == 0) ? (x + halfWs) / ws : (halfWs - x) / ws;
    }
    else if (seg == 2 || seg == 6)
    {
        // Right edge top to bottom, left edge bottom to top.
        if (hs <= 0.0) return -1.0;
        float y = clamp(p.y, -halfHs, halfHs);
        dist    = length(p - vec2((seg == 2) ? halfW : -halfW, y));
        u       = (seg == 2) ? (halfHs - y) / hs : (y + halfHs) / hs;
    }
    else
    {
        // Corner arc. The angle of the offset from its centre (radius r) gives
        // the fraction across the quarter, and is needed only within reach.
        if (r <= 0.0) return -1.0;
        vec2 sg = vec2((seg == 1 || seg == 3) ? 1.0 : -1.0, (seg == 1 || seg == 7) ? 1.0 : -1.0);
        vec2 w  = p - sg * vec2(halfWs, halfHs);
        if (w.x * sg.x <= 0.0 || w.y * sg.y <= 0.0) return -1.0;
        dist = abs(length(w) - r);
        if (dist >= reach) return -1.0;
        float th = atan(w.y, w.x);
        if (seg == 1)
        {
            u = (HALF_PI - th) / HALF_PI;                // top-right: theta 0..pi/2
        }
        else if (seg == 3)
        {
            u = -th / HALF_PI;                           // bottom-right: theta -pi/2..0
        }
        else if (seg == 5)
        {
            if (th > 0.0) th -= TWO_PI;                  // atan2 hands the left tangency back as +pi
            u = (-HALF_PI - th) / HALF_PI;               // bottom-left: theta -pi/2..-pi
        }
        else
        {
            u = (PI - th) / HALF_PI;                     // top-left: theta pi/2..pi
        }
    }
    return (dist < reach) ? dist : -1.0;
}


// uArcs[].w is packed CPU-side in PackArcFlags:
//   bit 0 (1)        - the arc has its own colour stops (read row `a` of uArcLUT)
//   2 * q, q 0..255  - how far another arc abuts its START (255 = covers the
//                      perimeter immediately before it, 0 = free)
//   512 * q          - the same for its END
// The abutment amounts pick each endpoint's feather direction in
// arcCoverContinuous (0 inward, 1 outward, between them the ramp slides, so
// the tip of an arc closing on a neighbour moves smoothly). They are a pure
// function of the arc set, so they are resolved once per frame on the CPU
// rather than rediscovered per fragment by an O(arcs^2) scan. Values are
// integers below 2^17, exact in a float.
bool arcHasStops(float flags)  { return mod(flags, 2.0) >= 0.5; }
float arcTailAbut(float flags) { return mod(floor(flags * 0.5), 256.0) / 255.0; }
float arcHeadAbut(float flags) { return floor(flags / 512.0) / 255.0; }

// Continuous [0,1] coverage of a fragment for the arc [start, start+length],
// which gates the sharp SDF filament. Feathered PER ENDPOINT, from the abut
// flags packed into uArcs[].w (packLightBlocks):
//
//   FREE endpoint     -> ramp INWARD: exactly 0 at the endpoint,
//                        so nothing outside the arc is lit.
//   ABUTTING endpoint -> ramp OUTWARD into the neighbour: 1 at the
//                        endpoint, so max() over two arcs hands over
//                        at full brightness with no notch.
//
// Both halves are needed: inward everywhere notches the seam between tiling
// arcs, and outward at a free endpoint lights a whole exterior quadrant at a
// sharp corner, where every fragment of the wedge shares one sPos. Feather
// widths arrive as perimeter fractions (converted at the call site).
float arcCoverContinuous(float sPos, float start, float length, float fHead, float fTail,
                         float tailAbut, float headAbut) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    float rel = sPos - start;
    rel -= floor(rel);                       // wrap to [0, 1): distance past start
    // An outward tail ramp reaches BEHIND the start, so those fragments need a
    // small NEGATIVE rel rather than one wrapped up to near 1. Split the gap
    // between the arc's head and its own start down the middle: past that
    // midpoint a fragment is approaching the start, not trailing the head.
    // Only for an outward tail - an inward one never reads rel < 0.
    if (tailAbut > 0.0 && rel > 0.5 * (1.0 + length)) { rel -= 1.0; }
    // Cap each feather at a share of the arc's own length. The widths arrive
    // as a fixed pixel span but `length` is a perimeter FRACTION, so on a small
    // rect a short arc can be narrower than the two ramps combined - they then
    // overlap and clip the peak, making the same arc config dimmer on a smaller
    // rect (0.21 vs 1.00 for L = 0.02 at 200x150 vs 800x600). Capping keeps the
    // peak at 1.0 for any length at any size, and leaves long arcs untouched.
    float cap    = length * ARC_FEATHER_MAX_SHARE;
    float fH     = min(fHead, cap);
    float fT     = min(fTail, cap);
    // Tail: inward ramps 0 -> 1 over [start, start + fT]; outward ramps over
    // [start - fT, start], reaching 1 AT the start.
    float tailIn = smoothstep(-tailAbut * fT, (1.0 - tailAbut) * fT, rel);
    // Head: inward falls 1 -> 0 over [end - fH, end]; outward holds 1 to the
    // end and falls over [end, end + fH].
    float headIn = 1.0 - smoothstep(length - (1.0 - headAbut) * fH, length + headAbut * fH, rel);
    return tailIn * headIn;
}

// The filament's generalized-Gaussian core at distance `a` from the line,
// pedestal-subtracted to reach exactly 0 at its reach - see main().
float filamentCore(float a, float sigma, float N, float corePed) {
    float c = exp2(-pow(a / sigma, N));
    return max(c - corePed, 0.0) / max(1.0 - corePed, 1e-6);
}

// The filament's two magnitudes at perimeter position sPos: .x the arcs'
// coverage x intensity x stop alpha (emitCover), .y the segments' boost x bell
// x stop alpha times their sharp gate (segCoverPt * filamentGate). main() takes
// them at each piece's own nearest point; the feathers arrive as perimeter
// fractions. Called from ONE place, so it is inlined once.
vec2 filamentCover(float sPos, float headF, float tailF) {
    // Colour-stop ALPHA rides here, on the magnitude and POINTWISE: folded
    // into `col` it would cancel, and gathered it would be a ring-wide mean
    // dragged toward the opaque far side. Alpha 0 kills the filament at that
    // position (the glow does not see stop alpha - V18), and the premultiplied
    // output alpha follows. Under uPerimeterUnread the ring is opaque at every
    // texel, so the read would return the 1.0 it starts at.
    float baseAlphaPt = 1.0;
    if (uPerimeterUnread == 0)
    {
        baseAlphaPt = textureLod(uGradientLUT, vec2(sPos - uTime * uHueRotationRate, 0.5), 0.0).a;
    }
    // The arcs' coverage at this point, per-arc intensity folded in: `col` is
    // gated-normalised, so intensity reaches the filament only through it.
    // Winner-take-all across arcs, as documented for overlap. This can be a
    // plain max() again because arcCoverContinuous now reaches a FULL 1.0 at an
    // abutting endpoint rather than 0 (inward) or 0.5 (straddling), so two arcs
    // tiling the ring hand over at max(w1, w2) with no notch - and because
    // their ramps overlap, the handover stays smooth even when w1 != w2.
    float emitCover = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;                       // dark arc: no filament
        float c = arcCoverContinuous(sPos, arc.x, arc.y, headF, tailF,
                                     arcTailAbut(arc.w), arcHeadAbut(arc.w));
        if (c <= 0.0) continue;                           // does not reach here
        // Each arc's own alpha, from the same LUT its colour came from and in
        // the same coordinate space the gather used - arc-local for hasStops,
        // perimeter space otherwise.
        float aA;
        if (arcHasStops(arc.w)) {
            // NO hue-rotation term here: uArc is the arc's OWN head-to-tail
            // coordinate, so there is nothing to rotate (it slid the gradient
            // off one end and seamed it mid-edge). An arc's gradient moves by
            // moving the arc or animating its stops, as a segment's does.
            //
            // WRAPPED, as arcCoverContinuous wraps its rel - an arc may
            // straddle the seam - with the same midpoint split, so a fragment
            // behind the start clamps to the head colour.
            float rowY = (float(a) + 0.5) / float(MAX_ARCS);
            float rel  = sPos - arc.x;
            rel       -= floor(rel);                       // wrap to [0, 1)
            if (rel > 0.5 * (1.0 + arc.y)) { rel -= 1.0; } // behind the start, not past the head
            float uArc = rel / max(arc.y, 1e-4);
            aA         = textureLod(uArcLUT, vec2(uArc, rowY), 0.0).a;
        } else {
            aA = baseAlphaPt;
        }
        emitCover = max(emitCover, c * arc.z * aA);
    }

    // Segment coverage at this point. This is the segments' whole magnitude
    // now: boost * bell, straight off the analytic gaussian, so it cannot
    // inherit either the gather's sample stepping or the far-side dilution that
    // used to make a segment dimmer on a small rect. Segments emit where no arc
    // covers, so they carry their own filament/halo/bloom.
    float segCoverPt = 0.0;
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg = uSegments[s];
        float rel = sPos - seg.x;
        rel      -= floor(rel + 0.5);                     // wrap to [-0.5, 0.5]
        float e   = rel * seg.y;
        // Per-segment alpha, pointwise - see emitCover above. Stop-less
        // segments inherit the base gradient's alpha, mirroring how their
        // colour falls back to segFallback in the gather.
        float sA;
        if (seg.w > 0.5) {
            float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
            float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
            sA           = textureLod(uSegmentLUT, vec2(tLocal, rowY), 0.0).a;
        } else {
            sA = baseAlphaPt;
        }
        segCoverPt += seg.z * exp(-e * e) * sA;
    }

    // The segments' SHARP gate (smoothstep 0.5..1), maxed with emitCover. Both
    // are exact at this perimeter position, so neither can quantise a slow
    // tracer's head to the gather points nor light the corner preceding an
    // arc's tail - the two bugs the old circular-mean/sample-based gates had.
    // Unclamped on purpose: boost above 1 must still brighten, as it did when
    // the gather's `bell` carried the magnitude. (segmentGlow's min(.., 1.0)
    // only bounds the shared halo/bloom reach - it is not the segment's
    // brightness.)
    float filamentGate = max(smoothstep(0.5, 1.0, min(segCoverPt, 1.0)), emitCover);
    return vec2(emitCover, segCoverPt * filamentGate);
}

// A piece whose own halo plus bloom at this fragment is below this, in linear
// light before the grade, keeps the gathered coverage rather than its own: the
// difference moves the output by well under one 8-bit level (the grade maps
// 1e-3 to about 1/255). Most pieces are this faint at most fragments - the far
// side of the rect, the bloom past `reach`.
const float GLOW_PIECE_MIN = 1e-4;

// The segment's glow magnitude from one piece's (arc, segment) coverage pair:
// its coverage times the soft gate the segment glow has always taken, max with
// the arc so a segment on a lit arc keeps the arc's glow reach.
float segmentGlow(vec2 cover) {
    return cover.y * max(cover.x, min(cover.y, 1.0));
}

// How lit one piece of the outline is as its own halo and bloom see it: the
// arcs' coverage x intensity (.r halo, .g bloom) and the segments' boost x bell
// (.b halo, .a bloom) over that piece's own extent, each weighted by the
// layer's kernel at the fragment - one filtered fetch of the table
// neon-glow-cover.frag bakes once per config change, at the texture coordinate
// glowCoverStraightUV / glowCoverCornerUV (neon-pieces.glsl) give for the
// piece.
//
// Without segments the table may have only two channels (RG16F, half the
// memory): its .b / .a would be zero anyway, but a two-channel texture samples
// .a as 1, which the decode turns into 1024. So they are zeroed here instead -
// on a uniform, and exactly what a four-channel table holds there.
vec4 glowCoverAt(vec2 uv) {
    vec4 e = textureLod(uGlowCover, uv, 0.0);
    e.ba = (uSegmentCount > 0) ? e.ba : vec2(0.0);
    return e / max(1.0 - e, vec4(1.0 / 1024.0));
}

// V19, V20 and V21: ONE PIECE's halo and bloom scaled by that piece's OWN
// coverage rather than the gathered mean. The exact term is INTEGRAL cover(s) *
// K(|p - P(s)|) ds over the piece; the table (glowCoverAt) holds its ratio to
// INTEGRAL K over the same piece, so times the piece's own halo or bloom it is
// exact - per piece, continuous in the fragment's position, so nothing switches
// at the medial axis.
//
// Applied as a CORRECTION to the gathered-mean scaling: each piece adds only
// the difference its own coverage makes, times its halo `h` and bloom `b`, to
// `fix` = (halo arc, halo segment, bloom arc, bloom segment). A skipped piece
// costs one compare. Keep what runs here small: earlier exact forms evaluated
// in this function cost 1.2-1.7x on EVERY scene through the program's register
// use.
//
// `haloWeight` / `bloomWeight` are the piece's halo and bloom as they reach the
// output, for the GLOW_PIECE_MIN test; the caller zeroes both on a ring lit
// uniformly, where the correction is exactly 0. `gatheredSeg` is
// segmentGlow(gathered).
void addPieceGlowFix(inout vec4 fix, vec2 uv, vec2 gathered, float gatheredSeg, float h, float b) {
    vec4 cover  = glowCoverAt(uv);
    vec2 coverH = cover.rb;
    vec2 coverB = cover.ga;
    fix += vec4((coverH.x - gathered.x) * h, (segmentGlow(coverH) - gatheredSeg) * h,
                (coverB.x - gathered.x) * b, (segmentGlow(coverB) - gatheredSeg) * b);
}

// A straight: `band` which one (GLOW_COVER_BAND_*), `x` the fragment's
// projection along it from its t1 end, unclamped, `len` its length, `a` its
// distance from the line.
void addStraightGlowFix(inout vec4 fix, int band, float x, float len, float a, float kh,
                        vec2 gathered, float gatheredSeg, float h, float b, float haloWeight, float bloomWeight) {
    if (haloWeight + bloomWeight <= GLOW_PIECE_MIN) {
        return;
    }
    addPieceGlowFix(fix, glowCoverStraightUV(band, x, len, a, kh, glowCoverInner(band, uGlowCoverSplit), uGlowCoverLayout),
                    gathered, gatheredSeg, h, b);
}

// A corner: `signs` its quadrant in vPos, `w` the fragment's offset from its
// centre in arcTangentSegment's frame.
void addCornerGlowFix(inout vec4 fix, vec2 signs, vec2 w, float kh, vec2 gathered, float gatheredSeg,
                      float h, float b, float haloWeight, float bloomWeight) {
    if (haloWeight + bloomWeight <= GLOW_PIECE_MIN) {
        return;
    }
    int block = glowCoverCornerBlock(signs);
    addPieceGlowFix(fix, glowCoverCornerUV(block, w, uCornerRadius, kh, glowCoverInner(block, uGlowCoverSplit),
                                           uGlowCoverLayout),
                    gathered, gatheredSeg, h, b);
}

// One corner arc, start to finish: developed onto its tangent
// (arcTangentSegment, whose .w is the measure the development rate cost), its
// halo and its bloom against the arcs' shared `pedestal`, added to `sum` (halo,
// bloom), and its own coverage correction.
//
// ONE ARC AT A TIME, AND THAT IS LOAD-BEARING. Developing all four arcs first
// kept sixteen floats live across the block and made every scene slower,
// sharp-cornered and fully lit ones included (measured history in
// docs/neon-frag-notes.md, from when this program also carried the gather
// loop). Keep the four calls whole.
//
// SKIPPED, whole, past uCornerSkip from the arc's circle: every developed point
// is at least length(w) - r away, where NeonRenderer::GetCornerSkip's bound
// puts the bloom at exactly 0 and the halo under a quarter of half an 8-bit
// level - four skipped arcs stay under half a level. The concave side, where
// length(w) < r, is never skipped. Change the halo or bloom terms and
// GlowBoundTerms has to follow.
//
// `bloomFade` is main()'s bloom end fade (V23) - its start on the concave side,
// on the convex side, and `reach` - taken against the developed distance c.x,
// the side picked as the straights pick theirs: by which side of the arc the
// fragment's development measures from.
void addCornerPiece(inout vec2 sum, inout vec4 fix, vec2 signs, vec2 w, float kh, float bw, float pedestal,
                    vec3 bloomFade, vec2 gathered, float gatheredSeg, float haloW, float bloomW) {
    if (length(w) - uCornerRadius >= uCornerSkip) {
        return;
    }
    vec2  u = arcTangentDirection(w);
    vec4  c = arcTangentSegmentAbout(w, uCornerRadius, u);
    float h = haloSegment(c.x, c.y, c.z, kh) * c.w;
    float fadeLo = (dot(w, u) >= uCornerRadius) ? bloomFade.y : bloomFade.x;
    float b = max(bloomSegment(c.x, c.y, c.z, bw) * c.w - pedestal, 0.0) *
              (1.0 - smoothstep(fadeLo, bloomFade.z, c.x));
    sum += vec2(h, b);
    addCornerGlowFix(fix, signs, w, kh, gathered, gatheredSeg, h, b, haloW * h, bloomW * b);
}



// ---------------------------------------------------------------------------

void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // The far exterior is culled on the CPU: the draw quad is sized to rect +
    // glowReach (NeonRenderer::setupGeometry), with a hole cut for
    // GlowSide::OUTSIDE, an inside cutoff, or the depth past which the output
    // is provably under half a level (GetGlowInnerReach). The geometry is
    // conservative by a few px; the discards below place the edge exactly. So
    // glowSide and insideCutoff are inputs to the QUAD, and in setupGeometry's
    // dirty gate.
    //
    // The one-sided cut's antialiasing width: one DESTINATION pixel in this
    // shader's units - sdRoundBoxFwidth is one buffer pixel for the SDF (what
    // fwidth(d) is along an edge, without its doubling on a sharp corner's
    // vertex pixel - V26), and uResolutionScale converts it to the pixel the
    // opaque fill (always full-res) registers with. Computed HERE, above every
    // discard, because a derivative downstream of non-uniform control flow is
    // undefined. This is the only derivative in this shader - keep it at the
    // top if a second one is ever needed.
    float sideAA = max(sdRoundBoxFwidth(vPos, halfSize, uCornerRadius) * uResolutionScale, 1e-6);

    // TOTAL feather width of the one-sided cut, floored at that pixel - the
    // floor is what antialiases the cut, and only because the ramp is applied
    // BELOW the tone map, as coverage.
    float sideSoft = max(uGlowSideSoftness, sideAA);

    // How far the ramp reaches back across the line: half a pixel, exactly as
    // far as the fill's own box filter reaches (black-rect.frag), so the two
    // share one ramp. The cut is ANCHORED at the line and feathers INTO the lit
    // side, never back past the fill.
    //
    // THE RING ONLY. A buffer texel lit at its centre is smeared over 1/scale
    // destination pixels both ways by the blit, so for pass 1b neon-blit.frag
    // applies the cut from ITS pixel width, and what is left here is the CULL,
    // BLIT_SIDE_GUARD_PX past the cut so the blit has lit texels to rebuild
    // the boundary from (neon-tuning.h).
    bool  blitOwnsCut = (uBlitOwnsCut != 0);
    float sideBack = 0.5 * sideAA;
    float sideCull = blitOwnsCut ? BLIT_SIDE_GUARD_PX : sideBack;
    // Deliberately NOT mirrored in neon-gather.frag - see the note at its top.
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  sideCull) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -sideCull) discard;

    // Hard geometric cutoffs. The band is [-uInsideCutoff, +uOutsideCutoff];
    // each side's feather STARTS at that boundary and runs its softness beyond
    // it, and anything past the feather is culled, so the glow cannot leak past
    // the stated reach whatever uGlowRadius says. Softness is per side and
    // decoupled from uGlowSideSoftness. Disabled sides arrive as a huge
    // sentinel, so these no-op.
    //
    // The floor is an ANTIALIASING floor, one DESTINATION pixel (sideAA - no
    // second derivative) on both paths: without it a cutoff is a binary edge, a
    // staircase wherever the boundary curves. Below 1.0 the masks are the
    // blit's; here the floor only places the discards, which have to agree with
    // the blit's ramp.
    float softFloor = sideAA;
    float inSoft  = max(uInsideCutoffSoftness,  softFloor);
    float outSoft = max(uOutsideCutoffSoftness, softFloor);

    // TOTAL widths, halved because each ramp is centred on the midpoint of its
    // fade (inMid / outMid): smoothstep(-w, w, x) spans 2w, and the unhalved
    // form feathered over twice the stated softness, off from an OpaqueMode
    // fill sharing the boundary.
    float inHalf  = 0.5 * inSoft;
    float outHalf = 0.5 * outSoft;

    // WHERE each fade sits: it starts at the cutoff and runs the requested
    // softness beyond it, so its MIDPOINT is softness/2 past the cutoff, and
    // the ramps are built symmetrically about it over the floored width. Below
    // the floor the ramp stays centred on the requested fade, so at softness 0
    // the edge's 50% point is the cutoff itself at every resolution scale.
    // Disabled and neutralised sides need no branch here.
    float inMid  = uInsideCutoff  + 0.5 * max(uInsideCutoffSoftness,  0.0);
    float outMid = uOutsideCutoff + 0.5 * max(uOutsideCutoffSoftness, 0.0);

    // Band boundaries measured against the offset rect, so a cornerRadius-0
    // band keeps square corners.
    //
    // A CUTOFF ON THE SIDE glowSide ALREADY CULLS IS NEUTRALISED - handed the
    // same huge distance a disabled one gets - so every use below no-ops
    // through the arithmetic. It never binds the silhouette there, but it was
    // not harmless: on the scaled path it ate the blit's guard band (a dark
    // seam), and at full res it squared the shared boundary.
    // NeonRenderer::setupGeometry mirrors this: the same redundant cutoff must
    // not shrink the quad's hole into the guard band either.
    float dOut;
    float dIn;
    if (uGlowSide == GLOW_SIDE_INSIDE)
    {
        dOut = -CUTOFF_NEUTRALISED;
    }
    else
    {
        dOut = bandOuterDistance(vPos, d, halfSize, uCornerRadius, outMid);
    }
    if (uGlowSide == GLOW_SIDE_OUTSIDE)
    {
        dIn = CUTOFF_NEUTRALISED;
    }
    else
    {
        dIn = bandInnerDistance(d, inMid);
    }
    // For pass 1b the blit owns the masks as well as the cut, and rebuilds
    // each boundary from the buffer texels around it - so the cull runs
    // BLIT_CUTOFF_GUARD_PX past the end of the ramp rather than at it, for the
    // reason sideCull does. The guard is an exact 0 for the ring, which keeps
    // culling exactly where its own masks end.
    float cutGuard = blitOwnsCut ? BLIT_CUTOFF_GUARD_PX : 0.0;
    if (dOut >  outHalf + cutGuard ) discard;
    if (dIn  < -(inHalf + cutGuard)) discard;

    // --- Filament -----------------------------------------------------
    // Generalized-Gaussian profile: core(ad) = exp(-ln(2) * (ad / sigma)^N),
    // sigma the half-brightness radius, N = 2 * uFilamentFalloff (0.5 Laplace,
    // 1.0 Gaussian - the default, 2.0 flatter top, 5.0 near-rectangular). Peak
    // at ad = 0 is exactly 1.0, with no power-law tail. lineGate fades the
    // filament in from lineWidth 0 ("no line") to FILAMENT_MIN_HALF_WIDTH * 2.
    // That constant is full-res px and uLineWidth arrives scaled, so it is
    // converted first - the identity at scale 1.0.
    float minHalf   = FILAMENT_MIN_HALF_WIDTH * uResolutionScale;
    float halfWidth = uLineWidth * 0.5;
    // Generalized-Gaussian exponent, needed before the sampling floor below
    // because that floor depends on the profile's SHAPE, not only its width.
    float N         = 2.0 * max(uFilamentFalloff, 1e-3);
    // Two floors, in different spaces on purpose. minHalf is the STATED-width
    // floor, converted like every full-res constant; it pairs with lineGate to
    // make lineWidth 0 mean no line. `nyquist` is the SAMPLING floor, in BUFFER
    // px and NOT converted: below 1.0 a filament the reduced buffer cannot
    // sample does not survive the blit.
    //
    // It is not a fixed half width: sigma must leave the profile at
    // FILAMENT_NYQUIST_MIN_SHARE of its peak FILAMENT_NYQUIST_SAMPLE_PX out -
    // what the bilinear filter reconstructs the peak from - and inverting
    // core() for sigma gives the expression. 0.5 buffer px at the default
    // falloff, under what the caller asked for at a soft one. Gated to the
    // scaled path: at 1.0 the filament stays exactly the width it was asked
    // for. NeonRenderer::setupGeometry mirrors both the expression and the gate
    // when it sizes the quad. See docs/corner-crease-and-filament-nyquist.md
    // section 2.8.
    float nyquist   = (uResolutionScale < 1.0)
                        ? FILAMENT_NYQUIST_SAMPLE_PX /
                          pow(log2(1.0 / FILAMENT_NYQUIST_MIN_SHARE), 1.0 / N)
                        : 0.0;
    float sigma     = max(max(halfWidth, max(minHalf, 1e-3)), nyquist);

    // Filament reach, in sigmas, for THIS falloff - see neon-tuning.h. Also
    // sizes the draw quad CPU-side; the two must stay in step.
    float reachSigmas = clamp(pow(log2(FILAMENT_GAIN / FILAMENT_CUTOFF), 1.0 / N),
                              FILAMENT_REACH_MIN_SIGMAS, FILAMENT_REACH_MAX_SIGMAS);
    // Pedestal-subtract the core so it reaches exactly zero at that reach,
    // renormalised to keep the ad = 0 peak at 1.0 (filamentCore). Where the
    // reach formula is honoured the pedestal is ~1.7e-4 and invisible; where
    // MAX_SIGMAS clamps it (soft falloff, whose tail would otherwise run for
    // hundreds of sigmas) this is what makes the glow end smoothly and,
    // crucially, SYMMETRICALLY. Without it the interior kept the full tail
    // while the exterior was cut at the quad edge.
    float corePed   = exp2(-pow(reachSigmas, N));
    // At the fragment's distance from the outline, i.e. from its NEAREST
    // piece: the filament where the whole ring reads one coverage, and the
    // test for whether any piece reaches this fragment at all.
    float core      = filamentCore(ad, sigma, N, corePed);
    float lineGate  = clamp(uLineWidth / max(minHalf * 2.0, 1e-3), 0.0, 1.0);

    // Perimeter of the rounded rect, in px - converts the arc feathers below
    // from px to perimeter fractions. The gather sizes its colour kernel from
    // the same function.
    float peri = rectPerimeter();

    // --- Kernel widths ------------------------------------------------
    // Two separate kernels, and the split is the point:
    //
    //  - kc is the COLOUR gather weight, sized inside gatherPerimeter
    //    (neon-gather.frag) as a fraction of the PERIMETER rather than in
    //    pixels, so the gradient reads identically at any size and cannot
    //    bead. Not coupled to glowRadius: the gather is colour only, so a wide
    //    glow has no business desaturating the ring.
    //
    //  - kh / bw are the EMISSION widths: raw glowRadius, no floor. The halo
    //    and bloom are evaluated analytically from the SDF distance further
    //    down, and a closed form cannot bead however far apart the gather
    //    samples are, so glowRadius is proportional across its entire range.
    float kh  = max(uGlowRadius,                       EMISSION_MIN_WIDTH);
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, EMISSION_MIN_WIDTH);

    // --- Colour gather -----------------------------------------------------
    // The gather's four results, read back from what neon-gather.frag stored:
    // the base and segment hues, each of unit magnitude, and the two g-weighted
    // mean coverages that scale the glow. COLOUR ONLY - the halo and bloom are
    // closed forms below.
    //
    // textureLod at level 0, not texture(): the discards above make control
    // flow non-uniform here, where an implicit derivative is undefined. The
    // coverages are stored as e = c / (1 + c) (neon-gather.frag) and decoded
    // after the bilinear read; the floor keeps a saturated texel finite. The
    // two sides change together.
    vec2  gatherUV          = vPos * uGatherUVScale + uGatherUVOffset;
    vec4  gather0           = textureLod(uGather, gatherUV, 0.0);
    vec3  col               = gather0.rgb;
    float emitCoverGathered = gather0.a / max(1.0 - gather0.a, 1.0 / 255.0);
    vec3  segColHue         = vec3(0.0);
    float segCoverGathered  = 0.0;
    if (uSegmentCount > 0) {
        vec4 gather1     = textureLod(uGatherSeg, gatherUV, 0.0);
        segColHue        = gather1.rgb;
        segCoverGathered = gather1.a / max(1.0 - gather1.a, 1.0 / 255.0);
    }
#ifdef NEON_FIELD_BAKE
    // The hue-invariant field (neon-field.frag). Everything below is linear in
    // the two hues - the output is col * Fa + segColHue * Fs before the tone
    // map - so with the arc hue on red alone and the segment hue on green
    // alone, .r computes Fa and .g computes Fs in one draw. .r is the same
    // expression as with col at 1 and segColHue 0, bit for bit; without
    // segments segColHue stays 0, so .g is 0 and the R16F field drops it.
    col = vec3(1.0, 0.0, 0.0);
    if (uSegmentCount > 0) {
        segColHue = vec3(0.0, 1.0, 0.0);
    }
#endif

    // --- Filament coverage, read at each piece's own nearest point -------
    // Everything in this block - the perimeter positions, the pointwise alpha
    // and the two coverages (filamentCover) - feeds ONE term, the filament
    // (`filament`, below), and `core` is exactly 0 past the filament's reach.
    // So off the line - most of the glow quad - none of it is computed: the
    // term is 0 * a finite value either way, so the output is the same bits.
    // Add a reader of any of them outside the filament and it has to move out
    // of this gate. The LUT reads are explicit-LOD for the same reason: they
    // now sit in non-uniform control flow, and every LUT has a single level.
    //
    // And never in a pass the blit composites (pass 1b and its field bake):
    // the edge ring re-shades every pixel within the filament's reach, as pass
    // 1 drew it, plus RING_GUARD_TEXELS (NeonRenderer's GetRingWidth), so the
    // blit's bilinear footprint never reaches a texel the filament lights -
    // the frame is the same bits without it (measured on the cover set,
    // check's scenes and the guide's figures). Shrink that guard and this has
    // to go.
    bool filamentLit = !blitOwnsCut && core > 0.0 && lineGate > 0.0;
    // Inward feathers: convert pixel widths to perimeter fractions at the
    // current geometry (`peri` is computed above the gather). `peri` is derived
    // from uRectSize and so is in SCALED px, while the two constants are
    // full-res - hence the conversion, which is identity at scale 1.0. Getting
    // it wrong changes the feather's width in perimeter fractions, i.e. the arc
    // ends soften over a different length at a different resolution scale.
    float headF  = HEAD_FEATHER_PX * uResolutionScale / peri;
    float tailF  = TAIL_FEATHER_PX * uResolutionScale / peri;
    // The filament as a MAX OVER THE OUTLINE'S PIECES (V25): each piece within
    // reach lights this fragment with its core at the fragment's distance from
    // IT, times the coverage at ITS nearest point - the same four straights and
    // four corners the halo and bloom are summed over. Not one read at the
    // fragment's nearest perimeter point: that map jumps across the medial
    // axis, so where the coverage differs on the two sides - an arc ending on a
    // sharp corner, an end ramp round any corner tighter than the filament's
    // reach - the filament was cut along the corner's diagonal, a hard 45-degree
    // edge on one side of the line only. Each piece's term is continuous in
    // the fragment's position, so their max is too. A max, not a sum, so a
    // ring lit uniformly is unchanged: core falls with distance, and the
    // nearest piece is the one at `ad`. And no bleed past a free end: every
    // piece meeting there reads that end's coverage, which is 0
    // (arcCoverContinuous).
    //
    // .x the arcs' magnitude and .y the segments', each with its core folded
    // in - two maxes, so the output stays linear in the two hues (the field's
    // factorisation, see NEON_FIELD_BAKE). Under uPerimeterUnread every
    // position reads one coverage, so the loop runs once, at the fragment's
    // own distance (filamentCore 1, `core` applied after - the expression as
    // before), and no position is computed. The step comes from the data, so
    // the loop is not unrolled into eight copies of filamentCover; a sharp
    // rect skips its four point-sized corners.
    vec2  filament  = vec2(0.0);
    bool  perPiece  = uPerimeterUnread == 0;
    float filReach  = sigma * reachSigmas;
    int   pieceStep = !perPiece ? 8 : ((uCornerRadius > 0.0) ? 1 : 2);
    int   pieceEnd  = filamentLit ? 8 : 0;
    for (int piece = 0; piece < pieceEnd; piece += pieceStep) {
        float sPos  = 0.0;
        float pCore = 1.0;
        if (perPiece) {
            float u;
            float dist = filamentPieceDistance(piece, vPos, filReach, u);
            if (dist < 0.0) continue;
            pCore = filamentCore(dist, sigma, N, corePed);
            if (pCore <= 0.0) continue;
            sPos = perimeterAt(piece, u);
        }
        filament = max(filament, pCore * filamentCover(sPos, headF, tailF));
    }
    float filamentCoreAt = perPiece ? 1.0 : core;

    // --- GLOW coverage: the same two magnitudes, GATHERED ------------------
    // The halo and bloom integrate over the WHOLE emitter, so what scales them
    // is the emitter's coverage averaged over that integral, not the nearest
    // perimeter point's: a nearest-point map JUMPS across the medial axis, and
    // a smooth field scaled by it creases there (V4 one level up). The
    // gather's ratio SUM(cover * g) / SUM(g) is that mean - smooth by
    // construction, and exactly 1.0 on a fully lit ring.
    //
    // Colour-stop alpha is NOT in this pair (V18): the glow does not see it. On
    // the line the gathered mean is too wide (V19), so the halo block corrects
    // it per piece toward each piece's own coverage - see addPieceGlowFix.
    vec2 gatheredCover = vec2(emitCoverGathered, segCoverGathered);

    // --- Analytic halo + bloom --------------------------------------------
    // Closed forms evaluated as a SUM OVER THE EMITTER'S PIECES - four
    // straights between the tangent points, and above cornerRadius 0 four
    // corner arcs developed onto their tangents - rather than one infinite-line
    // term at the nearest distance. Closed forms cannot bead, so glowRadius
    // sets the width directly at any size; the sum removes the medial-axis
    // creases and keeps both edges' light at a corner. The NORM factors keep
    // their calibration. What does move: a short edge's bloom is dimmer (it
    // emits less), and the interior settles on a floor rather than decaying
    // (capped by insideCutoff) - see
    // docs/corner-crease-and-filament-nyquist.md.
    //
    // Per edge: the perpendicular distance is the per-axis offset, and the
    // segment runs the opposite axis's extent, TRIMMED TO THE TANGENT POINTS.
    vec2  straight = max(halfSize - vec2(uCornerRadius), vec2(0.0));
    float aLeft  = abs(vPos.x + halfSize.x);
    float aRight = abs(vPos.x - halfSize.x);
    float aTop   = abs(vPos.y + halfSize.y);
    float aBot   = abs(vPos.y - halfSize.y);
    float tv1    = -straight.y - vPos.y;
    float tv2    =  straight.y - vPos.y;
    float th1    = -straight.x - vPos.x;
    float th2    =  straight.x - vPos.x;

    // Distance at which the emission has to be gone: the CPU's uncapped
    // quad-sizing formula (setupGeometry), recomputed here, so the pedestals
    // are size-invariant even where the outside cutoff clamps the quad. The
    // `sigma * reachSigmas` term is setupGeometry's filament-reach floor;
    // without it `reach` is 0 at glowRadius 0 and the pedestal subtracts the
    // whole bloom.
    float reach = max(uGlowRadius * GLOW_REACH_RADIUS_FACTOR *
                      (1.0 + uBloomStrength * uIntensity),
                      sigma * reachSigmas);

    // Each piece's own halo and bloom, and V19's correction to the coverage
    // that scales them (addPieceGlowFix). aTop measures to y = -halfSize.y and
    // aBot to +halfSize.y.

    // One arc over the whole ring and no segments: every piece's own coverage
    // equals the gathered mean, so there is nothing to correct. It zeroes the
    // weights below, so every piece takes addStraightGlowFix's /
    // addCornerGlowFix's first return.
    //
    // Do NOT turn this into an `if (!uniformCover)` around the corrections, or
    // around a second copy of the sums. Measured on an M2 Pro, either one makes
    // EVERY scene 10-15% slower at scale 1.0 - fully lit ones included, which
    // never enter it - presumably because this program carries the gather loop
    // there. Below 1.0 the same branch saves ~3%, which is not worth it.
    // (Measured while the loop ran inline at 1.0; a compile-time variant was
    // measured since, at 1.05-1.10x on intensity frames only -
    // docs/neon-shader-cleanup-plan.md.)
    //
    // Decided ONCE, on the CPU (uUniformCover): the very test NeonRenderer
    // skips baking the coverage table on, so wherever the table may be stale
    // this holds and nothing reads it.
    bool uniformCover = uUniformCover != 0;

    // Each piece's linear weight in the output, for the GLOW_PIECE_MIN tests.
    // bloomGain is the renormalisation applied further down.
    float bloomPeak = BLOOM_NORM_FACTOR * PI;
    float bloomPed  = BLOOM_NORM_FACTOR * PI * bw / sqrt(reach * reach + bw * bw);
    float bloomGain = bloomPeak / max(bloomPeak - bloomPed, 1e-6);
    float haloW     = uniformCover ? 0.0 : HALO_NORM_FACTOR * HALO_GAIN;
    float bloomW    = uniformCover ? 0.0 : BLOOM_NORM_FACTOR * bloomGain * uBloomStrength;

    float hLeft  = haloSegment(aLeft,  tv1, tv2, kh);
    float hRight = haloSegment(aRight, tv1, tv2, kh);
    float hTop   = haloSegment(aTop,   th1, th2, kh);
    float hBot   = haloSegment(aBot,   th1, th2, kh);
    float pedV   = bloomSegment(reach, tv1, tv2, bw);
    float pedH   = bloomSegment(reach, th1, th2, bw);

    // The bloom's END FADE (V23). The pedestal lands each piece's bloom on 0 at
    // `reach` with its slope still on: a crease, and the tone map's 0.85 power
    // steepens it. Inside, nothing hid it, and a large rect's interior showed a
    // hard-edged dark rectangle `reach` in. Outside, the quad-edge fade below
    // did, but starting at 0.8 of the margin it pulls the tail down faster
    // before it flattens it, which reads as an edge too. So each piece's bloom
    // is faded by its OWN distance, on both sides, from BLOOM_FADE_START_FRAC of
    // `reach` - about the latest start that does not steepen the tail
    // (neon-tuning.h). Its start is floored at the cutoff's end on the side the
    // fragment sits of that piece, as fadeStart is below, so a cutoff band is
    // not dimmed ahead of its mask. A factor of the fragment's position alone,
    // at most 1: the coverage table's ratios and the field's factorisation
    // hold, and every CPU bound on the bloom only gets looser.
    float bloomFadeFloor = reach * BLOOM_FADE_START_FRAC;
    float inCutEdge      = inMid + inHalf + cutGuard;
    float outCutEdge     = outMid + outHalf + cutGuard;
    vec3  bloomFade = vec3((inCutEdge  < reach) ? max(bloomFadeFloor, inCutEdge)  : bloomFadeFloor,
                           (outCutEdge < reach) ? max(bloomFadeFloor, outCutEdge) : bloomFadeFloor,
                           reach);
    // Each straight's fade starts from the cutoff on its side of that line.
    float bLeft  = bloomSegmentPedestalled(aLeft,  tv1, tv2, bw, reach, pedV,
                                           (vPos.x > -halfSize.x) ? bloomFade.x : bloomFade.y);
    float bRight = bloomSegmentPedestalled(aRight, tv1, tv2, bw, reach, pedV,
                                           (vPos.x <  halfSize.x) ? bloomFade.x : bloomFade.y);
    float bTop   = bloomSegmentPedestalled(aTop,   th1, th2, bw, reach, pedH,
                                           (vPos.y > -halfSize.y) ? bloomFade.x : bloomFade.y);
    float bBot   = bloomSegmentPedestalled(aBot,   th1, th2, bw, reach, pedH,
                                           (vPos.y <  halfSize.y) ? bloomFade.x : bloomFade.y);

    // The plain sums, as before V19, and V19's per-piece correction to them.
    float halo  = hLeft + hRight + hTop + hBot;
    float bloom = bLeft + bRight + bTop + bBot;
    float gatheredSeg = segmentGlow(gatheredCover);
    vec4  glowFix = vec4(0.0);
    // Each straight's table is read at the fragment's projection along it
    // from its t1 end - unclamped, since past an end its coverage still
    // changes - and its distance from the line.
    float ws       = 2.0 * straight.x;
    float hs       = 2.0 * straight.y;
    float alongX   = vPos.x + straight.x;
    float alongY   = vPos.y + straight.y;
    addStraightGlowFix(glowFix, GLOW_COVER_BAND_NEG_X, alongY, hs, aLeft, kh, gatheredCover,
                       gatheredSeg, hLeft, bLeft, haloW * hLeft, bloomW * bLeft);
    addStraightGlowFix(glowFix, GLOW_COVER_BAND_POS_X, alongY, hs, aRight, kh, gatheredCover,
                       gatheredSeg, hRight, bRight, haloW * hRight, bloomW * bRight);
    addStraightGlowFix(glowFix, GLOW_COVER_BAND_NEG_Y, alongX, ws, aTop, kh, gatheredCover,
                       gatheredSeg, hTop, bTop, haloW * hTop, bloomW * bTop);
    addStraightGlowFix(glowFix, GLOW_COVER_BAND_POS_Y, alongX, ws, aBot, kh, gatheredCover,
                       gatheredSeg, hBot, bBot, haloW * hBot, bloomW * bBot);

    // The four corner arcs, each developed onto its own tangent - see
    // arcTangentSegment. Gated because at cornerRadius 0 there is nothing to
    // model: `straight` is then halfSize, the four segments above already run
    // corner to corner, and each arc term would contribute a zero-length
    // segment. uCornerRadius is a UNIFORM, so this branches uniformly and the
    // sharp-cornered case pays nothing for the four atans behind it.
    //
    // A fragment's offset from each arc centre, in that corner's own outward
    // frame, is (+/-|vPos.x| - straight.x, +/-|vPos.y| - straight.y): the two
    // axes are the incident edges' outward normals, so folding through abs()
    // puts every corner in the same first-quadrant form arcTangentSegment
    // expects, with the sign pair selecting which corner.
    if (uCornerRadius > 0.0)
    {
        vec2 wNear = abs(vPos) - straight;
        vec2 wFar  = -abs(vPos) - straight;

        // Which corner each arc is, for its coverage table: the folded frames
        // above unfolded by the corner's sign pair. N is the fragment's own
        // side of an axis, F the opposite one.
        vec2 sN    = vec2(vPos.x >= 0.0 ? 1.0 : -1.0, vPos.y >= 0.0 ? 1.0 : -1.0);

        // One shared pedestal for all four arcs. An arc's developed extent is
        // the same lam*HALF_PI wherever the fragment sits at a given distance,
        // so evaluating it CENTRED leaves an expression in uniforms alone -
        // exact for a fragment facing an arc from `reach`, an over-subtraction
        // only off to its side, where the clamp takes it to 0. lam is pinned to
        // the value at `reach` from the arc, sqrt((reach + r) * r). The
        // straights cannot share one: their half-length routinely exceeds
        // `reach`. Worst case, a circle: the tail ends sooner, all within
        // 4/255.
        //
        // Do NOT collapse this further to bw*L/c^2 (atan(x) -> x). That is a
        // ~7% over-subtraction at the largest arc in range, and with the clamp
        // sitting right underneath it, 7% of the pedestal took the same circle
        // to 0.576. (The lam below makes the atan's argument LARGER than the
        // arclength form did, so the linearisation is worse here, not better.)
        float arcC        = sqrt(reach * reach + bw * bw);
        float arcLamPed   = sqrt((reach + uCornerRadius) * uCornerRadius);
        float arcPedestal = uCornerRadius / arcLamPed *
                            bw / arcC * 2.0 * atan(arcLamPed * HALF_PI / (2.0 * arcC));

        // Each arc in turn - developed, its halo and bloom, its coverage read -
        // see addCornerPiece. The two sums run in the order the four-term
        // expressions they replaced did, so a fully lit ring is unchanged bit
        // for bit.
        vec2 cornerGlow = vec2(0.0);
        addCornerPiece(cornerGlow, glowFix, vec2( sN.x,  sN.y), vec2(wNear.x, wNear.y), kh, bw, arcPedestal,
                       bloomFade, gatheredCover, gatheredSeg, haloW, bloomW);
        addCornerPiece(cornerGlow, glowFix, vec2( sN.x, -sN.y), vec2(wNear.x, wFar.y),  kh, bw, arcPedestal,
                       bloomFade, gatheredCover, gatheredSeg, haloW, bloomW);
        addCornerPiece(cornerGlow, glowFix, vec2(-sN.x,  sN.y), vec2(wFar.x,  wNear.y), kh, bw, arcPedestal,
                       bloomFade, gatheredCover, gatheredSeg, haloW, bloomW);
        addCornerPiece(cornerGlow, glowFix, vec2(-sN.x, -sN.y), vec2(wFar.x,  wFar.y),  kh, bw, arcPedestal,
                       bloomFade, gatheredCover, gatheredSeg, haloW, bloomW);
        halo  += cornerGlow.x;
        bloom += cornerGlow.y;
    }

    halo    *= HALO_NORM_FACTOR;
    bloom   *= BLOOM_NORM_FACTOR;
    glowFix *= vec4(HALO_NORM_FACTOR, HALO_NORM_FACTOR, BLOOM_NORM_FACTOR, BLOOM_NORM_FACTOR);

    // The RENORMALISATION: the pedestalled sum scaled back up by peak/(peak -
    // pedestal), so the value on the line is unchanged and BLOOM_NORM_FACTOR
    // keeps its calibration. The gain uses the infinite-line pedestal - within
    // a few percent of the near edge's own on the line. The halo needs no
    // pedestal: at `reach` it is ~2e-4 of peak.
    bloom      = bloom * bloomGain;
    glowFix.zw = glowFix.zw * bloomGain;

    // glowRadius == 0 must read as "filament only", but an analytic profile at
    // radius 0 is a sub-pixel spike of FULL height rather than nothing, so
    // both layers fade in over glowRadius = [0, GLOW_GATE_FADE_PX]. Measured
    // against a fixed pixel width: gating against the sampleSpacing-derived
    // floor instead would re-couple brightness to the rect size. (The bloom is
    // gated too now - the gather's spacing floor used to keep it finite here.)
    // Full-res constant against a scaled uGlowRadius, so it is converted like
    // the feathers above - identity at scale 1.0.
    float glowGate = clamp(uGlowRadius / (GLOW_GATE_FADE_PX * uResolutionScale), 0.0, 1.0);

    // Compose: base arc x intensity + segments (independent of intensity, so a
    // segment stays lit on a dark arc). EACH SOURCE CARRIES ITS OWN COVERAGE:
    // `col` is a unit hue everywhere, so it is multiplied by the arcs'
    // coverage here; one shared gate let a segment's gate lift the arc term
    // where no arc covers (a red segment on a blue half-ring read magenta).
    vec3 arcCol = col * uIntensity;

    // The segments' filament carries their SHARP gate (filamentCover);
    // segmentGlow's is the soft one. The arc takes its own coverage directly
    // in both, since for an arc the two gates were just that coverage anyway.
    //
    // THE TWO TAKE DIFFERENT COVERAGES, and that is the point. The filament is
    // an SDF-derived line: it lives ON the perimeter, so it wants the
    // POINTWISE pair, exact at each piece's own nearest point. The halo and
    // bloom are integrals over the whole emitter, so they want the GATHERED
    // pair - plus glowFix, V19's per-piece correction toward the coverage at
    // each piece's foot, in the same arc / segment halves.
    vec3 emitFil  = arcCol * filament.x          + segColHue  * filament.y;
    vec3 emitGlow = arcCol * gatheredCover.x     + segColHue  * gatheredSeg;

    vec3 result  = emitFil  * filamentCoreAt * FILAMENT_GAIN  * lineGate;
    result      += (emitGlow * halo  + arcCol * glowFix.x + segColHue * glowFix.y) * HALO_GAIN      * glowGate;
    result      += (emitGlow * bloom + arcCol * glowFix.z + segColHue * glowFix.w) * uBloomStrength * glowGate;

    // NOTE neither the one-sided cut NOR the hard cutoff masks are applied
    // here. Both are COVERAGE, not emission, so both belong below the grade -
    // see the block after the tone map. The quad-edge fade below stays, and is
    // the one mask that genuinely shapes emission: it hides a clip rather than
    // drawing an edge, and it is tens of pixels wide.

    // --- Quad-edge fade: the draw quad ends uQuadMargin past the rect ON EACH
    // AXIS. The emission fades to zero over the last stretch so a strong bloom
    // never shows a hard edge where the quad clips it. PER-AXIS (dQuad), not
    // from d: what this hides is a rectangle, and a d-keyed ramp ate the
    // corners early.
    //
    // fadeStart is floored at the outside cutoff's end when that falls inside
    // the quad, so the ramp never dims the band's outer edge before the cutoff
    // mask does. Otherwise the unfloored start, so fadeStart < uQuadMargin
    // always holds and the ramp width is strictly positive (an inverted
    // smoothstep is undefined in GLSL).
    float fadeFloor = uQuadMargin * QUAD_FADE_START_FRAC;
    // Where the band's emission ends: outMid + outHalf, plus cutGuard on the
    // scaled path. NeonRenderer::setupGeometry caps uQuadMargin at the same end
    // (GetCutoffEnd, plus the same guard) plus 1 full-res px, so while that cap
    // holds cutEdge < uQuadMargin and the fade starts at the end.
    float cutEdge   = outMid + outHalf + cutGuard;
    float fadeStart = (cutEdge < uQuadMargin) ? max(fadeFloor, cutEdge) : fadeFloor;
    float dQuad     = sdRoundBox(vPos, halfSize + vec2(uQuadMargin), 0.0);
    result *= 1.0 - smoothstep(-(uQuadMargin - fadeStart), 0.0, dQuad);

    // --- Grade --------------------------------------------------------
    // neonToneMap (neon-grade.glsl, shared with neon-field.frag).
#ifdef NEON_FIELD_BAKE
    // Fa and Fs: the pre-tone-map result per unit of each hue, which
    // neon-field.frag tone-maps times the gathered hues. The masks below are
    // not in it: for the shading's field the blit owns both; the edge ring's
    // field, baked with the ring's uniforms, has its composite apply them
    // (neon-field.frag, NEON_FIELD_RING). uGlowSideSoftness is read only
    // below this return, so this program may compile it out, and
    // NeonRenderer leaves it out of the bake's upload (UploadEdgeMaskUniforms):
    // a read of it above here has to go back into that upload.
    fragColor = vec4(result.r, result.g, 0.0, 1.0);
    return;
#endif
    result = neonToneMap(result);

    // --- One-sided cut: mask the WHOLE layer at the line --------------
    // Anchored at the opaque fill's own edge and feathered INTO the lit side
    // (sideAA / sideSoft / sideBack at the top of main()).
    //
    // BELOW THE GRADE: the cut is COVERAGE, so it scales what reaches the
    // framebuffer. Above the tone map it was nearly annihilated - a
    // half-covered filament pixel came out at 94% - and the alpha (the peak
    // channel) occluded at 0.94 instead of 0.5. Below it, the cut is the 1 px
    // box filter and tracks the fill it has to register with.
    //
    // Written as `1.0 - smoothstep(lo, hi, d)` for the INSIDE arm rather than
    // smoothstep(hi, lo, d): edge0 > edge1 is UNDEFINED in GLSL. Same curve,
    // portably.
    //
    // THE RING ONLY: for pass 1b neon-blit.frag applies this exact expression
    // against ITS own pixel width, and the ring field's composite (neon-field.frag)
    // applies it for the ring's field. Keep the three in step - one edge.
    if (!blitOwnsCut)
    {
        if (uGlowSide == GLOW_SIDE_INSIDE)       result *= 1.0 - smoothstep(sideBack - sideSoft, sideBack, d);
        else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-sideBack, sideSoft - sideBack, d);
    }

    // --- Hard cutoff masks: close the band at its two boundaries ------
    // In the band means outside the shrunk rect (dIn >= 0) and inside the grown
    // one (dOut <= 0); each ramp spans its full softness, centred on inMid /
    // outMid. Disabled sides arrive as a huge sentinel, so the smoothstep
    // passes 1.0 through. BELOW THE GRADE, as coverage, like the cut: the floor
    // removes the staircase, the placement the brightness bias at half
    // coverage.
    //
    // THE RING ONLY: for pass 1b neon-blit.frag applies these same two ramps at
    // destination resolution, from BLIT_CUTOFF_GUARD_PX of lit texels the
    // discards leave it, and the ring field's composite applies them for the
    // ring's field. Keep the three in step - one edge.
    if (!blitOwnsCut)
    {
        result *= smoothstep(-inHalf, inHalf, dIn);
        result *= 1.0 - smoothstep(-outHalf, outHalf, dOut);
    }

    // Premultiplied-alpha output so the effect composites over arbitrary
    // background objects instead of only adding light. Coverage = brightest
    // channel: the hot filament core (alpha ~ 1) occludes the background and
    // reads as a solid tube; the dim halo/bloom (alpha ~ 0) stay additive; the
    // dark surround (alpha = 0) leaves the background untouched. Pairs with
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) in the renderer.
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    // The output dither, on the ring's write to the caller's framebuffer only:
    // pass 1b writes the reduced buffer, which the blit dithers as it writes.
    fragColor = vec4(blitOwnsCut ? result : neonDither(result), alpha);
}

)shadersrc";

                static const char *const NEON_GATHER_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - the neon shaders (neon.frag, neon-emission.frag), where CMake
//     text-injects this file via @NEON_TUNING@ in shaders.h.in, and
//   - the C++ renderer (neon-renderer.cpp), which #includes it for the
//     glow-reach quad-sizing factors.
//
// The px constants below are written in FULL-RESOLUTION pixels. Where the
// renderer draws at a reduced NeonConfig::resolutionScale, the shader converts
// each one with uResolutionScale at the point of use; the notes on the
// individual constants say which need it and which do not.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a `const float X = 0.9f;` cannot
// be written once and stay valid in both languages. Plain #define is the only
// form that compiles identically as GLSL and as C++ from a single file.
// (For C++-only constants, prefer constexpr - macros are used here strictly
// because these values must also live inside the shaders.)
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
//
//     FILAMENT_MIN_HALF_WIDTH is a STATED-width floor: it is what makes
//     lineWidth 0 mean "no line" rather than a single bright dot, and it is
//     paired with lineGate, which fades the filament in over the same span. It
//     describes the width the caller asked for, so it is a FULL-RES px constant
//     and the shader converts it with uResolutionScale like every other one.
//
//     The SAMPLING floor is the pair below, and the distinction from the
//     stated floor is the whole point of having two of them. Below
//     resolutionScale 1.0 the gather rasterises into a reduced buffer and
//     neon-blit.frag bilinearly upsamples it, so a filament the buffer cannot
//     sample is not reconstructed - it is resampled, and what comes out
//     depends on where the rect edge happens to fall between buffer texel
//     centres. That is a property of the BUFFER, so these are in BUFFER px and
//     are NOT converted with uResolutionScale.
//
//     Getting that backwards is what made a 1 px line at scale 0.5 disagree
//     with the same line at 1.0. sigma was floored only by the converted
//     stated constant, which at scale 0.5 is 0.25 buffer px, so the line sat
//     under the buffer's Nyquist limit and its peak swung with the rect's
//     sub-pixel position - 217 on a texel centre, 180 half a texel off,
//     against a 1.0 reference that holds 230-241 at every position.
//
//     THE FLOOR IS NOT A FIXED HALF WIDTH. It was one - a flat 0.5 buffer px -
//     and a fixed half width asks the wrong question. What survives the blit
//     is not decided by the profile's width at half maximum; it is decided by
//     how much signal the NEIGHBOURING buffer texel still carries, because
//     that is what the bilinear filter rebuilds the peak from. So the floor
//     says exactly that: the profile must still be at
//     FILAMENT_NYQUIST_MIN_SHARE of its peak, FILAMENT_NYQUIST_SAMPLE_PX out.
//     Inverting the generalized Gaussian for sigma gives
//
//         sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
//
//     which neon.frag and NeonRenderer::setupGeometry both evaluate.
//
//     0.0625 and 1.0 are today's behaviour restated, not a retune: at the
//     DEFAULT falloff (N = 2) the expression is exactly 0.5 buffer px, the
//     constant it replaces, so nothing at or near the default moves.
//
//     The two questions diverge at a SOFT falloff, and that is what the change
//     is for. sigma does not only set the core - it multiplies reachSigmas, so
//     it sets the whole tail. At filamentFalloff 0.27 (N = 0.54) reachSigmas
//     clamps at FILAMENT_REACH_MAX_SIGMAS and the tail is 64 sigmas, so a flat
//     0.5 floor stretched a 31 px filament to 61 px at scale 0.5 and past
//     120 px at 0.25 - to buy 5 levels of peak accuracy (max per-phase error
//     13 -> 8) on a profile 31 px wide that the buffer was sampling perfectly
//     well. The share form asks for 0.082 buffer px there, below what the
//     caller already supplies, so it does not engage.
//
//     Where it DOES still engage it is doing the work it was added for: at the
//     default falloff, without any floor, the peak collapses to 153 against a
//     235 reference, and at filamentFalloff 1.5 to 24. The known cost of the
//     share form is the band around filamentFalloff 0.4 to 0.5, where it stops
//     engaging while the unfloored peak error is still 12 to 18 levels;
//     tightening MIN_SHARE to cover that would stop the default case being
//     bit-identical, so the crossover sits here deliberately. See
//     docs/corner-crease-and-filament-nyquist.md section 2.8.
//
//     The line does read WIDER at a reduced scale wherever the floor engages,
//     and that part is not a defect to tune away: a line the buffer cannot
//     sample cannot be reconstructed from it. The floor buys a stable,
//     correctly-bright line of the narrowest width the buffer can actually
//     carry. Deliberately NOT paired with an amplitude compensation to
//     conserve the line's integral - the grade at the end of neon.frag
//     tonemaps per-fragment INSIDE the buffer and FILAMENT_GAIN puts the core
//     deep in saturation, so scaling the linear amplitude barely moves the
//     8-bit value while it does measurably dim the peak (error 15-23/255
//     against 0-10 without it). See
//     docs/corner-crease-and-filament-nyquist.md section 2.
//
//     GATED to scales below 1.0 in both consumers. At scale 1.0 the reduced
//     buffer is full size and the blit copies it 1:1, so there is nothing to
//     survive, and the ring always shades at 1.0. The old flat constant was a
//     no-op at 1.0 by arithmetic
//     coincidence - it equalled the converted stated floor; this expression
//     would not be, above N = 2, so the gate is explicit now. ---
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_NYQUIST_SAMPLE_PX 1.0
#define FILAMENT_NYQUIST_MIN_SHARE 0.0625
#define FILAMENT_GAIN             12.0

// --- Continuous-arc filament gate feathers (neon.frag).
//     INWARD FEATHER: the smooth ramp sits INSIDE the arc's own perimeter
//     span, so nothing outside the arc gets lit -> no corner bleed regardless
//     of width, no perpendicular spike, and the profile is a plain smoothstep
//     that reads the same on straight edges and at corners. Trade-off: the
//     visible arc appears inset by these widths (arc lights up at
//     start + TAIL_FEATHER_PX and ends at start + length - HEAD_FEATHER_PX).
//     Values are pixel-space spans, divided by the current perimeter at the
//     call site, and capped per-arc by ARC_FEATHER_MAX_SHARE below. ---
#define HEAD_FEATHER_PX           14.0
#define TAIL_FEATHER_PX           14.0

// --- Cap on each feather, as a share of the arc's own length.
//
//     The feathers above are a fixed pixel span while an arc's length is a
//     FRACTION of the perimeter, so the same arc config is a different pixel
//     length on every rect. Below ~(HEAD + TAIL) px the two ramps overlapped
//     and ate into the arc's peak: an L = 0.02 arc peaked at 1.00 on an
//     800x600 rect but only 0.21 on a 200x150 one - the last place rect size
//     still reached brightness. Capping each feather at a share of the arc
//     length holds the peak at exactly 1.0 for any length at any size; a short
//     arc gets a proportionally shorter ramp instead of a truncated top.
//
//     0.4 leaves a 0.2 * length plateau at full brightness between the two
//     ramps. Up to 0.5 also holds the peak, but with no plateau the arc reads
//     as a spike rather than a flat-topped segment. Arcs longer than
//     (HEAD + TAIL) / 0.4 px never reach the cap, so the normal case is
//     bit-identical to before. ---
#define ARC_FEATHER_MAX_SHARE     0.4

// --- Halo (sharp coloured glow).
//
//     The halo and bloom are evaluated ANALYTICALLY from the rounded-box SDF
//     distance, not summed over the perimeter gather. For a locally straight
//     emitter of unit density the old sums converge exactly to:
//
//       sum g*sqrt(g) * spacing*kh^2*HALO_NORM  -> HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
//       sum 1/(dd+bw^2) * spacing*bw*BLOOM_NORM -> BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
//
//     so the NORM factors keep their meaning and their calibration: peak
//     values at ad = 0 are unchanged (0.86 and 1.005 respectively).
//
//     The closed form is what makes the effect geometry-independent. The
//     gather had to floor its kernel at a multiple of sampleSpacing to stop
//     128 discrete samples beading into dots, and sampleSpacing is
//     perimeter / NEON_MAX_LOOP_SAMPLES - so halo width, and via the gate its
//     brightness, both tracked the rect size, and glowRadius did nothing at
//     all until it exceeded the floor (~56 px on a 1920x1080 rect, i.e. most
//     of its usable range). An analytic profile cannot bead at any radius, so
//     no floor is needed and glowRadius sets the width directly.
//
//     Those two limits are of an INFINITE emitter, and evaluating one of them
//     at ad = the nearest-edge distance is what used to produce the interior
//     medial-axis creases - the dark wedges running in from each corner, plus
//     the central spine. A fragment on a corner diagonal has two edges equally
//     near and was lit by exactly one of them: measured against a fragment with
//     a single edge at the same distance, the ratio was 1.000 at every distance
//     tested, where physics says roughly 2.
//
//     neon.frag now sums the FINITE-SEGMENT form of the same two integrals over
//     the four straight edges instead (haloSegment / bloomSegment). Integrating
//     the same kernels from t1 to t2 along a segment at perpendicular distance
//     a gives elementary antiderivatives, and as t1 -> -inf, t2 -> +inf each one
//     reduces to exactly the limit above - so this is a strict generalisation
//     and the NORM factors keep the calibration they already had. The corner
//     sweep went from a V kinked at exactly 45 degrees (21 levels deep at
//     r = 160) to a smooth basin of 12, which is genuine falloff rather than a
//     crease. Cost is ~1.06-1.10x of the neon frame; the gather loop is still
//     the overwhelming majority of it.
//
//     The CREASE is what the four-segment form was adopted for. It is not the
//     largest thing it moves. The old expressions were functions of ad and of
//     nothing else, so the old profile was SYMMETRIC about the line - a
//     fragment 120 px inside the edge and one 120 px outside were lit
//     identically, by construction. Outside, the emitter recedes; inside, it
//     wraps around you. The segment sum knows the difference, so the exterior
//     now falls off slightly faster (mid-edge peak itself moves by 2/255) and
//     the INTERIOR stops falling off past roughly one rect-half and settles on
//     a floor. On the 1000x500 / glowRadius 60 probe the centre goes 162 ->
//     186 and the interior mean 174.5 -> 188.8; at glowRadius 30 on 800x400 it
//     is 81 -> 113. The glow reads as bigger, and the part that grew is the
//     interior - the exterior mean goes slightly DOWN.
//
//     Intended, and not tunable back out: a real rectangular tube does light
//     its own interior, and HALO_GAIN cannot be lowered to undo it without
//     dimming the line with it. The knobs for a perimeter-hugging glow are the
//     ones that always meant that - insideCutoff, and glowSide OUTSIDE.
//
//     THE CORNERS ARE A FIFTH THROUGH EIGHTH SEGMENT, not an extension of the
//     four. The straights are trimmed to their TANGENT POINTS, and each quarter
//     arc contributes one more segment: the arc DEVELOPED onto its own tangent
//     at whichever arc point is nearest the fragment, carrying the arc's full
//     length and split about that point, so it abuts the straights exactly in
//     arclength and the emitter is continuous. See neon.frag's
//     arcTangentSegment.
//
//     DEVELOPED AT RATE sqrt(rho * min(rho, r)), NOT AT RATE r, with the
//     measure put back by scaling the segment by r/rate - `rho` being the
//     fragment's distance from the arc centre. Laying an arc out one unit of
//     tangent per unit of arc is only right for a fragment ON it; the exact
//     distance to the point dphi away is a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,
//     whose slope at the foot is sqrt(rho*r). Rate r cost two things:
//
//       - at the CENTRE OF CURVATURE the whole arc ran off to one side of the
//         foot, and both kernels peak at t = 0, so it under-counted - to 54%
//         of the true value on a circle, where all four centres coincide at
//         the middle of the shape;
//       - and it CREASED, because the arc's endpoints slide at the tangent
//         rate on one side of the clamp and at rate 1 on the other, which
//         agree only where the rate is length(w). Every other point of the two
//         lines through an arc centre carried a C1 kink: an L-shaped seam per
//         corner, and on a circle an unmistakable dark cross. See
//         docs/review-findings.md V12.
//
//     rho == r gives rate r and weight 1, so a fragment on the tube is
//     bit-identical to the rate-r form and this calibration is untouched. The
//     whole emitter's worst error against a numerically integrated perimeter
//     falls on every geometry tested (16.5 -> 9.8% on a 600x400 r=40,
//     37.3 -> 26.3% on a circle).
//
//     The straights used to run to the SHARP corner instead, on the grounds
//     that over-extending them past the tangent point stood in for the arc that
//     has no elementary closed form. It does not stand in for it. The
//     over-extension is a phantom emitter a few px from a fragment that is tens
//     of px from the real tube: on an 800x400 rect at cornerRadius 40, a point
//     20 px outside the arc on the diagonal sits 2.4 px from EACH of the two
//     phantoms, and rendered 98 -> 146 across the commit that introduced them.
//     Against a numerically integrated rounded-rect perimeter it was 40 levels
//     too dark ON the tube and 50 too bright just outside it, which is what
//     made a rounded corner read square at a narrow glowRadius.
//
//     The developed arc costs one extra segment per corner - one atan and a
//     length on top - and takes the worst error over a quadrant from 76 to 6
//     levels at glowRadius 5, 112 to 6 at cornerRadius 120, 130 to 8 at 200.
//     A full circle (cornerRadius == halfMin) is still the residual case,
//     since its perimeter is four developed arcs and no straights at all.
//
//     Gated on uCornerRadius > 0, which is a UNIFORM - safe branching, and the
//     reason a sharp rect pays nothing and stays bit-identical (nine scenes
//     cmp-equal, not argued, and still cmp-equal after the rate fix above).
//     Rounded costs 1.19x of the neon pass against no arcs at all, and the rate
//     fix adds 1.03x on top - but the sharp path pays 1.026x of THAT for code
//     it never runs, because what decides this block is REGISTER PRESSURE, not
//     structure: with a per-arc bloom pedestal it was heavy enough to cost the
//     sharp path 1.14x. Re-time a cornerRadius 0 scene as well as a rounded one
//     after touching this. See docs/corner-crease-and-filament-nyquist.md
//     sections 1.7 to 1.9, and docs/review-findings.md V4, V10 and V12. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Width of the COLOUR gather kernel, as a FRACTION OF THE PERIMETER.
//
//     The perimeter colour blend is the one part of the shader that is still a
//     discrete sum over the loop samples, and it is the only place a length has
//     to be expressed this way rather than in pixels. The reason is that the
//     signal it filters - the gradient LUT - is itself parameterised by
//     perimeter fraction: sample i contributes LUT(i / NEON_MAX_LOOP_SAMPLES).
//     A kernel measured in pixels therefore covers a DIFFERENT span of the
//     gradient on every geometry, and the same colour stops render washed out
//     on a small rect and crisp on a large one. Measured on the stock 4-stop
//     ring, the colour sampled exactly on the red stop ran (0.95, 0.33, 0.06)
//     at 200x150 against (0.96, 0.04, 0.00) at 2800x2200 - roughly 8x the hue
//     bleed from geometry alone. Scaling the kernel with the perimeter is what
//     makes the gradient read identically at any size.
//
//     Two further properties fall out of the same choice:
//
//       - A constant anti-bead margin. The kernel has to stay >= about one
//         sample spacing, or it collapses between samples and the blend beads
//         into dots. Spacing is perimeter / NEON_MAX_LOOP_SAMPLES, so a
//         perimeter fraction pins the ratio at
//         COLOR_BLEND_PERIM_FRAC * NEON_MAX_LOOP_SAMPLES = 1.13 spacings on
//         every geometry, where the previous fixed 24 px span met the bound
//         only up to a ~3000 px perimeter and was down to 0.31 spacings by
//         9900 px. (No beading was actually measurable there - a DFT of the
//         hue around the perimeter put the 128-cycle component at the noise
//         floor - because the gradient varies slowly over one spacing on a
//         rect that large. The margin is a guarantee for dense colour stops,
//         not a fix for an observed artifact.)
//
//       - Sample-count independence. The kernel comes from the
//         NEON_MAX_LOOP_SAMPLES-based fraction rather than from the runtime
//         sample count, so the numSamples knob does not move the colour
//         blend. (An earlier sampleSpacing-derived floor divided by the live
//         count and landed 2x wider at reduced counts.)
//
//     Deliberately NOT coupled to glowRadius. The gather produces colour only -
//     the halo and bloom have been closed-form since they stopped riding on it -
//     so a wide glow has no reason to desaturate the gradient, and the old
//     max(glowRadius, floor) reintroduced exactly the pixel-space dependence
//     this constant exists to remove.
//
//     0.0088 puts the kernel at 24.0 px on the stock 800x600 / radius 40
//     geometry, matching the previous fixed span, so default-sized output is
//     unchanged.
//
//     NOTE: unit-free, unlike the px constants around it. The shader
//     multiplies it by a perimeter that is already in scaled px, so it needs
//     no uResolutionScale correction - applying one would double-apply. ---
#define COLOR_BLEND_PERIM_FRAC    0.0088

// --- Emission on/off ramp. glowRadius = 0 must read as "filament only", but
//     an analytic profile at radius 0 is a sub-pixel spike of full height
//     rather than nothing, so the halo and bloom fade in over
//     glowRadius = [0, this]. A FIXED pixel width: gating against the
//     sampleSpacing-derived floor instead would re-couple brightness to the
//     rect size, which is the whole thing this design removes.
//
//     NOTE: full-res pixel span, compared against a uGlowRadius that arrives
//     already scaled, so the shader multiplies this by uResolutionScale at
//     the point of use. Identity at scale 1.0. ---
#define GLOW_GATE_FADE_PX         2.0

// --- Lower bound on the analytic emission widths. Guards the divides only;
//     anything this small is already multiplied out by GLOW_GATE_FADE_PX. ---
#define EMISSION_MIN_WIDTH        1e-3

// --- Degenerate guard for the corner arc's tangent frame (neon.frag,
//     arcTangentSegment). The frame is built by normalising the fragment's
//     offset from the arc centre, clamped into the quarter the arc occupies;
//     that offset is the zero vector only for a fragment sitting exactly on an
//     arc centre, where the direction is undefined and atan(0, 0) is undefined
//     with it. Below this the frame falls back to the nearer of the two tangent
//     points, which is the correct clamp for the whole region the guard covers.
//
//     It floors `rho` in the same function, for the same fragment: the
//     development rate divides into the segment's weight, so an exactly-zero
//     rho would be a division by zero. The floored value is nowhere near a
//     pixel and the limit it lands on is the exact answer anyway, so this only
//     keeps a NaN out.
//
//     Unit-free: compared against a length in the shader's own px space, and
//     small enough that nothing but the exact degeneracy reaches it. ---
#define ARC_FRAME_EPSILON         1e-6

// --- Bloom (wide background spill). See the halo note above for the closed
//     form; BLOOM_SPACING_FLOOR is gone with the gather. ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_NORM_FACTOR         0.32

// --- Travelling-segment array size (shared by C++ vector cap + GLSL uniform array) ---
#define MAX_SEGMENT_BOOSTS        8

// --- Arc range array size (shared by C++ vector cap + GLSL uniform array).
//     Each arc gates a slice of the perimeter with its own colour + intensity.
//     Overlap resolves winner-take-all (max mask*intensity wins). ---
#define MAX_ARCS                  8

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO and
//     the shader's array. The gather iterates only uNumSamples of them
//     (NeonConfig::numSamples, which defaults to this), so it is a ceiling,
//     not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Glow coverage table (neon-glow-cover.frag, V20 and V21): for each piece
//     of the emitter and each fragment position round it, the arcs' and the
//     segments' coverage of that piece as the halo and the bloom see it. Laid
//     out in neon-pieces.glsl: four bands of ROWS rows, each holding one
//     straight at its left and one corner at its right. A band's columns are
//     shared between the two in proportion to their lengths (uGlowCoverSplit,
//     NeonRenderer::GetGlowCoverSplit), so a circle's corners get the columns
//     its straights do not need and a long rect's straights get its corners':
//     the straight has OVERHANG columns past each end, the corner
//     CORNER_OVERHANG on each side of its arc and a guard texel at each end of
//     its block, and the columns left over (the layout's `shared`) are split
//     between the two interiors, each keeping at least MIN_INTERIOR. ROWS 32 is half of them
//     within one halo width of a straight, and 16 inside and 16 outside a
//     corner's arc. 1024 x 128 RGBA16F is 1.0 MB - half V20's single table per
//     perimeter. Sizing it, against an exact per-piece reference (V21 in
//     docs/review-findings.md): 48 rows (1.5 MB) read at most 2 levels off where
//     32 reads 3 on a few pixels of a segment scene, for 1.3x the bake's cost
//     under animation; 768 columns read 6 levels off a 1 px halo on a 4K-sized
//     rect.
//
//     The WIDTH is not a constant: the table's layout is a parameter
//     (NeonRenderer's GlowCoverLayout, the shaders' uGlowCoverLayout), between
//     MIN_WIDTH and MAX_WIDTH in steps of WIDTH_STEP. ROWS and HEIGHT are. ---
#define GLOW_COVER_MAX_WIDTH      1024
#define GLOW_COVER_MIN_WIDTH      256
#define GLOW_COVER_WIDTH_STEP     64
#define GLOW_COVER_ROWS           32
#define GLOW_COVER_OVERHANG       64
#define GLOW_COVER_CORNER_OVERHANG 32
#define GLOW_COVER_MIN_INTERIOR   16
#define GLOW_COVER_HEIGHT         (4 * GLOW_COVER_ROWS)

// --- The glow coverage table's width (NeonRenderer's GetGlowCoverWidth):
//     enough interior columns that the longest band - its straight plus its
//     quarter arc, L full-res px - gets one every S px, S growing with the
//     halo width kh (max(glowRadius, EMISSION_MIN_WIDTH), px):
//
//         S    = clamp(PX_PER_COLUMN_PER_KH * kh, MIN_PX_PER_COLUMN, MAX_PX_PER_COLUMN)
//         need = 2 + 2 OVERHANG + 2 CORNER_OVERHANG + ceil(L / S)
//         W    = clamp(need rounded up to WIDTH_STEP, MIN_WIDTH, MAX_WIDTH)
//
//     In full-res px because the edge ring reads the table at full
//     resolution; lengths enter the layout only as ratios, so every scale
//     shares one table.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the 1024-wide
//     table (docs/neon-glow-cover-resolution-plan.md step 2): 250 configs -
//     rects 200 x 120 to 3600 x 2000, glowRadius 1 / 2 / 5 / 10 / 20,
//     cornerRadius 0 / 40 / a circle, both windings, three partial arcs with
//     and without colour stops and with two segments, at scales 1.0 / 0.5 /
//     0.25 - each at every width from 256 to 960, the frame after a light
//     change, against the same frame at 1024. The widest spacing that still
//     met the plan's criterion (max 2 levels, 99.9% of lit pixels within 1)
//     on every config was ~2.2 px per column at kh 1, 2.5 at 2, 4.6 at 5, 6.8
//     at 10 and 10 at 20. These constants sit under that envelope - 12% at
//     kh 5, 27% or more elsewhere - and at the widths they give, every config
//     read at most 2 levels with at least 99.988% of lit pixels within 1.
//     Spacing 20% wider failed at kh 5 (4 levels on a 200 px circle). A
//     narrow glow on a large rect still gets MAX_WIDTH: at kh 1-2 a
//     3600 x 2000 rect needed all 1024 columns. ---
#define GLOW_COVER_PX_PER_COLUMN_PER_KH 0.8
#define GLOW_COVER_MIN_PX_PER_COLUMN    1.6
#define GLOW_COVER_MAX_PX_PER_COLUMN    5.0

// --- The segment table (NeonRenderer's GetGlowCoverSegmentWidth,
//     neon-glow-cover-fill.frag): the segments' two channels of the glow
//     coverage table, baked at a width of their own and copied in. A bell's
//     standard deviation is 0.3536 x its length x the perimeter - ~100 px for
//     a length-0.05 segment on a 1840 x 1000 rect, against an arc's 14 px
//     feather - so it needs far fewer columns than the arcs:
//
//         sigma = 0.3536 x the shortest lit segment's length x perimeter   (full-res px)
//         need  = 2 + 2 SEG_OVERHANG + 2 SEG_CORNER_OVERHANG + ceil(L / (sigma / SEG_COLUMNS_PER_SIGMA))
//         W     = clamp(need rounded up to WIDTH_STEP, SEG_MIN_WIDTH, the main table's width)
//
//     with L the longest band, as for the main table. Where W would be at
//     least SEG_DIRECT_SHARE of the main table's width (very short segments)
//     the separate table saves nothing, and the segments are baked into the
//     main table directly, as before it existed.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the segments
//     baked into the main table directly (plan step 3): 135 configs - rects
//     200 x 120 to 3600 x 2000, glowRadius 2 / 5 / 10, cornerRadius 0 / 40 /
//     a circle, scales 1.0 / 0.5 / 0.25, segments of length 0.01-0.2 with
//     boosts 0.5-2, overlapping, abutting, over the corners, travelling, and
//     under a partial arc - the frame after a light change, the segment table
//     forced on. With overhangs 16 / 8, every COLUMNS_PER_SIGMA from 3 to 8
//     read at most 2 levels with 99.9992% of lit pixels within 1; 2 read 3
//     levels on a thin glow at 3840 x 2160. 4 is one step of margin. Overhangs
//     8 / 4 held at every value tried (99.988%), 32 / 16 read at most 1 level
//     from 4 up but do not fit SEG_MIN_WIDTH. ---
#define GLOW_COVER_SEG_COLUMNS_PER_SIGMA 4.0

// --- How far the glow coverage bake integrates a segment's bell either side
//     of its centre, in units of 1 / invSigma (the bell is exp(-(d invSigma)^2),
//     so 3 is 4.24 standard deviations, where it has fallen to 1.2e-4 of its
//     peak). The bake (neon-glow-cover.frag, segmentsOnPiece) cuts the bell
//     here and NeonRenderer's GetGlowCoverDirtySegmentPieces mirrors it to
//     find the pieces a moved segment reaches: change one and the other
//     follows, or a piece the bell reaches is not re-baked. It was 5 / sqrt(2)
//     (5 sigma); at 3 a moved segment dirties fewer pieces, which took the
//     segment table's bake on a band of 8 segments from 0.75 to 0.58 (timer
//     units, AMD Radeon Pro 5300M), at most 1 level, on up to 1.6% of a
//     frame's lit pixels (0.15% on average over the cover set; I61 in
//     docs/review-findings.md). An 8-node rule on top halved it again but
//     read 3 levels off a long, bright segment, and was not taken. ---
#define GLOW_COVER_BELL_REACH            3.0
#define GLOW_COVER_SEG_OVERHANG          16
#define GLOW_COVER_SEG_CORNER_OVERHANG   8
#define GLOW_COVER_SEG_MIN_WIDTH         128
#define GLOW_COVER_SEG_DIRECT_SHARE      0.75

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- The output dither (R7), peak to peak in 8-bit levels: neonDither
//     (neon-grade.glsl) adds noise in +/- half this to every colour the neon
//     writes to the caller's framebuffer - the blit, the edge ring and the
//     ring's field composite, never an offscreen buffer. The halo and bloom
//     are slow gradients that cross a level only every few px, up to tens of
//     px in the dark tail, so an undithered write draws concentric contour
//     rings around the rect. 1.0 is the least that removes them; 0 turns the
//     dither off. It assumes an 8-bit target. ---
#define OUTPUT_DITHER_LSB         1.0

// --- Epsilons ---
#define WSUM_EPSILON              1e-6

// --- One-sided cut guard band, in BUFFER pixels. Scaled path only.
//
//     One of the two constants in this file stated in buffer px rather than
//     full-res px (BLIT_CUTOFF_GUARD_PX below is the other): it describes the
//     BUFFER's own sampling, so it must NOT be converted with uResolutionScale.
//
//     For pass 1b the one-sided cut is not applied by neon.frag at all -
//     neon-blit.frag applies it at DESTINATION resolution, where a pixel is a
//     pixel and the edge can land exactly where the full-resolution shading
//     puts it. What neon.frag still does is cull the dark side, and the bound
//     it culls at cannot be the cut itself: the blit reconstructs each
//     destination pixel from the 2x2 buffer texels around it, so a texel
//     killed at the cut leaves the first LIT destination pixel rebuilt partly
//     from black. Measured at scale 0.5, glowSide OUTSIDE: the first lit pixel
//     came back at 178 against the 239 full resolution puts there, a dark seam
//     hugging the inside of the glow's own edge.
//
//     So the cull runs this far PAST the cut, and the guard band's emission is
//     what the blit's filter reconstructs the boundary from. One buffer texel
//     is the filter's reach; 2.0 covers that with room for the diagonal, where
//     fwidth of an SDF is sqrt(2) rather than 1, and for the half-texel of
//     phase between the cut and the nearest texel centre.
//
//     The band costs a two-texel strip along the perimeter - nothing beside
//     the half-plane the cull still removes - and none of it is visible: the
//     blit's mask is zero everywhere past the cut, including at the hard step
//     where this bound finally discards.
//
//     NOT used by the edge ring, which owns its own cut (uBlitOwnsCut 0). See
//     neon.frag's sideCull and the post-grade cut block. ---
#define BLIT_SIDE_GUARD_PX        2.0

// --- Cutoff guard band, in BUFFER pixels. Pass 1b only.
//
//     BLIT_SIDE_GUARD_PX's argument, applied to the inside/outside cutoffs.
//     For pass 1b neon-blit.frag applies the cutoff masks at
//     DESTINATION resolution, and neon.frag only culls - this far past the end
//     of each ramp, so the blit's bilinear filter rebuilds every boundary from
//     lit texels rather than from black. Same value as the side guard, for the
//     same reasons: one buffer texel of filter reach, plus room for the
//     diagonal and for the phase between the boundary and the nearest texel
//     centre.
//
//     It replaces CUTOFF_SOFT_FLOOR_PX, a feather in buffer px that let a mask
//     drawn INTO the buffer place its boundary sub-texel. That was the best the
//     buffer could do, and it still softened the boundary across a buffer
//     texel and snapped it toward the buffer grid: on the bounded_band scene of
//     docs/neon-resolution-scale-comparison.html it read p99 41/255 against the
//     1.0 render at scale 0.5, and 69 at 0.25, and the band spread past its
//     own cutoffs as the scale fell (50,703 lit px at 0.5 and 53,568 at 0.25,
//     against 47,012 at 1.0). Drawn by the blit instead it reads p99 4 at 0.5
//     and 12 at 0.25, with 47,012 lit px at every scale; what is left is the
//     reduced-resolution glow inside the band, not its edges. See
//     docs/neon-resolution-scale-plan.md, step 2.
//
//     NOT used by the edge ring, whose masks and culls are its own. See
//     neon.frag's cutGuard and NeonRenderer::setupGeometry's cutGuardPx. ---
#define BLIT_CUTOFF_GUARD_PX      2.0

// --- Edge ring width. CPU only.
//
//     At every scale, NeonRenderer redraws a ring around the rect
//     edge at FULL resolution (neon.frag's ring program),
//     reading only the gather's result from the gather buffer. The ring reaches R
//     full-res px either side of the edge (GetRingWidth in neon-renderer.cpp):
//
//         R = max(reach(1.0), reach(scale)) + RING_GUARD_TEXELS / scale
//
//     where reach(s) is the filament's reach as neon.frag computes it at scale
//     s, in full-res px. BOTH, because inside R the reduced pass's filament -
//     floored to what its buffer can sample, so wider than the real one
//     whenever the line is thin - is replaced by the real one, and R has to
//     clear whichever reaches further. Uncapped: the reach is where a filament
//     has fallen to FILAMENT_CUTOFF of its gain, about 2/255 after the grade,
//     which is exactly how far its error can show. RING_GUARD_TEXELS adds the
//     reduced buffer's bilinear footprint past that.
//
//     Calibrated over lineWidth {1, 2, 4, 8, 16} x filamentFalloff
//     {0.5, 1, 2, 4} x glowRadius {0, 2, 5, 20}, each config rendered at
//     scales 0.75, 0.5 and 0.25 and compared against its own 1.0 render
//     (docs/neon-resolution-scale-plan.md, step 5). Worst error over all 80:
//
//       RING_GUARD_TEXELS    scale 0.75    scale 0.5    scale 0.25
//              0.5               3            12            3
//              1.0               4             2            2
//              2.0               2             2            2
//
//     1.0 is the smallest that holds 2/255 at 0.5 and 0.25, the targets. 2.0
//     also brings 0.75 within 2 (its worst at 1.0 is a 1 px flat-top line
//     with no glow) for a ring one buffer texel wider each side.
//
//     The two things that made the first version of this ring fail that sweep,
//     both now handled: R used to cover only the full-res filament (fixed by
//     taking the reduced pass's too, above), and at a small glowRadius pass 1's
//     quad stopped short of R, so the ring read gather texels nothing wrote
//     (fixed by covering the ring with the quad; since the gather was split
//     out, the gather pass's own quad, built in setupRingGeometry, covers
//     it). ---
#define RING_GUARD_TEXELS         1.0

// --- Gather resolution. CPU only.
//
//     At every resolutionScale the gather - ~95% of neon.frag's cost when it
//     ran inline - runs in a pass of its own (neon-gather.frag) at its own
//     scale, and pass 1 and the
//     edge ring shade from its result. Its scale is (GetGatherScale in
//     neon-renderer.cpp)
//
//         clamp(GATHER_TEXELS_PER_KERNEL / kc, GATHER_MIN_SCALE,
//               min(resolutionScale, GATHER_MAX_SCALE))
//
//     with kc the colour kernel's width in full-res px, perimeter *
//     COLOR_BLEND_PERIM_FRAC: the gather's outputs are Lorentzian-weighted
//     means whose kernel is never narrower than kc, so this many texels per kc
//     carries them through a bilinear read.
//
//     Calibrated on Mesa llvmpipe with an RGBA16F gather buffer (worst error
//     against 1.0 over `probe scenes`, and the default scene's frame time):
//
//       GATHER_TEXELS_PER_KERNEL   worst error   default 0.5   default 0.25
//                1.0                   4              -              -
//                1.5                   2           10.5 ms        8.0 ms
//                2.0                  1-2          10.8 ms        7.3 ms
//                4.0                   -           14-17 ms      11-12 ms
//         (gather at resolutionScale)  1-2          37.5 ms       12.3 ms
//
//     2.0 is the knee: 1.5 saves nothing measurable (the gather is no longer
//     the pass that costs) and 4.0 costs half as much again for no visible
//     gain. With an RGBA8 gather buffer - the fallback - 2.0 reads 3/255.
//     GATHER_MIN_SCALE barely matters - full-viewport rects
//     at 720p and 1080p read 1-2 at a floor of 0 and 0.0625 alike - and is
//     kept as a cheap guard: the gather buffer is clipped to the viewport, so
//     at the floor it is at most 128 x 80 texels at 1080p. See
//     docs/neon-resolution-scale-plan.md section 13. ---
#define GATHER_TEXELS_PER_KERNEL  2.0
#define GATHER_MIN_SCALE          0.0625

// --- The gather's ceiling. CPU only.
//
//     GetGatherScale never gathers finer than this, at any resolutionScale.
//     It binds only on a rect whose perimeter is under ~450 px (kc under 4
//     px), which would otherwise gather near full resolution: its gather
//     buffer - RGBA16F over the glow quad's box, two attachments with
//     segments - ran to 16 / 33 MB at 1080p for a 40 x 24 rect whose glow
//     fills the frame. At 0.5 that is a quarter, the same as the scaled path
//     at 0.5 always gathered at, and the check suite's smallest rect (perimeter
//     478 px) is not touched.
//
//     It replaces the split gate at 1.0 (FULL_RES_SPLIT_GATHER_MAX_SCALE /
//     FULL_RES_SPLIT_MIN_AREA_PX, removed - docs/neon-shader-cleanup-plan.md
//     step 3), which ran the gather INLINE in neon.frag where splitting it
//     out did not pay: on an AMD Radeon Pro 5300M, where the first offscreen
//     pass of a frame costs ~0.15 ms, for a still cutoff band under ~140k px
//     of quad and for a gather near full resolution. Since unchanged frames
//     stopped re-running the gather (I40) the still case no longer leaves
//     the caller's framebuffer at all; an Apple M2 Pro measured the split
//     1.1-4.75x faster at every size and mode, and the AMD, re-measured with
//     every rect split against its own gate, 0.98-1.02x in every animated
//     mode with one loss - a cutoff band under an intensity pulse, 0.84x.
//     So every scale splits, and the inline loop went with the gate. ---
#define GATHER_MAX_SCALE          0.5

// --- Stand-in distance for a cutoff that glowSide has already subsumed.
//
//     neon.frag hands this to the band distance on the side the one-sided cut
//     culls, so the discard and the mask there no-op through the arithmetic
//     exactly as a DISABLED cutoff does - same mechanism, same sentinel shape,
//     no extra branch downstream.
//
//     Matches CUTOFF_DISABLED_SIZE in neon-renderer.cpp, which is what a
//     disabled cutoff actually arrives as. It is a distance in the shader's own
//     px space and only has to be far enough that no realistic geometry reaches
//     it; see the note on that constant for the same reasoning. ---
#define CUTOFF_NEUTRALISED        1.0e6

// --- Glow reach (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity).
//
//     Named for the reach, not for an early-out: the per-fragment
//     `ad > earlyOut -> discard` this constant was originally calibrated for
//     no longer exists. Geometry culls the far region instead, which is
//     tiler-friendly, so what the factor sets is how far the glow is allowed to
//     reach before the quad stops covering it. The locals it feeds already say
//     so - `glowReach` in both setupGeometry implementations, `reach` in both
//     shaders.
//
//     glowRadius only: the companion sampleSpacing * SPACING_FACTOR term is
//     gone. sampleSpacing is perimeter / NEON_MAX_LOOP_SAMPLES, so it won on
//     any reasonably large rect at default glowRadius and made both the
//     distance the bloom got truncated at and the brightness it still had
//     there track the rect size. The bloom's reach is a function of glowRadius
//     alone, so that is all that sizes the quad.
//
//     Used by the renderers' setupGeometry AND by the shaders, which recompute
//     the same expression to place the bloom pedestal that lets this margin
//     stay tight without the truncation showing. Keep the two in step. ---
#define GLOW_REACH_RADIUS_FACTOR  48.0

// --- Where the shaders' quad-edge fade begins, as a FRACTION of the quad
//     margin. The emission ramps to zero over [FRAC * margin, margin], so the
//     bloom's 1/D tail - still ~10% of peak out at the quad edge - never clips
//     as a hard rectangle.
//
//     A fraction, not a pixel span, because the margin is proportional to
//     glowRadius (see GLOW_REACH_RADIUS_FACTOR) and so is the bloom profile it
//     hides. A fraction keeps the ramp at a constant proportion of the bloom's
//     reach, so the fade reads the same at every glow radius; a fixed px ramp
//     would vanish on a wide glow and dominate a narrow one. At the stock
//     glowRadius 5 / bloom 0.3 / intensity 1 the margin is 312 px, giving a
//     62 px ramp.
//
//     Nothing derives 0.8 - anything leaving a ramp wide enough to hide the
//     clip behaves the same. What it DOES assume is that the margin was set by
//     the glow, where 20% of it is a long distance. When outsideCutoff clamps
//     the margin instead (to where its fade ends, size + softness, plus 1),
//     20% of ~13 px is 2.6 px and
//     the ramp lands INSIDE the cutoff band, dimming the band's outer edge
//     ahead of the cutoff mask - and only on the exterior, since the fade keys
//     on positive d. Both shaders therefore floor the ramp's start at the
//     cutoff boundary whenever that boundary falls inside the quad; see the
//     fadeStart block in neon.frag.
//
//     Since V23 the bloom no longer relies on this fade to end smoothly - each
//     piece fades by its own distance from BLOOM_FADE_START_FRAC, below, which
//     has it under a level well before this ramp starts. ---
#define QUAD_FADE_START_FRAC      0.8

// --- Where each piece's bloom starts fading out, as a FRACTION of `reach`
//     (neon.frag's bloom end fade, V23).
//
//     The pedestal takes the bloom's 1/a tail to exactly 0 at `reach`, but
//     with its slope still on, so the glow stopped on a crease: inside a large
//     rect, a hard-edged dark rectangle `reach` in. The quad-edge fade above
//     hid it outside, but over [0.8, 1.0] of the margin, where the tail is
//     still ~5 levels up, and a fade that late STEEPENS the tail before it
//     flattens it - a shoulder, then flat, which reads as an edge too. So
//     each piece's bloom is multiplied by 1 - smoothstep(FRAC * reach, reach,
//     a) of its own distance, on both sides.
//
//     0.5 is about the latest start that does not steepen it: the steepest
//     slope inside the fade over the slope where it starts, on the
//     infinite-line bloom at glowRadius 5, bloomStrength 0.52, intensity 0.8,
//     is 1.40 at 0.8, 1.14 at 0.6, 1.035 at 0.5 and 1.00 from 0.4 down. (It
//     is also where c * (reach - a)^2 meets the pedestalled 1/a tail in value
//     and slope.) An earlier start costs more of the tail; at 0.5 the glow
//     reads up to 6 levels dimmer in the outer half of its reach. ---
#define BLOOM_FADE_START_FRAC     0.5

// --- Filament reach for the same quad sizing, expressed in sigmas.
//
//     glowRadius = 0 means "filament only" - the halo and bloom are gated off
//     by GLOW_GATE_FADE_PX - but the filament itself is still there, sized by
//     lineWidth, so the quad has to cover it or the exterior gets clipped.
//
//     The reach is NOT a constant, because the profile is
//     exp2(-(ad/sigma)^N) with N = 2 * filamentFalloff and the slider runs
//     down to falloff 0. Solving core * FILAMENT_GAIN < FILAMENT_CUTOFF gives
//
//         reach = sigma * log2(FILAMENT_GAIN / FILAMENT_CUTOFF) ^ (1/N)
//
//     which is 1.9 sigmas at falloff 2.0, 3.5 at 1.0, 12.6 at 0.5, 67.8 at 0.3
//     and 558 at 0.2 - it diverges as falloff -> 0. A fixed 12 (calibrated for
//     falloff 0.5) left the low end badly under-sized: the interior is
//     unbounded because the quad always covers it, so a soft filament flooded
//     inward while the exterior was chopped at 12 sigmas. That asymmetry is
//     the "no outer glow at glowRadius 0" report.
//
//     MAX_SIGMAS bounds the fill cost where the formula diverges; the shaders
//     pedestal-subtract the core at exactly this reach, so hitting the cap
//     shortens the glow symmetrically on BOTH sides instead of showing a cliff
//     on the outside. MIN_SIGMAS keeps a little room at very high falloff.
//     Keep the expression in step across neon-tuning.h, both setupGeometry
//     implementations, and both fragment shaders. ---
#define FILAMENT_CUTOFF           0.002
#define FILAMENT_REACH_MIN_SIGMAS 2.0
#define FILAMENT_REACH_MAX_SIGMAS 64.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// What neon.frag and neon-gather.frag share
// ---------------------------------------------------------------------------
//
// Not a shader on its own. lib/CMakeLists.txt reads it and shaders.h.in
// injects it into both, after the version line and @NEON_TUNING@ and ahead of
// the file's own source - so everything here is declared before either file's
// first line, and neither declares any of it again.
//
// It is what the gather pass and the shading that reads it back both declare:
// the fragment position, the rect's shape, the segment block and the
// perimeter's length. The gather loop itself is neon-gather.frag's alone -
// it used to live here too, compiled out of neon.frag's reading variant, while
// neon.frag could also run it inline at 1.0; that path is gone
// (docs/neon-shader-cleanup-plan.md step 3).

precision highp float;

in vec2 vPos;

uniform vec2  uRectSize;
uniform float uCornerRadius;

// Travelling segments - up to MAX_SEGMENT_BOOSTS independent coloured lights
// on the perimeter. Each vec4 is packed as (position, invSigma, boost,
// hasStops): when .w > 0.5 the segment's colour comes from row `s` of
// uSegmentLUT (its own head-to-tail gradient); when .w == 0 it inherits the
// current base gradient sample. When uSegmentCount == 0 the whole feature is
// skipped in the gather loop.
//
// Here rather than in neon.frag because the gather branches on uSegmentCount;
// every neon program reads it, whichever file it is built from.
//
// Declared in a std140 uniform block (the DALi PunctualLightBlock pattern):
// DALi writes one element per registered property ("uSegments[0]", ...) into
// the block's UBO at the reflected std140 array stride. On desktop GL the
// block is fed from a UBO in neon-renderer.cpp.
layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

const float PI      = 3.141592653589793;
const float TWO_PI  = 6.283185307179586;
const float HALF_PI = 1.5707963267948966;

// Perimeter of the rounded rect, in the px uRectSize is in - SCALED px below
// resolutionScale 1.0. Sizes the gather's colour kernel, and neon.frag also
// converts the arc feathers from px to perimeter fractions with it.
float rectPerimeter() {
    float r = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    return 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + TWO_PI * r;
}

// ---------------------------------------------------------------------------
// The gather pass (pass 1a)
// ---------------------------------------------------------------------------
//
// At every NeonConfig::resolutionScale the perimeter gather - the loop that
// was ~95% of the neon's cost when it ran inline in every fragment - runs
// ONCE, alone, into NeonRenderer's gather buffer, at its own coarse scale
// (GetGatherScale in neon-renderer.cpp). neon.frag then shades everything else
// from what this writes: pass 1b at resolutionScale and the edge ring at full
// resolution. The inline path that ran the loop in neon.frag at 1.0 is gone
// (docs/neon-shader-cleanup-plan.md step 3), and so is the direct path that
// shaded the whole glow quad with the ring's program at 1.0 (I57). See
// docs/neon-resolution-scale-plan.md section 13 and docs/neon-perf-review.md
// section 10.
//
// precision, vPos, the shape uniforms and the segment block are in
// neon-common.glsl, which is injected ahead of this file and neon.frag. The
// loop and what only it reads - the sample block, the count, the emission
// table - are below; then what makes this a PASS: where the results go, and in
// what encoding.
//
// NO CULLS, unlike neon.frag. Its one-sided cut and its cutoffs discard; this
// pass must not. Its texels are read by passes at OTHER resolutions - pass 1
// and the edge ring - whose own culls sit at their own guard distances, and a
// texel culled here would hand their bilinear read a black hue just inside a
// boundary they still draw. So the gather runs over its whole quad
// (setupRingGeometry), which pass 1's geometry already bounds, and the cut is
// drawn by the passes that shade.

// --- The gather loop and everything it reads ---------------------------
// Here, not in neon-common.glsl, since this pass is the only program that
// runs it: every shading program reads its result back from the gather
// buffer (neon.frag).

// Loop sample positions (perimeter points) as a std140 uniform block. Each
// entry is packed as a vec4 (only .xy is meaningful; std140 pads vec2 to a
// 16-byte stride anyway) so the shader reads raw float32 out of the constant
// cache in the gather loop. Fixed size at compile time - see the shared
// NEON_MAX_LOOP_SAMPLES tuning constant.
layout(std140) uniform LoopSamplesBlock
{
    vec4 uLoopSamples[NEON_MAX_LOOP_SAMPLES];
};

// How many of the block's entries are actually in use this frame, from
// NeonConfig::numSamples (clamped to 1..NEON_MAX_LOOP_SAMPLES CPU-side). The
// block is always allocated at full size; entries past this are (0,0,0,0) and
// never read, because the gather stops here.
//
// The emission pre-pass is handed the SAME count and bakes its table over
// exactly these indices, so texel i there is sample i here. The two must agree
// or the gather reads emission belonging to a different perimeter position.
uniform int uNumSamples;

// Perimeter emission table from neon-emission.frag: NEON_MAX_LOOP_SAMPLES
// wide, 2 tall, RGBA16F (RGBA8 where the driver refuses float rendering).
//   row 0: .rgb = arcColour * arcW, .a = arcW
//   row 1: .rgb = SUM(segColour * bell), .a = SUM(bell)
// Read with texelFetch at integer sample index - never filtered, since
// neighbouring texels are unrelated perimeter samples. See the gather loop
// and docs/emission-prepass.md.
uniform sampler2D uEmission;

// The gather's four results - its ONLY outputs, and all four smooth across
// the screen (Lorentzian-weighted means over the whole perimeter), which is
// what lets neon-gather.frag store them on a grid far coarser than a pixel.
struct PerimeterGather
{
    vec3  hue;      // base perimeter hue, unit magnitude
    float arcCover; // g-weighted mean arc coverage x intensity
    vec3  segHue;   // segment hue, unit magnitude
    float segCover; // g-weighted mean segment boost x bell
};

// --- Colour gather ---------------------------------------------------------
// This loop gathers COLOUR ONLY - the halo and bloom intensities are computed
// in closed form by neon.frag after it, and every per-sample colour / mask
// term is precomputed by neon-emission.frag into uEmission.
//
// Per iteration: 1 UBO read for the sample position, 1 sub, 1 dot, 1
// reciprocal, and 2 texelFetches - 1 where the config has no segments and the
// branch below takes the shorter body. What used to live here - the arc
// winner-take-all scan over uArcCount, the segment loop over uSegmentCount,
// and one to two FILTERED LUT fetches - was a pure function of
// (si, uTime, config), so it did not belong in a loop that runs once per
// fragment. Hoisting it also removed two dynamic inner loops (which blocked
// unrolling), a serial reduction (`if (mask > bestMask)`), and the
// loop-carried `si += dti` chain.
//
// See docs/emission-prepass.md for the packing and the invariant that keeps
// the split honest.
PerimeterGather gatherPerimeter(vec2 p) {
    // kc is the COLOUR gather weight, and it is the one length in the neon
    // that is NOT in pixels. The blend is a uNumSamples-sample sum over
    // colours baked from the gradient LUT, which is indexed by perimeter
    // FRACTION, so a pixel-sized kernel spans a different slice of the
    // gradient on every geometry - the same stops render washed out small and
    // crisp large. Sizing it as a fraction of the perimeter makes the gradient
    // read identically at any size, and pins the kernel at a constant 1.13
    // sample spacings so it cannot bead at any size either. Not coupled to
    // glowRadius: the gather is colour only, so a wide glow has no business
    // desaturating the ring. See neon-tuning.h for the measurements behind
    // this, and neon.frag's kernel widths for the emission widths it is split
    // from.
    float kc  = max(rectPerimeter() * COLOR_BLEND_PERIM_FRAC, EMISSION_MIN_WIDTH);
    float kc2 = kc * kc;

    vec3  acc       = vec3(0.0); // base colour x arc-gated gather weight
    vec3  segAcc    = vec3(0.0); // segment colour x bell x gather weight
    float wsumLit   = 0.0; // SUM ARC-GATED g     - normalises `hue` (see below)
    float wsumSegW  = 0.0; // SUM SEGMENT bell*g  - normalises the segment hue
    // SUM UNGATED g. One add per iteration, and it buys the GLOW's magnitude:
    // wsumLit / wsumAll and wsumSegW / wsumAll are the g-weighted MEANS of the
    // two coverages over the perimeter, which is what the halo and bloom have
    // to be scaled by. See neon.frag's glow-coverage block.
    float wsumAll   = 0.0;

    // Runtime loop bound, from NeonConfig::numSamples. The UBO behind
    // uLoopSamples is always NEON_MAX_LOOP_SAMPLES long, so this only ever
    // stops the walk EARLY - it can never run off the end. Hoisted into a
    // local because a uniform in the condition is re-read per iteration on
    // some drivers.
    int n = uNumSamples;

    // TWO LOOP BODIES, ONE UNIFORM BRANCH, and the duplication is deliberate.
    //
    // Row 1 of the emission table is the segment term, and neon-emission.frag
    // writes it as vec4(segSum, bellSum) over a loop bounded by uSegmentCount.
    // At uSegmentCount == 0 that loop does not execute, so the row is exactly
    // vec4(0.0) at every texel - and the two accumulations it feeds here are
    // provably no-ops. A config with no segments was therefore issuing one
    // texture read per sample per fragment to add zero: 128 of them, over a
    // quad that covers most of the viewport at the default glowRadius.
    //
    // The branch has to be OUTSIDE the loop, not inside it. Gating the fetch
    // per iteration (`uSegmentCount > 0 ? texelFetch(...) : vec4(0.0)`) was
    // measured SLOWER than leaving the fetch alone - 4.52 ms against a 4.23 ms
    // baseline at 1920x1080 - because the per-iteration branch costs what the
    // fetch it skips cost. Hoisting it so each body is straight-line is what
    // actually pays: 1.46x on the whole neon layer, measured as an interleaved
    // A/B over seven rounds at two resolutions.
    //
    // That was when this loop ran inline in every fragment. Re-measured once
    // it ran only here, on the gather's coarse grid (AMD Radeon Pro 5300M,
    // 1920 x 1080): one body for both cases was still 0.88x on hue frames at
    // scale 1.0 (down to 0.72x on one scene) and 1.26x slower on a 40 x 24
    // rect, whose gather buffer is the largest - so keep both
    // (docs/neon-shader-cleanup-plan.md step 4).
    //
    // The uSegmentCount > 0 body below is the original loop VERBATIM, which is
    // what makes a segmented config byte-identical by construction rather than
    // by measurement. The segment-less body drops exactly the two dead
    // accumulations and the fetch that fed them; segAcc and wsumSegW keep the
    // vec3(0.0) / 0.0 they were initialised with, which is what the deleted
    // adds would have left them holding. Verified 0 of 2073600 pixels changed
    // across six scenes - no segments, a plain segment, a segment with its own
    // stops, four arcs with stops, both cutoffs enabled, and resolutionScale
    // 0.5.
    //
    // Branching on a uniform is safe here for the same reason the lens flare's
    // uSpread guard is (see lens-flare.frag): the condition is uniform across
    // the draw, so control flow stays uniform. Nothing in either body takes a
    // derivative in any case - texelFetch has no LOD to compute.
    if (uSegmentCount > 0) {
        for (int i = 0; i < n; i++) {
            vec2  dv  = p - uLoopSamples[i].xy;
            float dd  = dot(dv, dv);

            float g   = 1.0 / (dd + kc2);

            // Both rows of the emission table for this sample. Row 0 carries
            // the arc term already premultiplied by its own gather weight
            // arcW, plus arcW itself for the denominator; row 1 does the same
            // for the summed segment term. texelFetch (not texture): integer
            // sample index, no filtering, no wrap math, no LOD derivatives.
            vec4 e0 = texelFetch(uEmission, ivec2(i, 0), 0);
            vec4 e1 = texelFetch(uEmission, ivec2(i, 1), 0);

            // GATED normalisation, and it is the point. Dividing by the same
            // weight the numerator was gathered with makes `hue` a pure hue of
            // unit magnitude: it carries no coverage and no per-arc intensity,
            // both of which cancel. Those reach the emission solely through
            // emitCover / filamentGate in neon.frag, which are px-based and
            // size-invariant. segAcc / wsumSegW does the identical thing for
            // the segment hue.
            //
            // Both used to divide by an UNGATED sum over every sample, so an
            // unlit far side of the ring dragged the lit colour toward black by
            // roughly kc / rectHeight. With kc pinned to a fixed px span that
            // ratio grew as the rect shrank: a quarter-perimeter arc measured
            // 0.79 of full brightness at 200x150 against 0.97 at 1920x1080.
            // Gated normalisation is exactly 1.0 at every size.
            //
            // e0.rgb is baseColI * arcW and e0.a is arcW, so these two lines
            // are exactly the old `acc += baseColI * lg` / `wsumLit += lg` with
            // lg = g * arcW.
            acc      += e0.rgb * g;
            wsumLit  += e0.a   * g;

            // Segments are gathered with the raw proximity weight g, NOT the
            // arc-gated one, so a segment lights even on perimeter stretches no
            // arc covers. e1 holds SUM(segColour * bell) and SUM(bell) over
            // every segment, so the old inner loop collapses to one add each.
            segAcc   += e1.rgb * g;
            wsumSegW += e1.a   * g;

            wsumAll  += g;
        }
    } else {
        // No segments: row 1 is all zeros, so the fetch and the two adds it
        // feeds are dropped. Everything else is the body above, line for line.
        for (int i = 0; i < n; i++) {
            vec2  dv  = p - uLoopSamples[i].xy;
            float dd  = dot(dv, dv);

            float g   = 1.0 / (dd + kc2);

            vec4 e0 = texelFetch(uEmission, ivec2(i, 0), 0);

            acc      += e0.rgb * g;
            wsumLit  += e0.a   * g;
            wsumAll  += g;
        }
    }

    PerimeterGather result;
    // Both are pure hues of unit magnitude now; neon.frag attaches the
    // magnitudes from the pointwise coverages.
    result.hue    = acc    / max(wsumLit,  WSUM_EPSILON);
    result.segHue = segAcc / max(wsumSegW, WSUM_EPSILON);

    // The glow's two coverages: each SUM(cover * g) over SUM(g), the g-weighted
    // mean of that coverage over the perimeter. Why the glow is scaled by a
    // gathered coverage rather than the pointwise one is neon.frag's
    // glow-coverage block.
    //
    // Two divides rather than one reciprocal and two multiplies: a fully lit
    // ring rests on wsumLit / wsumAll being exactly 1.0 when the two sums are
    // equal, and x * (1.0 / x) is not. Outside the loop, so it costs one extra
    // divide per fragment, not per sample.
    float wsumDen   = max(wsumAll, WSUM_EPSILON);
    result.arcCover = wsumLit  / wsumDen; // arc coverage x intensity
    result.segCover = wsumSegW / wsumDen; // segment boost x bell
    return result;
}

// Both outputs get an explicit location because GLSL ES 3.0 requires that once
// there is more than one.
layout(location = 0) out vec4 oGather;
layout(location = 1) out vec4 oGatherSeg;

void main() {
    PerimeterGather gather = gatherPerimeter(vPos);

    // THE GATHER BUFFER: the loop's four results, for the passes that shade
    // from it to read back instead of running it. A coarse grid can carry them
    // where it cannot carry the filament because every one is a smooth
    // Lorentzian-weighted mean over the whole perimeter. See
    // docs/neon-resolution-scale-plan.md, section 0.
    //
    // The buffer is RGBA16F where the driver renders to it and RGBA8 where
    // not, so each value has to fit [0, 1] for the fallback. The hues already
    // do - they are weighted means of stop colours - and are clamped only so a
    // stop authored above 1 cannot wrap. The coverages do not: arc intensity
    // and segment boost both fold into them unbounded. c / (1 + c) maps
    // [0, inf) onto [0, 1) monotonically, is exact at 0, and inverts as
    // e / (1 - e) - neon.frag's gather read is the other half of
    // this pair, and the two have to change together. A fully lit ring's 1.0
    // stores as 0.5.
    //
    // Location 1 lands only when the gather buffer has a second attachment,
    // which the renderer gives it only when there are segments. Without one
    // the write is dropped by GL, and segHue and segCover are 0 anyway.
    oGather    = vec4(clamp(gather.hue,    0.0, 1.0), gather.arcCover / (1.0 + gather.arcCover));
    oGatherSeg = vec4(clamp(gather.segHue, 0.0, 1.0), gather.segCover / (1.0 + gather.segCover));
}

)shadersrc";

                static const char *const NEON_EMISSION_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - the neon shaders (neon.frag, neon-emission.frag), where CMake
//     text-injects this file via @NEON_TUNING@ in shaders.h.in, and
//   - the C++ renderer (neon-renderer.cpp), which #includes it for the
//     glow-reach quad-sizing factors.
//
// The px constants below are written in FULL-RESOLUTION pixels. Where the
// renderer draws at a reduced NeonConfig::resolutionScale, the shader converts
// each one with uResolutionScale at the point of use; the notes on the
// individual constants say which need it and which do not.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a `const float X = 0.9f;` cannot
// be written once and stay valid in both languages. Plain #define is the only
// form that compiles identically as GLSL and as C++ from a single file.
// (For C++-only constants, prefer constexpr - macros are used here strictly
// because these values must also live inside the shaders.)
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
//
//     FILAMENT_MIN_HALF_WIDTH is a STATED-width floor: it is what makes
//     lineWidth 0 mean "no line" rather than a single bright dot, and it is
//     paired with lineGate, which fades the filament in over the same span. It
//     describes the width the caller asked for, so it is a FULL-RES px constant
//     and the shader converts it with uResolutionScale like every other one.
//
//     The SAMPLING floor is the pair below, and the distinction from the
//     stated floor is the whole point of having two of them. Below
//     resolutionScale 1.0 the gather rasterises into a reduced buffer and
//     neon-blit.frag bilinearly upsamples it, so a filament the buffer cannot
//     sample is not reconstructed - it is resampled, and what comes out
//     depends on where the rect edge happens to fall between buffer texel
//     centres. That is a property of the BUFFER, so these are in BUFFER px and
//     are NOT converted with uResolutionScale.
//
//     Getting that backwards is what made a 1 px line at scale 0.5 disagree
//     with the same line at 1.0. sigma was floored only by the converted
//     stated constant, which at scale 0.5 is 0.25 buffer px, so the line sat
//     under the buffer's Nyquist limit and its peak swung with the rect's
//     sub-pixel position - 217 on a texel centre, 180 half a texel off,
//     against a 1.0 reference that holds 230-241 at every position.
//
//     THE FLOOR IS NOT A FIXED HALF WIDTH. It was one - a flat 0.5 buffer px -
//     and a fixed half width asks the wrong question. What survives the blit
//     is not decided by the profile's width at half maximum; it is decided by
//     how much signal the NEIGHBOURING buffer texel still carries, because
//     that is what the bilinear filter rebuilds the peak from. So the floor
//     says exactly that: the profile must still be at
//     FILAMENT_NYQUIST_MIN_SHARE of its peak, FILAMENT_NYQUIST_SAMPLE_PX out.
//     Inverting the generalized Gaussian for sigma gives
//
//         sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
//
//     which neon.frag and NeonRenderer::setupGeometry both evaluate.
//
//     0.0625 and 1.0 are today's behaviour restated, not a retune: at the
//     DEFAULT falloff (N = 2) the expression is exactly 0.5 buffer px, the
//     constant it replaces, so nothing at or near the default moves.
//
//     The two questions diverge at a SOFT falloff, and that is what the change
//     is for. sigma does not only set the core - it multiplies reachSigmas, so
//     it sets the whole tail. At filamentFalloff 0.27 (N = 0.54) reachSigmas
//     clamps at FILAMENT_REACH_MAX_SIGMAS and the tail is 64 sigmas, so a flat
//     0.5 floor stretched a 31 px filament to 61 px at scale 0.5 and past
//     120 px at 0.25 - to buy 5 levels of peak accuracy (max per-phase error
//     13 -> 8) on a profile 31 px wide that the buffer was sampling perfectly
//     well. The share form asks for 0.082 buffer px there, below what the
//     caller already supplies, so it does not engage.
//
//     Where it DOES still engage it is doing the work it was added for: at the
//     default falloff, without any floor, the peak collapses to 153 against a
//     235 reference, and at filamentFalloff 1.5 to 24. The known cost of the
//     share form is the band around filamentFalloff 0.4 to 0.5, where it stops
//     engaging while the unfloored peak error is still 12 to 18 levels;
//     tightening MIN_SHARE to cover that would stop the default case being
//     bit-identical, so the crossover sits here deliberately. See
//     docs/corner-crease-and-filament-nyquist.md section 2.8.
//
//     The line does read WIDER at a reduced scale wherever the floor engages,
//     and that part is not a defect to tune away: a line the buffer cannot
//     sample cannot be reconstructed from it. The floor buys a stable,
//     correctly-bright line of the narrowest width the buffer can actually
//     carry. Deliberately NOT paired with an amplitude compensation to
//     conserve the line's integral - the grade at the end of neon.frag
//     tonemaps per-fragment INSIDE the buffer and FILAMENT_GAIN puts the core
//     deep in saturation, so scaling the linear amplitude barely moves the
//     8-bit value while it does measurably dim the peak (error 15-23/255
//     against 0-10 without it). See
//     docs/corner-crease-and-filament-nyquist.md section 2.
//
//     GATED to scales below 1.0 in both consumers. At scale 1.0 the reduced
//     buffer is full size and the blit copies it 1:1, so there is nothing to
//     survive, and the ring always shades at 1.0. The old flat constant was a
//     no-op at 1.0 by arithmetic
//     coincidence - it equalled the converted stated floor; this expression
//     would not be, above N = 2, so the gate is explicit now. ---
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_NYQUIST_SAMPLE_PX 1.0
#define FILAMENT_NYQUIST_MIN_SHARE 0.0625
#define FILAMENT_GAIN             12.0

// --- Continuous-arc filament gate feathers (neon.frag).
//     INWARD FEATHER: the smooth ramp sits INSIDE the arc's own perimeter
//     span, so nothing outside the arc gets lit -> no corner bleed regardless
//     of width, no perpendicular spike, and the profile is a plain smoothstep
//     that reads the same on straight edges and at corners. Trade-off: the
//     visible arc appears inset by these widths (arc lights up at
//     start + TAIL_FEATHER_PX and ends at start + length - HEAD_FEATHER_PX).
//     Values are pixel-space spans, divided by the current perimeter at the
//     call site, and capped per-arc by ARC_FEATHER_MAX_SHARE below. ---
#define HEAD_FEATHER_PX           14.0
#define TAIL_FEATHER_PX           14.0

// --- Cap on each feather, as a share of the arc's own length.
//
//     The feathers above are a fixed pixel span while an arc's length is a
//     FRACTION of the perimeter, so the same arc config is a different pixel
//     length on every rect. Below ~(HEAD + TAIL) px the two ramps overlapped
//     and ate into the arc's peak: an L = 0.02 arc peaked at 1.00 on an
//     800x600 rect but only 0.21 on a 200x150 one - the last place rect size
//     still reached brightness. Capping each feather at a share of the arc
//     length holds the peak at exactly 1.0 for any length at any size; a short
//     arc gets a proportionally shorter ramp instead of a truncated top.
//
//     0.4 leaves a 0.2 * length plateau at full brightness between the two
//     ramps. Up to 0.5 also holds the peak, but with no plateau the arc reads
//     as a spike rather than a flat-topped segment. Arcs longer than
//     (HEAD + TAIL) / 0.4 px never reach the cap, so the normal case is
//     bit-identical to before. ---
#define ARC_FEATHER_MAX_SHARE     0.4

// --- Halo (sharp coloured glow).
//
//     The halo and bloom are evaluated ANALYTICALLY from the rounded-box SDF
//     distance, not summed over the perimeter gather. For a locally straight
//     emitter of unit density the old sums converge exactly to:
//
//       sum g*sqrt(g) * spacing*kh^2*HALO_NORM  -> HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
//       sum 1/(dd+bw^2) * spacing*bw*BLOOM_NORM -> BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
//
//     so the NORM factors keep their meaning and their calibration: peak
//     values at ad = 0 are unchanged (0.86 and 1.005 respectively).
//
//     The closed form is what makes the effect geometry-independent. The
//     gather had to floor its kernel at a multiple of sampleSpacing to stop
//     128 discrete samples beading into dots, and sampleSpacing is
//     perimeter / NEON_MAX_LOOP_SAMPLES - so halo width, and via the gate its
//     brightness, both tracked the rect size, and glowRadius did nothing at
//     all until it exceeded the floor (~56 px on a 1920x1080 rect, i.e. most
//     of its usable range). An analytic profile cannot bead at any radius, so
//     no floor is needed and glowRadius sets the width directly.
//
//     Those two limits are of an INFINITE emitter, and evaluating one of them
//     at ad = the nearest-edge distance is what used to produce the interior
//     medial-axis creases - the dark wedges running in from each corner, plus
//     the central spine. A fragment on a corner diagonal has two edges equally
//     near and was lit by exactly one of them: measured against a fragment with
//     a single edge at the same distance, the ratio was 1.000 at every distance
//     tested, where physics says roughly 2.
//
//     neon.frag now sums the FINITE-SEGMENT form of the same two integrals over
//     the four straight edges instead (haloSegment / bloomSegment). Integrating
//     the same kernels from t1 to t2 along a segment at perpendicular distance
//     a gives elementary antiderivatives, and as t1 -> -inf, t2 -> +inf each one
//     reduces to exactly the limit above - so this is a strict generalisation
//     and the NORM factors keep the calibration they already had. The corner
//     sweep went from a V kinked at exactly 45 degrees (21 levels deep at
//     r = 160) to a smooth basin of 12, which is genuine falloff rather than a
//     crease. Cost is ~1.06-1.10x of the neon frame; the gather loop is still
//     the overwhelming majority of it.
//
//     The CREASE is what the four-segment form was adopted for. It is not the
//     largest thing it moves. The old expressions were functions of ad and of
//     nothing else, so the old profile was SYMMETRIC about the line - a
//     fragment 120 px inside the edge and one 120 px outside were lit
//     identically, by construction. Outside, the emitter recedes; inside, it
//     wraps around you. The segment sum knows the difference, so the exterior
//     now falls off slightly faster (mid-edge peak itself moves by 2/255) and
//     the INTERIOR stops falling off past roughly one rect-half and settles on
//     a floor. On the 1000x500 / glowRadius 60 probe the centre goes 162 ->
//     186 and the interior mean 174.5 -> 188.8; at glowRadius 30 on 800x400 it
//     is 81 -> 113. The glow reads as bigger, and the part that grew is the
//     interior - the exterior mean goes slightly DOWN.
//
//     Intended, and not tunable back out: a real rectangular tube does light
//     its own interior, and HALO_GAIN cannot be lowered to undo it without
//     dimming the line with it. The knobs for a perimeter-hugging glow are the
//     ones that always meant that - insideCutoff, and glowSide OUTSIDE.
//
//     THE CORNERS ARE A FIFTH THROUGH EIGHTH SEGMENT, not an extension of the
//     four. The straights are trimmed to their TANGENT POINTS, and each quarter
//     arc contributes one more segment: the arc DEVELOPED onto its own tangent
//     at whichever arc point is nearest the fragment, carrying the arc's full
//     length and split about that point, so it abuts the straights exactly in
//     arclength and the emitter is continuous. See neon.frag's
//     arcTangentSegment.
//
//     DEVELOPED AT RATE sqrt(rho * min(rho, r)), NOT AT RATE r, with the
//     measure put back by scaling the segment by r/rate - `rho` being the
//     fragment's distance from the arc centre. Laying an arc out one unit of
//     tangent per unit of arc is only right for a fragment ON it; the exact
//     distance to the point dphi away is a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,
//     whose slope at the foot is sqrt(rho*r). Rate r cost two things:
//
//       - at the CENTRE OF CURVATURE the whole arc ran off to one side of the
//         foot, and both kernels peak at t = 0, so it under-counted - to 54%
//         of the true value on a circle, where all four centres coincide at
//         the middle of the shape;
//       - and it CREASED, because the arc's endpoints slide at the tangent
//         rate on one side of the clamp and at rate 1 on the other, which
//         agree only where the rate is length(w). Every other point of the two
//         lines through an arc centre carried a C1 kink: an L-shaped seam per
//         corner, and on a circle an unmistakable dark cross. See
//         docs/review-findings.md V12.
//
//     rho == r gives rate r and weight 1, so a fragment on the tube is
//     bit-identical to the rate-r form and this calibration is untouched. The
//     whole emitter's worst error against a numerically integrated perimeter
//     falls on every geometry tested (16.5 -> 9.8% on a 600x400 r=40,
//     37.3 -> 26.3% on a circle).
//
//     The straights used to run to the SHARP corner instead, on the grounds
//     that over-extending them past the tangent point stood in for the arc that
//     has no elementary closed form. It does not stand in for it. The
//     over-extension is a phantom emitter a few px from a fragment that is tens
//     of px from the real tube: on an 800x400 rect at cornerRadius 40, a point
//     20 px outside the arc on the diagonal sits 2.4 px from EACH of the two
//     phantoms, and rendered 98 -> 146 across the commit that introduced them.
//     Against a numerically integrated rounded-rect perimeter it was 40 levels
//     too dark ON the tube and 50 too bright just outside it, which is what
//     made a rounded corner read square at a narrow glowRadius.
//
//     The developed arc costs one extra segment per corner - one atan and a
//     length on top - and takes the worst error over a quadrant from 76 to 6
//     levels at glowRadius 5, 112 to 6 at cornerRadius 120, 130 to 8 at 200.
//     A full circle (cornerRadius == halfMin) is still the residual case,
//     since its perimeter is four developed arcs and no straights at all.
//
//     Gated on uCornerRadius > 0, which is a UNIFORM - safe branching, and the
//     reason a sharp rect pays nothing and stays bit-identical (nine scenes
//     cmp-equal, not argued, and still cmp-equal after the rate fix above).
//     Rounded costs 1.19x of the neon pass against no arcs at all, and the rate
//     fix adds 1.03x on top - but the sharp path pays 1.026x of THAT for code
//     it never runs, because what decides this block is REGISTER PRESSURE, not
//     structure: with a per-arc bloom pedestal it was heavy enough to cost the
//     sharp path 1.14x. Re-time a cornerRadius 0 scene as well as a rounded one
//     after touching this. See docs/corner-crease-and-filament-nyquist.md
//     sections 1.7 to 1.9, and docs/review-findings.md V4, V10 and V12. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Width of the COLOUR gather kernel, as a FRACTION OF THE PERIMETER.
//
//     The perimeter colour blend is the one part of the shader that is still a
//     discrete sum over the loop samples, and it is the only place a length has
//     to be expressed this way rather than in pixels. The reason is that the
//     signal it filters - the gradient LUT - is itself parameterised by
//     perimeter fraction: sample i contributes LUT(i / NEON_MAX_LOOP_SAMPLES).
//     A kernel measured in pixels therefore covers a DIFFERENT span of the
//     gradient on every geometry, and the same colour stops render washed out
//     on a small rect and crisp on a large one. Measured on the stock 4-stop
//     ring, the colour sampled exactly on the red stop ran (0.95, 0.33, 0.06)
//     at 200x150 against (0.96, 0.04, 0.00) at 2800x2200 - roughly 8x the hue
//     bleed from geometry alone. Scaling the kernel with the perimeter is what
//     makes the gradient read identically at any size.
//
//     Two further properties fall out of the same choice:
//
//       - A constant anti-bead margin. The kernel has to stay >= about one
//         sample spacing, or it collapses between samples and the blend beads
//         into dots. Spacing is perimeter / NEON_MAX_LOOP_SAMPLES, so a
//         perimeter fraction pins the ratio at
//         COLOR_BLEND_PERIM_FRAC * NEON_MAX_LOOP_SAMPLES = 1.13 spacings on
//         every geometry, where the previous fixed 24 px span met the bound
//         only up to a ~3000 px perimeter and was down to 0.31 spacings by
//         9900 px. (No beading was actually measurable there - a DFT of the
//         hue around the perimeter put the 128-cycle component at the noise
//         floor - because the gradient varies slowly over one spacing on a
//         rect that large. The margin is a guarantee for dense colour stops,
//         not a fix for an observed artifact.)
//
//       - Sample-count independence. The kernel comes from the
//         NEON_MAX_LOOP_SAMPLES-based fraction rather than from the runtime
//         sample count, so the numSamples knob does not move the colour
//         blend. (An earlier sampleSpacing-derived floor divided by the live
//         count and landed 2x wider at reduced counts.)
//
//     Deliberately NOT coupled to glowRadius. The gather produces colour only -
//     the halo and bloom have been closed-form since they stopped riding on it -
//     so a wide glow has no reason to desaturate the gradient, and the old
//     max(glowRadius, floor) reintroduced exactly the pixel-space dependence
//     this constant exists to remove.
//
//     0.0088 puts the kernel at 24.0 px on the stock 800x600 / radius 40
//     geometry, matching the previous fixed span, so default-sized output is
//     unchanged.
//
//     NOTE: unit-free, unlike the px constants around it. The shader
//     multiplies it by a perimeter that is already in scaled px, so it needs
//     no uResolutionScale correction - applying one would double-apply. ---
#define COLOR_BLEND_PERIM_FRAC    0.0088

// --- Emission on/off ramp. glowRadius = 0 must read as "filament only", but
//     an analytic profile at radius 0 is a sub-pixel spike of full height
//     rather than nothing, so the halo and bloom fade in over
//     glowRadius = [0, this]. A FIXED pixel width: gating against the
//     sampleSpacing-derived floor instead would re-couple brightness to the
//     rect size, which is the whole thing this design removes.
//
//     NOTE: full-res pixel span, compared against a uGlowRadius that arrives
//     already scaled, so the shader multiplies this by uResolutionScale at
//     the point of use. Identity at scale 1.0. ---
#define GLOW_GATE_FADE_PX         2.0

// --- Lower bound on the analytic emission widths. Guards the divides only;
//     anything this small is already multiplied out by GLOW_GATE_FADE_PX. ---
#define EMISSION_MIN_WIDTH        1e-3

// --- Degenerate guard for the corner arc's tangent frame (neon.frag,
//     arcTangentSegment). The frame is built by normalising the fragment's
//     offset from the arc centre, clamped into the quarter the arc occupies;
//     that offset is the zero vector only for a fragment sitting exactly on an
//     arc centre, where the direction is undefined and atan(0, 0) is undefined
//     with it. Below this the frame falls back to the nearer of the two tangent
//     points, which is the correct clamp for the whole region the guard covers.
//
//     It floors `rho` in the same function, for the same fragment: the
//     development rate divides into the segment's weight, so an exactly-zero
//     rho would be a division by zero. The floored value is nowhere near a
//     pixel and the limit it lands on is the exact answer anyway, so this only
//     keeps a NaN out.
//
//     Unit-free: compared against a length in the shader's own px space, and
//     small enough that nothing but the exact degeneracy reaches it. ---
#define ARC_FRAME_EPSILON         1e-6

// --- Bloom (wide background spill). See the halo note above for the closed
//     form; BLOOM_SPACING_FLOOR is gone with the gather. ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_NORM_FACTOR         0.32

// --- Travelling-segment array size (shared by C++ vector cap + GLSL uniform array) ---
#define MAX_SEGMENT_BOOSTS        8

// --- Arc range array size (shared by C++ vector cap + GLSL uniform array).
//     Each arc gates a slice of the perimeter with its own colour + intensity.
//     Overlap resolves winner-take-all (max mask*intensity wins). ---
#define MAX_ARCS                  8

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO and
//     the shader's array. The gather iterates only uNumSamples of them
//     (NeonConfig::numSamples, which defaults to this), so it is a ceiling,
//     not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Glow coverage table (neon-glow-cover.frag, V20 and V21): for each piece
//     of the emitter and each fragment position round it, the arcs' and the
//     segments' coverage of that piece as the halo and the bloom see it. Laid
//     out in neon-pieces.glsl: four bands of ROWS rows, each holding one
//     straight at its left and one corner at its right. A band's columns are
//     shared between the two in proportion to their lengths (uGlowCoverSplit,
//     NeonRenderer::GetGlowCoverSplit), so a circle's corners get the columns
//     its straights do not need and a long rect's straights get its corners':
//     the straight has OVERHANG columns past each end, the corner
//     CORNER_OVERHANG on each side of its arc and a guard texel at each end of
//     its block, and the columns left over (the layout's `shared`) are split
//     between the two interiors, each keeping at least MIN_INTERIOR. ROWS 32 is half of them
//     within one halo width of a straight, and 16 inside and 16 outside a
//     corner's arc. 1024 x 128 RGBA16F is 1.0 MB - half V20's single table per
//     perimeter. Sizing it, against an exact per-piece reference (V21 in
//     docs/review-findings.md): 48 rows (1.5 MB) read at most 2 levels off where
//     32 reads 3 on a few pixels of a segment scene, for 1.3x the bake's cost
//     under animation; 768 columns read 6 levels off a 1 px halo on a 4K-sized
//     rect.
//
//     The WIDTH is not a constant: the table's layout is a parameter
//     (NeonRenderer's GlowCoverLayout, the shaders' uGlowCoverLayout), between
//     MIN_WIDTH and MAX_WIDTH in steps of WIDTH_STEP. ROWS and HEIGHT are. ---
#define GLOW_COVER_MAX_WIDTH      1024
#define GLOW_COVER_MIN_WIDTH      256
#define GLOW_COVER_WIDTH_STEP     64
#define GLOW_COVER_ROWS           32
#define GLOW_COVER_OVERHANG       64
#define GLOW_COVER_CORNER_OVERHANG 32
#define GLOW_COVER_MIN_INTERIOR   16
#define GLOW_COVER_HEIGHT         (4 * GLOW_COVER_ROWS)

// --- The glow coverage table's width (NeonRenderer's GetGlowCoverWidth):
//     enough interior columns that the longest band - its straight plus its
//     quarter arc, L full-res px - gets one every S px, S growing with the
//     halo width kh (max(glowRadius, EMISSION_MIN_WIDTH), px):
//
//         S    = clamp(PX_PER_COLUMN_PER_KH * kh, MIN_PX_PER_COLUMN, MAX_PX_PER_COLUMN)
//         need = 2 + 2 OVERHANG + 2 CORNER_OVERHANG + ceil(L / S)
//         W    = clamp(need rounded up to WIDTH_STEP, MIN_WIDTH, MAX_WIDTH)
//
//     In full-res px because the edge ring reads the table at full
//     resolution; lengths enter the layout only as ratios, so every scale
//     shares one table.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the 1024-wide
//     table (docs/neon-glow-cover-resolution-plan.md step 2): 250 configs -
//     rects 200 x 120 to 3600 x 2000, glowRadius 1 / 2 / 5 / 10 / 20,
//     cornerRadius 0 / 40 / a circle, both windings, three partial arcs with
//     and without colour stops and with two segments, at scales 1.0 / 0.5 /
//     0.25 - each at every width from 256 to 960, the frame after a light
//     change, against the same frame at 1024. The widest spacing that still
//     met the plan's criterion (max 2 levels, 99.9% of lit pixels within 1)
//     on every config was ~2.2 px per column at kh 1, 2.5 at 2, 4.6 at 5, 6.8
//     at 10 and 10 at 20. These constants sit under that envelope - 12% at
//     kh 5, 27% or more elsewhere - and at the widths they give, every config
//     read at most 2 levels with at least 99.988% of lit pixels within 1.
//     Spacing 20% wider failed at kh 5 (4 levels on a 200 px circle). A
//     narrow glow on a large rect still gets MAX_WIDTH: at kh 1-2 a
//     3600 x 2000 rect needed all 1024 columns. ---
#define GLOW_COVER_PX_PER_COLUMN_PER_KH 0.8
#define GLOW_COVER_MIN_PX_PER_COLUMN    1.6
#define GLOW_COVER_MAX_PX_PER_COLUMN    5.0

// --- The segment table (NeonRenderer's GetGlowCoverSegmentWidth,
//     neon-glow-cover-fill.frag): the segments' two channels of the glow
//     coverage table, baked at a width of their own and copied in. A bell's
//     standard deviation is 0.3536 x its length x the perimeter - ~100 px for
//     a length-0.05 segment on a 1840 x 1000 rect, against an arc's 14 px
//     feather - so it needs far fewer columns than the arcs:
//
//         sigma = 0.3536 x the shortest lit segment's length x perimeter   (full-res px)
//         need  = 2 + 2 SEG_OVERHANG + 2 SEG_CORNER_OVERHANG + ceil(L / (sigma / SEG_COLUMNS_PER_SIGMA))
//         W     = clamp(need rounded up to WIDTH_STEP, SEG_MIN_WIDTH, the main table's width)
//
//     with L the longest band, as for the main table. Where W would be at
//     least SEG_DIRECT_SHARE of the main table's width (very short segments)
//     the separate table saves nothing, and the segments are baked into the
//     main table directly, as before it existed.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the segments
//     baked into the main table directly (plan step 3): 135 configs - rects
//     200 x 120 to 3600 x 2000, glowRadius 2 / 5 / 10, cornerRadius 0 / 40 /
//     a circle, scales 1.0 / 0.5 / 0.25, segments of length 0.01-0.2 with
//     boosts 0.5-2, overlapping, abutting, over the corners, travelling, and
//     under a partial arc - the frame after a light change, the segment table
//     forced on. With overhangs 16 / 8, every COLUMNS_PER_SIGMA from 3 to 8
//     read at most 2 levels with 99.9992% of lit pixels within 1; 2 read 3
//     levels on a thin glow at 3840 x 2160. 4 is one step of margin. Overhangs
//     8 / 4 held at every value tried (99.988%), 32 / 16 read at most 1 level
//     from 4 up but do not fit SEG_MIN_WIDTH. ---
#define GLOW_COVER_SEG_COLUMNS_PER_SIGMA 4.0

// --- How far the glow coverage bake integrates a segment's bell either side
//     of its centre, in units of 1 / invSigma (the bell is exp(-(d invSigma)^2),
//     so 3 is 4.24 standard deviations, where it has fallen to 1.2e-4 of its
//     peak). The bake (neon-glow-cover.frag, segmentsOnPiece) cuts the bell
//     here and NeonRenderer's GetGlowCoverDirtySegmentPieces mirrors it to
//     find the pieces a moved segment reaches: change one and the other
//     follows, or a piece the bell reaches is not re-baked. It was 5 / sqrt(2)
//     (5 sigma); at 3 a moved segment dirties fewer pieces, which took the
//     segment table's bake on a band of 8 segments from 0.75 to 0.58 (timer
//     units, AMD Radeon Pro 5300M), at most 1 level, on up to 1.6% of a
//     frame's lit pixels (0.15% on average over the cover set; I61 in
//     docs/review-findings.md). An 8-node rule on top halved it again but
//     read 3 levels off a long, bright segment, and was not taken. ---
#define GLOW_COVER_BELL_REACH            3.0
#define GLOW_COVER_SEG_OVERHANG          16
#define GLOW_COVER_SEG_CORNER_OVERHANG   8
#define GLOW_COVER_SEG_MIN_WIDTH         128
#define GLOW_COVER_SEG_DIRECT_SHARE      0.75

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- The output dither (R7), peak to peak in 8-bit levels: neonDither
//     (neon-grade.glsl) adds noise in +/- half this to every colour the neon
//     writes to the caller's framebuffer - the blit, the edge ring and the
//     ring's field composite, never an offscreen buffer. The halo and bloom
//     are slow gradients that cross a level only every few px, up to tens of
//     px in the dark tail, so an undithered write draws concentric contour
//     rings around the rect. 1.0 is the least that removes them; 0 turns the
//     dither off. It assumes an 8-bit target. ---
#define OUTPUT_DITHER_LSB         1.0

// --- Epsilons ---
#define WSUM_EPSILON              1e-6

// --- One-sided cut guard band, in BUFFER pixels. Scaled path only.
//
//     One of the two constants in this file stated in buffer px rather than
//     full-res px (BLIT_CUTOFF_GUARD_PX below is the other): it describes the
//     BUFFER's own sampling, so it must NOT be converted with uResolutionScale.
//
//     For pass 1b the one-sided cut is not applied by neon.frag at all -
//     neon-blit.frag applies it at DESTINATION resolution, where a pixel is a
//     pixel and the edge can land exactly where the full-resolution shading
//     puts it. What neon.frag still does is cull the dark side, and the bound
//     it culls at cannot be the cut itself: the blit reconstructs each
//     destination pixel from the 2x2 buffer texels around it, so a texel
//     killed at the cut leaves the first LIT destination pixel rebuilt partly
//     from black. Measured at scale 0.5, glowSide OUTSIDE: the first lit pixel
//     came back at 178 against the 239 full resolution puts there, a dark seam
//     hugging the inside of the glow's own edge.
//
//     So the cull runs this far PAST the cut, and the guard band's emission is
//     what the blit's filter reconstructs the boundary from. One buffer texel
//     is the filter's reach; 2.0 covers that with room for the diagonal, where
//     fwidth of an SDF is sqrt(2) rather than 1, and for the half-texel of
//     phase between the cut and the nearest texel centre.
//
//     The band costs a two-texel strip along the perimeter - nothing beside
//     the half-plane the cull still removes - and none of it is visible: the
//     blit's mask is zero everywhere past the cut, including at the hard step
//     where this bound finally discards.
//
//     NOT used by the edge ring, which owns its own cut (uBlitOwnsCut 0). See
//     neon.frag's sideCull and the post-grade cut block. ---
#define BLIT_SIDE_GUARD_PX        2.0

// --- Cutoff guard band, in BUFFER pixels. Pass 1b only.
//
//     BLIT_SIDE_GUARD_PX's argument, applied to the inside/outside cutoffs.
//     For pass 1b neon-blit.frag applies the cutoff masks at
//     DESTINATION resolution, and neon.frag only culls - this far past the end
//     of each ramp, so the blit's bilinear filter rebuilds every boundary from
//     lit texels rather than from black. Same value as the side guard, for the
//     same reasons: one buffer texel of filter reach, plus room for the
//     diagonal and for the phase between the boundary and the nearest texel
//     centre.
//
//     It replaces CUTOFF_SOFT_FLOOR_PX, a feather in buffer px that let a mask
//     drawn INTO the buffer place its boundary sub-texel. That was the best the
//     buffer could do, and it still softened the boundary across a buffer
//     texel and snapped it toward the buffer grid: on the bounded_band scene of
//     docs/neon-resolution-scale-comparison.html it read p99 41/255 against the
//     1.0 render at scale 0.5, and 69 at 0.25, and the band spread past its
//     own cutoffs as the scale fell (50,703 lit px at 0.5 and 53,568 at 0.25,
//     against 47,012 at 1.0). Drawn by the blit instead it reads p99 4 at 0.5
//     and 12 at 0.25, with 47,012 lit px at every scale; what is left is the
//     reduced-resolution glow inside the band, not its edges. See
//     docs/neon-resolution-scale-plan.md, step 2.
//
//     NOT used by the edge ring, whose masks and culls are its own. See
//     neon.frag's cutGuard and NeonRenderer::setupGeometry's cutGuardPx. ---
#define BLIT_CUTOFF_GUARD_PX      2.0

// --- Edge ring width. CPU only.
//
//     At every scale, NeonRenderer redraws a ring around the rect
//     edge at FULL resolution (neon.frag's ring program),
//     reading only the gather's result from the gather buffer. The ring reaches R
//     full-res px either side of the edge (GetRingWidth in neon-renderer.cpp):
//
//         R = max(reach(1.0), reach(scale)) + RING_GUARD_TEXELS / scale
//
//     where reach(s) is the filament's reach as neon.frag computes it at scale
//     s, in full-res px. BOTH, because inside R the reduced pass's filament -
//     floored to what its buffer can sample, so wider than the real one
//     whenever the line is thin - is replaced by the real one, and R has to
//     clear whichever reaches further. Uncapped: the reach is where a filament
//     has fallen to FILAMENT_CUTOFF of its gain, about 2/255 after the grade,
//     which is exactly how far its error can show. RING_GUARD_TEXELS adds the
//     reduced buffer's bilinear footprint past that.
//
//     Calibrated over lineWidth {1, 2, 4, 8, 16} x filamentFalloff
//     {0.5, 1, 2, 4} x glowRadius {0, 2, 5, 20}, each config rendered at
//     scales 0.75, 0.5 and 0.25 and compared against its own 1.0 render
//     (docs/neon-resolution-scale-plan.md, step 5). Worst error over all 80:
//
//       RING_GUARD_TEXELS    scale 0.75    scale 0.5    scale 0.25
//              0.5               3            12            3
//              1.0               4             2            2
//              2.0               2             2            2
//
//     1.0 is the smallest that holds 2/255 at 0.5 and 0.25, the targets. 2.0
//     also brings 0.75 within 2 (its worst at 1.0 is a 1 px flat-top line
//     with no glow) for a ring one buffer texel wider each side.
//
//     The two things that made the first version of this ring fail that sweep,
//     both now handled: R used to cover only the full-res filament (fixed by
//     taking the reduced pass's too, above), and at a small glowRadius pass 1's
//     quad stopped short of R, so the ring read gather texels nothing wrote
//     (fixed by covering the ring with the quad; since the gather was split
//     out, the gather pass's own quad, built in setupRingGeometry, covers
//     it). ---
#define RING_GUARD_TEXELS         1.0

// --- Gather resolution. CPU only.
//
//     At every resolutionScale the gather - ~95% of neon.frag's cost when it
//     ran inline - runs in a pass of its own (neon-gather.frag) at its own
//     scale, and pass 1 and the
//     edge ring shade from its result. Its scale is (GetGatherScale in
//     neon-renderer.cpp)
//
//         clamp(GATHER_TEXELS_PER_KERNEL / kc, GATHER_MIN_SCALE,
//               min(resolutionScale, GATHER_MAX_SCALE))
//
//     with kc the colour kernel's width in full-res px, perimeter *
//     COLOR_BLEND_PERIM_FRAC: the gather's outputs are Lorentzian-weighted
//     means whose kernel is never narrower than kc, so this many texels per kc
//     carries them through a bilinear read.
//
//     Calibrated on Mesa llvmpipe with an RGBA16F gather buffer (worst error
//     against 1.0 over `probe scenes`, and the default scene's frame time):
//
//       GATHER_TEXELS_PER_KERNEL   worst error   default 0.5   default 0.25
//                1.0                   4              -              -
//                1.5                   2           10.5 ms        8.0 ms
//                2.0                  1-2          10.8 ms        7.3 ms
//                4.0                   -           14-17 ms      11-12 ms
//         (gather at resolutionScale)  1-2          37.5 ms       12.3 ms
//
//     2.0 is the knee: 1.5 saves nothing measurable (the gather is no longer
//     the pass that costs) and 4.0 costs half as much again for no visible
//     gain. With an RGBA8 gather buffer - the fallback - 2.0 reads 3/255.
//     GATHER_MIN_SCALE barely matters - full-viewport rects
//     at 720p and 1080p read 1-2 at a floor of 0 and 0.0625 alike - and is
//     kept as a cheap guard: the gather buffer is clipped to the viewport, so
//     at the floor it is at most 128 x 80 texels at 1080p. See
//     docs/neon-resolution-scale-plan.md section 13. ---
#define GATHER_TEXELS_PER_KERNEL  2.0
#define GATHER_MIN_SCALE          0.0625

// --- The gather's ceiling. CPU only.
//
//     GetGatherScale never gathers finer than this, at any resolutionScale.
//     It binds only on a rect whose perimeter is under ~450 px (kc under 4
//     px), which would otherwise gather near full resolution: its gather
//     buffer - RGBA16F over the glow quad's box, two attachments with
//     segments - ran to 16 / 33 MB at 1080p for a 40 x 24 rect whose glow
//     fills the frame. At 0.5 that is a quarter, the same as the scaled path
//     at 0.5 always gathered at, and the check suite's smallest rect (perimeter
//     478 px) is not touched.
//
//     It replaces the split gate at 1.0 (FULL_RES_SPLIT_GATHER_MAX_SCALE /
//     FULL_RES_SPLIT_MIN_AREA_PX, removed - docs/neon-shader-cleanup-plan.md
//     step 3), which ran the gather INLINE in neon.frag where splitting it
//     out did not pay: on an AMD Radeon Pro 5300M, where the first offscreen
//     pass of a frame costs ~0.15 ms, for a still cutoff band under ~140k px
//     of quad and for a gather near full resolution. Since unchanged frames
//     stopped re-running the gather (I40) the still case no longer leaves
//     the caller's framebuffer at all; an Apple M2 Pro measured the split
//     1.1-4.75x faster at every size and mode, and the AMD, re-measured with
//     every rect split against its own gate, 0.98-1.02x in every animated
//     mode with one loss - a cutoff band under an intensity pulse, 0.84x.
//     So every scale splits, and the inline loop went with the gate. ---
#define GATHER_MAX_SCALE          0.5

// --- Stand-in distance for a cutoff that glowSide has already subsumed.
//
//     neon.frag hands this to the band distance on the side the one-sided cut
//     culls, so the discard and the mask there no-op through the arithmetic
//     exactly as a DISABLED cutoff does - same mechanism, same sentinel shape,
//     no extra branch downstream.
//
//     Matches CUTOFF_DISABLED_SIZE in neon-renderer.cpp, which is what a
//     disabled cutoff actually arrives as. It is a distance in the shader's own
//     px space and only has to be far enough that no realistic geometry reaches
//     it; see the note on that constant for the same reasoning. ---
#define CUTOFF_NEUTRALISED        1.0e6

// --- Glow reach (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity).
//
//     Named for the reach, not for an early-out: the per-fragment
//     `ad > earlyOut -> discard` this constant was originally calibrated for
//     no longer exists. Geometry culls the far region instead, which is
//     tiler-friendly, so what the factor sets is how far the glow is allowed to
//     reach before the quad stops covering it. The locals it feeds already say
//     so - `glowReach` in both setupGeometry implementations, `reach` in both
//     shaders.
//
//     glowRadius only: the companion sampleSpacing * SPACING_FACTOR term is
//     gone. sampleSpacing is perimeter / NEON_MAX_LOOP_SAMPLES, so it won on
//     any reasonably large rect at default glowRadius and made both the
//     distance the bloom got truncated at and the brightness it still had
//     there track the rect size. The bloom's reach is a function of glowRadius
//     alone, so that is all that sizes the quad.
//
//     Used by the renderers' setupGeometry AND by the shaders, which recompute
//     the same expression to place the bloom pedestal that lets this margin
//     stay tight without the truncation showing. Keep the two in step. ---
#define GLOW_REACH_RADIUS_FACTOR  48.0

// --- Where the shaders' quad-edge fade begins, as a FRACTION of the quad
//     margin. The emission ramps to zero over [FRAC * margin, margin], so the
//     bloom's 1/D tail - still ~10% of peak out at the quad edge - never clips
//     as a hard rectangle.
//
//     A fraction, not a pixel span, because the margin is proportional to
//     glowRadius (see GLOW_REACH_RADIUS_FACTOR) and so is the bloom profile it
//     hides. A fraction keeps the ramp at a constant proportion of the bloom's
//     reach, so the fade reads the same at every glow radius; a fixed px ramp
//     would vanish on a wide glow and dominate a narrow one. At the stock
//     glowRadius 5 / bloom 0.3 / intensity 1 the margin is 312 px, giving a
//     62 px ramp.
//
//     Nothing derives 0.8 - anything leaving a ramp wide enough to hide the
//     clip behaves the same. What it DOES assume is that the margin was set by
//     the glow, where 20% of it is a long distance. When outsideCutoff clamps
//     the margin instead (to where its fade ends, size + softness, plus 1),
//     20% of ~13 px is 2.6 px and
//     the ramp lands INSIDE the cutoff band, dimming the band's outer edge
//     ahead of the cutoff mask - and only on the exterior, since the fade keys
//     on positive d. Both shaders therefore floor the ramp's start at the
//     cutoff boundary whenever that boundary falls inside the quad; see the
//     fadeStart block in neon.frag.
//
//     Since V23 the bloom no longer relies on this fade to end smoothly - each
//     piece fades by its own distance from BLOOM_FADE_START_FRAC, below, which
//     has it under a level well before this ramp starts. ---
#define QUAD_FADE_START_FRAC      0.8

// --- Where each piece's bloom starts fading out, as a FRACTION of `reach`
//     (neon.frag's bloom end fade, V23).
//
//     The pedestal takes the bloom's 1/a tail to exactly 0 at `reach`, but
//     with its slope still on, so the glow stopped on a crease: inside a large
//     rect, a hard-edged dark rectangle `reach` in. The quad-edge fade above
//     hid it outside, but over [0.8, 1.0] of the margin, where the tail is
//     still ~5 levels up, and a fade that late STEEPENS the tail before it
//     flattens it - a shoulder, then flat, which reads as an edge too. So
//     each piece's bloom is multiplied by 1 - smoothstep(FRAC * reach, reach,
//     a) of its own distance, on both sides.
//
//     0.5 is about the latest start that does not steepen it: the steepest
//     slope inside the fade over the slope where it starts, on the
//     infinite-line bloom at glowRadius 5, bloomStrength 0.52, intensity 0.8,
//     is 1.40 at 0.8, 1.14 at 0.6, 1.035 at 0.5 and 1.00 from 0.4 down. (It
//     is also where c * (reach - a)^2 meets the pedestalled 1/a tail in value
//     and slope.) An earlier start costs more of the tail; at 0.5 the glow
//     reads up to 6 levels dimmer in the outer half of its reach. ---
#define BLOOM_FADE_START_FRAC     0.5

// --- Filament reach for the same quad sizing, expressed in sigmas.
//
//     glowRadius = 0 means "filament only" - the halo and bloom are gated off
//     by GLOW_GATE_FADE_PX - but the filament itself is still there, sized by
//     lineWidth, so the quad has to cover it or the exterior gets clipped.
//
//     The reach is NOT a constant, because the profile is
//     exp2(-(ad/sigma)^N) with N = 2 * filamentFalloff and the slider runs
//     down to falloff 0. Solving core * FILAMENT_GAIN < FILAMENT_CUTOFF gives
//
//         reach = sigma * log2(FILAMENT_GAIN / FILAMENT_CUTOFF) ^ (1/N)
//
//     which is 1.9 sigmas at falloff 2.0, 3.5 at 1.0, 12.6 at 0.5, 67.8 at 0.3
//     and 558 at 0.2 - it diverges as falloff -> 0. A fixed 12 (calibrated for
//     falloff 0.5) left the low end badly under-sized: the interior is
//     unbounded because the quad always covers it, so a soft filament flooded
//     inward while the exterior was chopped at 12 sigmas. That asymmetry is
//     the "no outer glow at glowRadius 0" report.
//
//     MAX_SIGMAS bounds the fill cost where the formula diverges; the shaders
//     pedestal-subtract the core at exactly this reach, so hitting the cap
//     shortens the glow symmetrically on BOTH sides instead of showing a cliff
//     on the outside. MIN_SIGMAS keeps a little room at very high falloff.
//     Keep the expression in step across neon-tuning.h, both setupGeometry
//     implementations, and both fragment shaders. ---
#define FILAMENT_CUTOFF           0.002
#define FILAMENT_REACH_MIN_SIGMAS 2.0
#define FILAMENT_REACH_MAX_SIGMAS 64.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_

precision highp float;

// ---------------------------------------------------------------------------
// Perimeter emission pre-pass.
//
// Bakes the FRAGMENT-INVARIANT half of the neon gather loop into a small
// texture: NEON_MAX_LOOP_SAMPLES wide, 2 tall.
//
// Why this pass exists
// --------------------
// The gather loop in neon.frag runs, for every sample
// AND every screen fragment: the arc winner-take-all search (uArcCount x
// arcInside, four smoothsteps each), the travelling-segment loop
// (uSegmentCount x exp()), and one to two filtered LUT fetches. None of that
// depends on the fragment - the sample's perimeter position si is i / N, and
// every colour / mask term is a pure function of (si, uTime, config). So a
// full-viewport draw was recomputing the identical N-entry table once per
// fragment.
//
// Here it is computed once per frame in 2N fragments, and the consumer's
// per-sample cost collapses to two texelFetches plus the distance maths that
// genuinely does vary per fragment. Per-fragment cost stops scaling with the
// number of arcs and segments.
//
// Why TWO rows and not one
// ------------------------
// The consumer keeps two independently normalised accumulators:
//
//     col       = SUM(baseColI * arcW * g) / SUM(arcW * g)      <- arc hue
//     segColHue = SUM(segColour * bell * g) / SUM(bell * g)     <- segment hue
//
// That GATED normalisation is deliberate (it makes each hue a unit-magnitude
// value carrying neither coverage nor intensity - see neon.frag), but it means
// the two colour terms ride DIFFERENT weights: arcW * g for the arc, bell * g
// for the segments. They therefore cannot be folded into a single
// premultiplied colour the way they could under one shared weight - each needs
// its own numerator AND its own denominator, which is 8 floats:
//
//   row 0 (y = 0):  .rgb = baseColI * arcW      .a = arcW
//   row 1 (y = 1):  .rgb = SUM(segColour*bell)  .a = SUM(bell)
//
// Downstream, `acc += row0.rgb * g` and `wsumLit += row0.a * g` reproduce the
// old arc accumulation exactly, and likewise row 1 for the segments.
//
// uIntensity is deliberately NOT folded in here (the reference implementation
// on improve_neon_by_emission_pre_pass did fold it). It cancels out of the
// gated normalisation anyway, and reaches the emission through emitCover /
// filamentGate, which are pointwise and size-invariant.
//
// Both rows can exceed 1.0 - SegmentBoost::boost is an absolute peak
// brightness and several segments can stack, and Arc::intensity is unbounded -
// so the target wants a float format. The renderers ask for RGBA16F and fall
// back to RGBA8 with a warning where the driver refuses it.
//
// What is NOT baked here
// ----------------------
// Anything that reads the FRAGMENT's own perimeter position: the continuous
// arc / segment coverages, the filament gate, and the colour-stop alpha. Those
// are read at sPos in the main shader, not at the gather samples, and baking
// them at sample resolution would reintroduce the quantisation their pointwise
// evaluation exists to avoid.
// ---------------------------------------------------------------------------

out vec4 fragColor;

uniform float uTime;
uniform float uHueRotationRate;
uniform int   uNumSamples; ///< Samples in use; 1..NEON_MAX_LOOP_SAMPLES. Texels past it are written but never read.

// Same three LUT atlases the gather used to sample, bound to the same units.
// Reading them here rather than re-deriving the colours on the CPU is what
// makes the result exact by construction: a CPU bake would have to reproduce
// bilinear REPEAT filtering and could drift.
uniform sampler2D uGradientLUT;
uniform sampler2D uSegmentLUT;
uniform sampler2D uArcLUT;

layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// Verbatim from the gather it replaces - the feather widths buy hue-blend
// behaviour, so changing them here changes the ring's colour hand-over. Keep
// in step with the copy in neon.frag.
float arcInside(float si, float start, float length, float invNumSamples) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    float fHead = invNumSamples;
    float fTail = 0.25 * invNumSamples;
    float end = start + length;
    float g1a = smoothstep(start - fTail, start, si);
    float g2a = 1.0 - smoothstep(end, end + fHead, si);
    float g1b = smoothstep(start - fTail, start, si + 1.0);
    float g2b = 1.0 - smoothstep(end, end + fHead, si + 1.0);
    return max(g1a * g2a, g1b * g2b);
}

void main() {
    // gl_FragCoord.x is the sample index, .y picks the row. si is computed
    // directly rather than accumulated as the old loop's `si += dti` chain
    // did, so it carries no drift across N iterations.
    float invNumSamples = 1.0 / float(max(uNumSamples, 1));
    float si = floor(gl_FragCoord.x) * invNumSamples;
    float ti = si - uTime * uHueRotationRate;

    // --- Arc winner-take-all, moved verbatim from the gather ---------------
    float bestMask = 0.0;
    int   bestIdx  = -1;
    for (int a = 0; a < uArcCount; a++) {
        vec4  arc  = uArcs[a];
        float mask = arcInside(si, arc.x, arc.y, invNumSamples) * arc.z;
        if (mask > bestMask) {
            bestMask = mask;
            bestIdx  = a;
        }
    }
    float arcW = bestMask;

    // Winner's colour: arc-local for hasStops, perimeter space otherwise.
    // segFallback is what a stop-less segment inherits - the arc's colour
    // where an arc covers, the base gradient where none does.
    vec3 baseColI;
    vec3 segFallback;
    if (bestIdx >= 0) {
        vec4 winner = uArcs[bestIdx];
        // .w is a bitmask (bit 0 = hasStops, bits 1-2 = abutment, used only by
        // the consumer's coverage feather) - test bit 0, not the whole value.
        if (mod(winner.w, 2.0) >= 0.5) {
            // No hue-rotation term: uArc is the arc's own head-to-tail
            // coordinate, not a perimeter position. The consumer's alpha read
            // in neon.frag must agree exactly or colour and alpha come from
            // different texels of the same row. See neon.frag for why the term
            // was wrong here in the first place.
            //
            // WRAPPED, and by the same expression the consumer uses for its
            // alpha read. arcInside above already covers a seam-straddling arc
            // (it tests si and si + 1.0), so an unwrapped si - start went
            // negative past the seam and CLAMP_TO_EDGE pinned the wrapped
            // remainder to the head colour while the arc stayed lit.
            float rowY = (float(bestIdx) + 0.5) / float(MAX_ARCS);
            float rel  = si - winner.x;
            rel       -= floor(rel);                          // wrap to [0, 1)
            if (rel > 0.5 * (1.0 + winner.y)) { rel -= 1.0; } // behind the start, not past the head
            float uArc = rel / max(winner.y, 1e-4);
            baseColI   = texture(uArcLUT, vec2(uArc, rowY)).rgb;
        } else {
            baseColI = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
        }
        segFallback = baseColI;
    } else {
        baseColI    = vec3(0.0);
        segFallback = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
    }

    // --- Row 0: the arc term, premultiplied by its own gather weight -------
    if (gl_FragCoord.y < 1.0) {
        fragColor = vec4(baseColI * arcW, arcW);
        return;
    }

    // --- Row 1: the segment term -------------------------------------------
    // Summed over segments here so the consumer sees one vec4 per sample
    // regardless of uSegmentCount - that sum is the whole point of the pass.
    vec3  segSum  = vec3(0.0);
    float bellSum = 0.0;
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg  = uSegments[s];
        float rel  = si - seg.x;
        rel       -= floor(rel + 0.5);              // wrap to [-0.5, 0.5]
        float e    = rel * seg.y;                   // normalise by invSigma
        float bell = seg.z * exp(-e * e);           // boost * gaussian
        if (bell < 0.005) continue;                 // same early-out as the old loop

        vec3 segColor;
        if (seg.w > 0.5) {
            float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
            float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
            segColor     = texture(uSegmentLUT, vec2(tLocal, rowY)).rgb;
        } else {
            segColor = segFallback;
        }
        segSum  += segColor * bell;
        bellSum += bell;
    }
    fragColor = vec4(segSum, bellSum);
}

)shadersrc";

                static const char *const NEON_GLOW_COVER_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - the neon shaders (neon.frag, neon-emission.frag), where CMake
//     text-injects this file via @NEON_TUNING@ in shaders.h.in, and
//   - the C++ renderer (neon-renderer.cpp), which #includes it for the
//     glow-reach quad-sizing factors.
//
// The px constants below are written in FULL-RESOLUTION pixels. Where the
// renderer draws at a reduced NeonConfig::resolutionScale, the shader converts
// each one with uResolutionScale at the point of use; the notes on the
// individual constants say which need it and which do not.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a `const float X = 0.9f;` cannot
// be written once and stay valid in both languages. Plain #define is the only
// form that compiles identically as GLSL and as C++ from a single file.
// (For C++-only constants, prefer constexpr - macros are used here strictly
// because these values must also live inside the shaders.)
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
//
//     FILAMENT_MIN_HALF_WIDTH is a STATED-width floor: it is what makes
//     lineWidth 0 mean "no line" rather than a single bright dot, and it is
//     paired with lineGate, which fades the filament in over the same span. It
//     describes the width the caller asked for, so it is a FULL-RES px constant
//     and the shader converts it with uResolutionScale like every other one.
//
//     The SAMPLING floor is the pair below, and the distinction from the
//     stated floor is the whole point of having two of them. Below
//     resolutionScale 1.0 the gather rasterises into a reduced buffer and
//     neon-blit.frag bilinearly upsamples it, so a filament the buffer cannot
//     sample is not reconstructed - it is resampled, and what comes out
//     depends on where the rect edge happens to fall between buffer texel
//     centres. That is a property of the BUFFER, so these are in BUFFER px and
//     are NOT converted with uResolutionScale.
//
//     Getting that backwards is what made a 1 px line at scale 0.5 disagree
//     with the same line at 1.0. sigma was floored only by the converted
//     stated constant, which at scale 0.5 is 0.25 buffer px, so the line sat
//     under the buffer's Nyquist limit and its peak swung with the rect's
//     sub-pixel position - 217 on a texel centre, 180 half a texel off,
//     against a 1.0 reference that holds 230-241 at every position.
//
//     THE FLOOR IS NOT A FIXED HALF WIDTH. It was one - a flat 0.5 buffer px -
//     and a fixed half width asks the wrong question. What survives the blit
//     is not decided by the profile's width at half maximum; it is decided by
//     how much signal the NEIGHBOURING buffer texel still carries, because
//     that is what the bilinear filter rebuilds the peak from. So the floor
//     says exactly that: the profile must still be at
//     FILAMENT_NYQUIST_MIN_SHARE of its peak, FILAMENT_NYQUIST_SAMPLE_PX out.
//     Inverting the generalized Gaussian for sigma gives
//
//         sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
//
//     which neon.frag and NeonRenderer::setupGeometry both evaluate.
//
//     0.0625 and 1.0 are today's behaviour restated, not a retune: at the
//     DEFAULT falloff (N = 2) the expression is exactly 0.5 buffer px, the
//     constant it replaces, so nothing at or near the default moves.
//
//     The two questions diverge at a SOFT falloff, and that is what the change
//     is for. sigma does not only set the core - it multiplies reachSigmas, so
//     it sets the whole tail. At filamentFalloff 0.27 (N = 0.54) reachSigmas
//     clamps at FILAMENT_REACH_MAX_SIGMAS and the tail is 64 sigmas, so a flat
//     0.5 floor stretched a 31 px filament to 61 px at scale 0.5 and past
//     120 px at 0.25 - to buy 5 levels of peak accuracy (max per-phase error
//     13 -> 8) on a profile 31 px wide that the buffer was sampling perfectly
//     well. The share form asks for 0.082 buffer px there, below what the
//     caller already supplies, so it does not engage.
//
//     Where it DOES still engage it is doing the work it was added for: at the
//     default falloff, without any floor, the peak collapses to 153 against a
//     235 reference, and at filamentFalloff 1.5 to 24. The known cost of the
//     share form is the band around filamentFalloff 0.4 to 0.5, where it stops
//     engaging while the unfloored peak error is still 12 to 18 levels;
//     tightening MIN_SHARE to cover that would stop the default case being
//     bit-identical, so the crossover sits here deliberately. See
//     docs/corner-crease-and-filament-nyquist.md section 2.8.
//
//     The line does read WIDER at a reduced scale wherever the floor engages,
//     and that part is not a defect to tune away: a line the buffer cannot
//     sample cannot be reconstructed from it. The floor buys a stable,
//     correctly-bright line of the narrowest width the buffer can actually
//     carry. Deliberately NOT paired with an amplitude compensation to
//     conserve the line's integral - the grade at the end of neon.frag
//     tonemaps per-fragment INSIDE the buffer and FILAMENT_GAIN puts the core
//     deep in saturation, so scaling the linear amplitude barely moves the
//     8-bit value while it does measurably dim the peak (error 15-23/255
//     against 0-10 without it). See
//     docs/corner-crease-and-filament-nyquist.md section 2.
//
//     GATED to scales below 1.0 in both consumers. At scale 1.0 the reduced
//     buffer is full size and the blit copies it 1:1, so there is nothing to
//     survive, and the ring always shades at 1.0. The old flat constant was a
//     no-op at 1.0 by arithmetic
//     coincidence - it equalled the converted stated floor; this expression
//     would not be, above N = 2, so the gate is explicit now. ---
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_NYQUIST_SAMPLE_PX 1.0
#define FILAMENT_NYQUIST_MIN_SHARE 0.0625
#define FILAMENT_GAIN             12.0

// --- Continuous-arc filament gate feathers (neon.frag).
//     INWARD FEATHER: the smooth ramp sits INSIDE the arc's own perimeter
//     span, so nothing outside the arc gets lit -> no corner bleed regardless
//     of width, no perpendicular spike, and the profile is a plain smoothstep
//     that reads the same on straight edges and at corners. Trade-off: the
//     visible arc appears inset by these widths (arc lights up at
//     start + TAIL_FEATHER_PX and ends at start + length - HEAD_FEATHER_PX).
//     Values are pixel-space spans, divided by the current perimeter at the
//     call site, and capped per-arc by ARC_FEATHER_MAX_SHARE below. ---
#define HEAD_FEATHER_PX           14.0
#define TAIL_FEATHER_PX           14.0

// --- Cap on each feather, as a share of the arc's own length.
//
//     The feathers above are a fixed pixel span while an arc's length is a
//     FRACTION of the perimeter, so the same arc config is a different pixel
//     length on every rect. Below ~(HEAD + TAIL) px the two ramps overlapped
//     and ate into the arc's peak: an L = 0.02 arc peaked at 1.00 on an
//     800x600 rect but only 0.21 on a 200x150 one - the last place rect size
//     still reached brightness. Capping each feather at a share of the arc
//     length holds the peak at exactly 1.0 for any length at any size; a short
//     arc gets a proportionally shorter ramp instead of a truncated top.
//
//     0.4 leaves a 0.2 * length plateau at full brightness between the two
//     ramps. Up to 0.5 also holds the peak, but with no plateau the arc reads
//     as a spike rather than a flat-topped segment. Arcs longer than
//     (HEAD + TAIL) / 0.4 px never reach the cap, so the normal case is
//     bit-identical to before. ---
#define ARC_FEATHER_MAX_SHARE     0.4

// --- Halo (sharp coloured glow).
//
//     The halo and bloom are evaluated ANALYTICALLY from the rounded-box SDF
//     distance, not summed over the perimeter gather. For a locally straight
//     emitter of unit density the old sums converge exactly to:
//
//       sum g*sqrt(g) * spacing*kh^2*HALO_NORM  -> HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
//       sum 1/(dd+bw^2) * spacing*bw*BLOOM_NORM -> BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
//
//     so the NORM factors keep their meaning and their calibration: peak
//     values at ad = 0 are unchanged (0.86 and 1.005 respectively).
//
//     The closed form is what makes the effect geometry-independent. The
//     gather had to floor its kernel at a multiple of sampleSpacing to stop
//     128 discrete samples beading into dots, and sampleSpacing is
//     perimeter / NEON_MAX_LOOP_SAMPLES - so halo width, and via the gate its
//     brightness, both tracked the rect size, and glowRadius did nothing at
//     all until it exceeded the floor (~56 px on a 1920x1080 rect, i.e. most
//     of its usable range). An analytic profile cannot bead at any radius, so
//     no floor is needed and glowRadius sets the width directly.
//
//     Those two limits are of an INFINITE emitter, and evaluating one of them
//     at ad = the nearest-edge distance is what used to produce the interior
//     medial-axis creases - the dark wedges running in from each corner, plus
//     the central spine. A fragment on a corner diagonal has two edges equally
//     near and was lit by exactly one of them: measured against a fragment with
//     a single edge at the same distance, the ratio was 1.000 at every distance
//     tested, where physics says roughly 2.
//
//     neon.frag now sums the FINITE-SEGMENT form of the same two integrals over
//     the four straight edges instead (haloSegment / bloomSegment). Integrating
//     the same kernels from t1 to t2 along a segment at perpendicular distance
//     a gives elementary antiderivatives, and as t1 -> -inf, t2 -> +inf each one
//     reduces to exactly the limit above - so this is a strict generalisation
//     and the NORM factors keep the calibration they already had. The corner
//     sweep went from a V kinked at exactly 45 degrees (21 levels deep at
//     r = 160) to a smooth basin of 12, which is genuine falloff rather than a
//     crease. Cost is ~1.06-1.10x of the neon frame; the gather loop is still
//     the overwhelming majority of it.
//
//     The CREASE is what the four-segment form was adopted for. It is not the
//     largest thing it moves. The old expressions were functions of ad and of
//     nothing else, so the old profile was SYMMETRIC about the line - a
//     fragment 120 px inside the edge and one 120 px outside were lit
//     identically, by construction. Outside, the emitter recedes; inside, it
//     wraps around you. The segment sum knows the difference, so the exterior
//     now falls off slightly faster (mid-edge peak itself moves by 2/255) and
//     the INTERIOR stops falling off past roughly one rect-half and settles on
//     a floor. On the 1000x500 / glowRadius 60 probe the centre goes 162 ->
//     186 and the interior mean 174.5 -> 188.8; at glowRadius 30 on 800x400 it
//     is 81 -> 113. The glow reads as bigger, and the part that grew is the
//     interior - the exterior mean goes slightly DOWN.
//
//     Intended, and not tunable back out: a real rectangular tube does light
//     its own interior, and HALO_GAIN cannot be lowered to undo it without
//     dimming the line with it. The knobs for a perimeter-hugging glow are the
//     ones that always meant that - insideCutoff, and glowSide OUTSIDE.
//
//     THE CORNERS ARE A FIFTH THROUGH EIGHTH SEGMENT, not an extension of the
//     four. The straights are trimmed to their TANGENT POINTS, and each quarter
//     arc contributes one more segment: the arc DEVELOPED onto its own tangent
//     at whichever arc point is nearest the fragment, carrying the arc's full
//     length and split about that point, so it abuts the straights exactly in
//     arclength and the emitter is continuous. See neon.frag's
//     arcTangentSegment.
//
//     DEVELOPED AT RATE sqrt(rho * min(rho, r)), NOT AT RATE r, with the
//     measure put back by scaling the segment by r/rate - `rho` being the
//     fragment's distance from the arc centre. Laying an arc out one unit of
//     tangent per unit of arc is only right for a fragment ON it; the exact
//     distance to the point dphi away is a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,
//     whose slope at the foot is sqrt(rho*r). Rate r cost two things:
//
//       - at the CENTRE OF CURVATURE the whole arc ran off to one side of the
//         foot, and both kernels peak at t = 0, so it under-counted - to 54%
//         of the true value on a circle, where all four centres coincide at
//         the middle of the shape;
//       - and it CREASED, because the arc's endpoints slide at the tangent
//         rate on one side of the clamp and at rate 1 on the other, which
//         agree only where the rate is length(w). Every other point of the two
//         lines through an arc centre carried a C1 kink: an L-shaped seam per
//         corner, and on a circle an unmistakable dark cross. See
//         docs/review-findings.md V12.
//
//     rho == r gives rate r and weight 1, so a fragment on the tube is
//     bit-identical to the rate-r form and this calibration is untouched. The
//     whole emitter's worst error against a numerically integrated perimeter
//     falls on every geometry tested (16.5 -> 9.8% on a 600x400 r=40,
//     37.3 -> 26.3% on a circle).
//
//     The straights used to run to the SHARP corner instead, on the grounds
//     that over-extending them past the tangent point stood in for the arc that
//     has no elementary closed form. It does not stand in for it. The
//     over-extension is a phantom emitter a few px from a fragment that is tens
//     of px from the real tube: on an 800x400 rect at cornerRadius 40, a point
//     20 px outside the arc on the diagonal sits 2.4 px from EACH of the two
//     phantoms, and rendered 98 -> 146 across the commit that introduced them.
//     Against a numerically integrated rounded-rect perimeter it was 40 levels
//     too dark ON the tube and 50 too bright just outside it, which is what
//     made a rounded corner read square at a narrow glowRadius.
//
//     The developed arc costs one extra segment per corner - one atan and a
//     length on top - and takes the worst error over a quadrant from 76 to 6
//     levels at glowRadius 5, 112 to 6 at cornerRadius 120, 130 to 8 at 200.
//     A full circle (cornerRadius == halfMin) is still the residual case,
//     since its perimeter is four developed arcs and no straights at all.
//
//     Gated on uCornerRadius > 0, which is a UNIFORM - safe branching, and the
//     reason a sharp rect pays nothing and stays bit-identical (nine scenes
//     cmp-equal, not argued, and still cmp-equal after the rate fix above).
//     Rounded costs 1.19x of the neon pass against no arcs at all, and the rate
//     fix adds 1.03x on top - but the sharp path pays 1.026x of THAT for code
//     it never runs, because what decides this block is REGISTER PRESSURE, not
//     structure: with a per-arc bloom pedestal it was heavy enough to cost the
//     sharp path 1.14x. Re-time a cornerRadius 0 scene as well as a rounded one
//     after touching this. See docs/corner-crease-and-filament-nyquist.md
//     sections 1.7 to 1.9, and docs/review-findings.md V4, V10 and V12. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Width of the COLOUR gather kernel, as a FRACTION OF THE PERIMETER.
//
//     The perimeter colour blend is the one part of the shader that is still a
//     discrete sum over the loop samples, and it is the only place a length has
//     to be expressed this way rather than in pixels. The reason is that the
//     signal it filters - the gradient LUT - is itself parameterised by
//     perimeter fraction: sample i contributes LUT(i / NEON_MAX_LOOP_SAMPLES).
//     A kernel measured in pixels therefore covers a DIFFERENT span of the
//     gradient on every geometry, and the same colour stops render washed out
//     on a small rect and crisp on a large one. Measured on the stock 4-stop
//     ring, the colour sampled exactly on the red stop ran (0.95, 0.33, 0.06)
//     at 200x150 against (0.96, 0.04, 0.00) at 2800x2200 - roughly 8x the hue
//     bleed from geometry alone. Scaling the kernel with the perimeter is what
//     makes the gradient read identically at any size.
//
//     Two further properties fall out of the same choice:
//
//       - A constant anti-bead margin. The kernel has to stay >= about one
//         sample spacing, or it collapses between samples and the blend beads
//         into dots. Spacing is perimeter / NEON_MAX_LOOP_SAMPLES, so a
//         perimeter fraction pins the ratio at
//         COLOR_BLEND_PERIM_FRAC * NEON_MAX_LOOP_SAMPLES = 1.13 spacings on
//         every geometry, where the previous fixed 24 px span met the bound
//         only up to a ~3000 px perimeter and was down to 0.31 spacings by
//         9900 px. (No beading was actually measurable there - a DFT of the
//         hue around the perimeter put the 128-cycle component at the noise
//         floor - because the gradient varies slowly over one spacing on a
//         rect that large. The margin is a guarantee for dense colour stops,
//         not a fix for an observed artifact.)
//
//       - Sample-count independence. The kernel comes from the
//         NEON_MAX_LOOP_SAMPLES-based fraction rather than from the runtime
//         sample count, so the numSamples knob does not move the colour
//         blend. (An earlier sampleSpacing-derived floor divided by the live
//         count and landed 2x wider at reduced counts.)
//
//     Deliberately NOT coupled to glowRadius. The gather produces colour only -
//     the halo and bloom have been closed-form since they stopped riding on it -
//     so a wide glow has no reason to desaturate the gradient, and the old
//     max(glowRadius, floor) reintroduced exactly the pixel-space dependence
//     this constant exists to remove.
//
//     0.0088 puts the kernel at 24.0 px on the stock 800x600 / radius 40
//     geometry, matching the previous fixed span, so default-sized output is
//     unchanged.
//
//     NOTE: unit-free, unlike the px constants around it. The shader
//     multiplies it by a perimeter that is already in scaled px, so it needs
//     no uResolutionScale correction - applying one would double-apply. ---
#define COLOR_BLEND_PERIM_FRAC    0.0088

// --- Emission on/off ramp. glowRadius = 0 must read as "filament only", but
//     an analytic profile at radius 0 is a sub-pixel spike of full height
//     rather than nothing, so the halo and bloom fade in over
//     glowRadius = [0, this]. A FIXED pixel width: gating against the
//     sampleSpacing-derived floor instead would re-couple brightness to the
//     rect size, which is the whole thing this design removes.
//
//     NOTE: full-res pixel span, compared against a uGlowRadius that arrives
//     already scaled, so the shader multiplies this by uResolutionScale at
//     the point of use. Identity at scale 1.0. ---
#define GLOW_GATE_FADE_PX         2.0

// --- Lower bound on the analytic emission widths. Guards the divides only;
//     anything this small is already multiplied out by GLOW_GATE_FADE_PX. ---
#define EMISSION_MIN_WIDTH        1e-3

// --- Degenerate guard for the corner arc's tangent frame (neon.frag,
//     arcTangentSegment). The frame is built by normalising the fragment's
//     offset from the arc centre, clamped into the quarter the arc occupies;
//     that offset is the zero vector only for a fragment sitting exactly on an
//     arc centre, where the direction is undefined and atan(0, 0) is undefined
//     with it. Below this the frame falls back to the nearer of the two tangent
//     points, which is the correct clamp for the whole region the guard covers.
//
//     It floors `rho` in the same function, for the same fragment: the
//     development rate divides into the segment's weight, so an exactly-zero
//     rho would be a division by zero. The floored value is nowhere near a
//     pixel and the limit it lands on is the exact answer anyway, so this only
//     keeps a NaN out.
//
//     Unit-free: compared against a length in the shader's own px space, and
//     small enough that nothing but the exact degeneracy reaches it. ---
#define ARC_FRAME_EPSILON         1e-6

// --- Bloom (wide background spill). See the halo note above for the closed
//     form; BLOOM_SPACING_FLOOR is gone with the gather. ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_NORM_FACTOR         0.32

// --- Travelling-segment array size (shared by C++ vector cap + GLSL uniform array) ---
#define MAX_SEGMENT_BOOSTS        8

// --- Arc range array size (shared by C++ vector cap + GLSL uniform array).
//     Each arc gates a slice of the perimeter with its own colour + intensity.
//     Overlap resolves winner-take-all (max mask*intensity wins). ---
#define MAX_ARCS                  8

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO and
//     the shader's array. The gather iterates only uNumSamples of them
//     (NeonConfig::numSamples, which defaults to this), so it is a ceiling,
//     not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Glow coverage table (neon-glow-cover.frag, V20 and V21): for each piece
//     of the emitter and each fragment position round it, the arcs' and the
//     segments' coverage of that piece as the halo and the bloom see it. Laid
//     out in neon-pieces.glsl: four bands of ROWS rows, each holding one
//     straight at its left and one corner at its right. A band's columns are
//     shared between the two in proportion to their lengths (uGlowCoverSplit,
//     NeonRenderer::GetGlowCoverSplit), so a circle's corners get the columns
//     its straights do not need and a long rect's straights get its corners':
//     the straight has OVERHANG columns past each end, the corner
//     CORNER_OVERHANG on each side of its arc and a guard texel at each end of
//     its block, and the columns left over (the layout's `shared`) are split
//     between the two interiors, each keeping at least MIN_INTERIOR. ROWS 32 is half of them
//     within one halo width of a straight, and 16 inside and 16 outside a
//     corner's arc. 1024 x 128 RGBA16F is 1.0 MB - half V20's single table per
//     perimeter. Sizing it, against an exact per-piece reference (V21 in
//     docs/review-findings.md): 48 rows (1.5 MB) read at most 2 levels off where
//     32 reads 3 on a few pixels of a segment scene, for 1.3x the bake's cost
//     under animation; 768 columns read 6 levels off a 1 px halo on a 4K-sized
//     rect.
//
//     The WIDTH is not a constant: the table's layout is a parameter
//     (NeonRenderer's GlowCoverLayout, the shaders' uGlowCoverLayout), between
//     MIN_WIDTH and MAX_WIDTH in steps of WIDTH_STEP. ROWS and HEIGHT are. ---
#define GLOW_COVER_MAX_WIDTH      1024
#define GLOW_COVER_MIN_WIDTH      256
#define GLOW_COVER_WIDTH_STEP     64
#define GLOW_COVER_ROWS           32
#define GLOW_COVER_OVERHANG       64
#define GLOW_COVER_CORNER_OVERHANG 32
#define GLOW_COVER_MIN_INTERIOR   16
#define GLOW_COVER_HEIGHT         (4 * GLOW_COVER_ROWS)

// --- The glow coverage table's width (NeonRenderer's GetGlowCoverWidth):
//     enough interior columns that the longest band - its straight plus its
//     quarter arc, L full-res px - gets one every S px, S growing with the
//     halo width kh (max(glowRadius, EMISSION_MIN_WIDTH), px):
//
//         S    = clamp(PX_PER_COLUMN_PER_KH * kh, MIN_PX_PER_COLUMN, MAX_PX_PER_COLUMN)
//         need = 2 + 2 OVERHANG + 2 CORNER_OVERHANG + ceil(L / S)
//         W    = clamp(need rounded up to WIDTH_STEP, MIN_WIDTH, MAX_WIDTH)
//
//     In full-res px because the edge ring reads the table at full
//     resolution; lengths enter the layout only as ratios, so every scale
//     shares one table.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the 1024-wide
//     table (docs/neon-glow-cover-resolution-plan.md step 2): 250 configs -
//     rects 200 x 120 to 3600 x 2000, glowRadius 1 / 2 / 5 / 10 / 20,
//     cornerRadius 0 / 40 / a circle, both windings, three partial arcs with
//     and without colour stops and with two segments, at scales 1.0 / 0.5 /
//     0.25 - each at every width from 256 to 960, the frame after a light
//     change, against the same frame at 1024. The widest spacing that still
//     met the plan's criterion (max 2 levels, 99.9% of lit pixels within 1)
//     on every config was ~2.2 px per column at kh 1, 2.5 at 2, 4.6 at 5, 6.8
//     at 10 and 10 at 20. These constants sit under that envelope - 12% at
//     kh 5, 27% or more elsewhere - and at the widths they give, every config
//     read at most 2 levels with at least 99.988% of lit pixels within 1.
//     Spacing 20% wider failed at kh 5 (4 levels on a 200 px circle). A
//     narrow glow on a large rect still gets MAX_WIDTH: at kh 1-2 a
//     3600 x 2000 rect needed all 1024 columns. ---
#define GLOW_COVER_PX_PER_COLUMN_PER_KH 0.8
#define GLOW_COVER_MIN_PX_PER_COLUMN    1.6
#define GLOW_COVER_MAX_PX_PER_COLUMN    5.0

// --- The segment table (NeonRenderer's GetGlowCoverSegmentWidth,
//     neon-glow-cover-fill.frag): the segments' two channels of the glow
//     coverage table, baked at a width of their own and copied in. A bell's
//     standard deviation is 0.3536 x its length x the perimeter - ~100 px for
//     a length-0.05 segment on a 1840 x 1000 rect, against an arc's 14 px
//     feather - so it needs far fewer columns than the arcs:
//
//         sigma = 0.3536 x the shortest lit segment's length x perimeter   (full-res px)
//         need  = 2 + 2 SEG_OVERHANG + 2 SEG_CORNER_OVERHANG + ceil(L / (sigma / SEG_COLUMNS_PER_SIGMA))
//         W     = clamp(need rounded up to WIDTH_STEP, SEG_MIN_WIDTH, the main table's width)
//
//     with L the longest band, as for the main table. Where W would be at
//     least SEG_DIRECT_SHARE of the main table's width (very short segments)
//     the separate table saves nothing, and the segments are baked into the
//     main table directly, as before it existed.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the segments
//     baked into the main table directly (plan step 3): 135 configs - rects
//     200 x 120 to 3600 x 2000, glowRadius 2 / 5 / 10, cornerRadius 0 / 40 /
//     a circle, scales 1.0 / 0.5 / 0.25, segments of length 0.01-0.2 with
//     boosts 0.5-2, overlapping, abutting, over the corners, travelling, and
//     under a partial arc - the frame after a light change, the segment table
//     forced on. With overhangs 16 / 8, every COLUMNS_PER_SIGMA from 3 to 8
//     read at most 2 levels with 99.9992% of lit pixels within 1; 2 read 3
//     levels on a thin glow at 3840 x 2160. 4 is one step of margin. Overhangs
//     8 / 4 held at every value tried (99.988%), 32 / 16 read at most 1 level
//     from 4 up but do not fit SEG_MIN_WIDTH. ---
#define GLOW_COVER_SEG_COLUMNS_PER_SIGMA 4.0

// --- How far the glow coverage bake integrates a segment's bell either side
//     of its centre, in units of 1 / invSigma (the bell is exp(-(d invSigma)^2),
//     so 3 is 4.24 standard deviations, where it has fallen to 1.2e-4 of its
//     peak). The bake (neon-glow-cover.frag, segmentsOnPiece) cuts the bell
//     here and NeonRenderer's GetGlowCoverDirtySegmentPieces mirrors it to
//     find the pieces a moved segment reaches: change one and the other
//     follows, or a piece the bell reaches is not re-baked. It was 5 / sqrt(2)
//     (5 sigma); at 3 a moved segment dirties fewer pieces, which took the
//     segment table's bake on a band of 8 segments from 0.75 to 0.58 (timer
//     units, AMD Radeon Pro 5300M), at most 1 level, on up to 1.6% of a
//     frame's lit pixels (0.15% on average over the cover set; I61 in
//     docs/review-findings.md). An 8-node rule on top halved it again but
//     read 3 levels off a long, bright segment, and was not taken. ---
#define GLOW_COVER_BELL_REACH            3.0
#define GLOW_COVER_SEG_OVERHANG          16
#define GLOW_COVER_SEG_CORNER_OVERHANG   8
#define GLOW_COVER_SEG_MIN_WIDTH         128
#define GLOW_COVER_SEG_DIRECT_SHARE      0.75

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- The output dither (R7), peak to peak in 8-bit levels: neonDither
//     (neon-grade.glsl) adds noise in +/- half this to every colour the neon
//     writes to the caller's framebuffer - the blit, the edge ring and the
//     ring's field composite, never an offscreen buffer. The halo and bloom
//     are slow gradients that cross a level only every few px, up to tens of
//     px in the dark tail, so an undithered write draws concentric contour
//     rings around the rect. 1.0 is the least that removes them; 0 turns the
//     dither off. It assumes an 8-bit target. ---
#define OUTPUT_DITHER_LSB         1.0

// --- Epsilons ---
#define WSUM_EPSILON              1e-6

// --- One-sided cut guard band, in BUFFER pixels. Scaled path only.
//
//     One of the two constants in this file stated in buffer px rather than
//     full-res px (BLIT_CUTOFF_GUARD_PX below is the other): it describes the
//     BUFFER's own sampling, so it must NOT be converted with uResolutionScale.
//
//     For pass 1b the one-sided cut is not applied by neon.frag at all -
//     neon-blit.frag applies it at DESTINATION resolution, where a pixel is a
//     pixel and the edge can land exactly where the full-resolution shading
//     puts it. What neon.frag still does is cull the dark side, and the bound
//     it culls at cannot be the cut itself: the blit reconstructs each
//     destination pixel from the 2x2 buffer texels around it, so a texel
//     killed at the cut leaves the first LIT destination pixel rebuilt partly
//     from black. Measured at scale 0.5, glowSide OUTSIDE: the first lit pixel
//     came back at 178 against the 239 full resolution puts there, a dark seam
//     hugging the inside of the glow's own edge.
//
//     So the cull runs this far PAST the cut, and the guard band's emission is
//     what the blit's filter reconstructs the boundary from. One buffer texel
//     is the filter's reach; 2.0 covers that with room for the diagonal, where
//     fwidth of an SDF is sqrt(2) rather than 1, and for the half-texel of
//     phase between the cut and the nearest texel centre.
//
//     The band costs a two-texel strip along the perimeter - nothing beside
//     the half-plane the cull still removes - and none of it is visible: the
//     blit's mask is zero everywhere past the cut, including at the hard step
//     where this bound finally discards.
//
//     NOT used by the edge ring, which owns its own cut (uBlitOwnsCut 0). See
//     neon.frag's sideCull and the post-grade cut block. ---
#define BLIT_SIDE_GUARD_PX        2.0

// --- Cutoff guard band, in BUFFER pixels. Pass 1b only.
//
//     BLIT_SIDE_GUARD_PX's argument, applied to the inside/outside cutoffs.
//     For pass 1b neon-blit.frag applies the cutoff masks at
//     DESTINATION resolution, and neon.frag only culls - this far past the end
//     of each ramp, so the blit's bilinear filter rebuilds every boundary from
//     lit texels rather than from black. Same value as the side guard, for the
//     same reasons: one buffer texel of filter reach, plus room for the
//     diagonal and for the phase between the boundary and the nearest texel
//     centre.
//
//     It replaces CUTOFF_SOFT_FLOOR_PX, a feather in buffer px that let a mask
//     drawn INTO the buffer place its boundary sub-texel. That was the best the
//     buffer could do, and it still softened the boundary across a buffer
//     texel and snapped it toward the buffer grid: on the bounded_band scene of
//     docs/neon-resolution-scale-comparison.html it read p99 41/255 against the
//     1.0 render at scale 0.5, and 69 at 0.25, and the band spread past its
//     own cutoffs as the scale fell (50,703 lit px at 0.5 and 53,568 at 0.25,
//     against 47,012 at 1.0). Drawn by the blit instead it reads p99 4 at 0.5
//     and 12 at 0.25, with 47,012 lit px at every scale; what is left is the
//     reduced-resolution glow inside the band, not its edges. See
//     docs/neon-resolution-scale-plan.md, step 2.
//
//     NOT used by the edge ring, whose masks and culls are its own. See
//     neon.frag's cutGuard and NeonRenderer::setupGeometry's cutGuardPx. ---
#define BLIT_CUTOFF_GUARD_PX      2.0

// --- Edge ring width. CPU only.
//
//     At every scale, NeonRenderer redraws a ring around the rect
//     edge at FULL resolution (neon.frag's ring program),
//     reading only the gather's result from the gather buffer. The ring reaches R
//     full-res px either side of the edge (GetRingWidth in neon-renderer.cpp):
//
//         R = max(reach(1.0), reach(scale)) + RING_GUARD_TEXELS / scale
//
//     where reach(s) is the filament's reach as neon.frag computes it at scale
//     s, in full-res px. BOTH, because inside R the reduced pass's filament -
//     floored to what its buffer can sample, so wider than the real one
//     whenever the line is thin - is replaced by the real one, and R has to
//     clear whichever reaches further. Uncapped: the reach is where a filament
//     has fallen to FILAMENT_CUTOFF of its gain, about 2/255 after the grade,
//     which is exactly how far its error can show. RING_GUARD_TEXELS adds the
//     reduced buffer's bilinear footprint past that.
//
//     Calibrated over lineWidth {1, 2, 4, 8, 16} x filamentFalloff
//     {0.5, 1, 2, 4} x glowRadius {0, 2, 5, 20}, each config rendered at
//     scales 0.75, 0.5 and 0.25 and compared against its own 1.0 render
//     (docs/neon-resolution-scale-plan.md, step 5). Worst error over all 80:
//
//       RING_GUARD_TEXELS    scale 0.75    scale 0.5    scale 0.25
//              0.5               3            12            3
//              1.0               4             2            2
//              2.0               2             2            2
//
//     1.0 is the smallest that holds 2/255 at 0.5 and 0.25, the targets. 2.0
//     also brings 0.75 within 2 (its worst at 1.0 is a 1 px flat-top line
//     with no glow) for a ring one buffer texel wider each side.
//
//     The two things that made the first version of this ring fail that sweep,
//     both now handled: R used to cover only the full-res filament (fixed by
//     taking the reduced pass's too, above), and at a small glowRadius pass 1's
//     quad stopped short of R, so the ring read gather texels nothing wrote
//     (fixed by covering the ring with the quad; since the gather was split
//     out, the gather pass's own quad, built in setupRingGeometry, covers
//     it). ---
#define RING_GUARD_TEXELS         1.0

// --- Gather resolution. CPU only.
//
//     At every resolutionScale the gather - ~95% of neon.frag's cost when it
//     ran inline - runs in a pass of its own (neon-gather.frag) at its own
//     scale, and pass 1 and the
//     edge ring shade from its result. Its scale is (GetGatherScale in
//     neon-renderer.cpp)
//
//         clamp(GATHER_TEXELS_PER_KERNEL / kc, GATHER_MIN_SCALE,
//               min(resolutionScale, GATHER_MAX_SCALE))
//
//     with kc the colour kernel's width in full-res px, perimeter *
//     COLOR_BLEND_PERIM_FRAC: the gather's outputs are Lorentzian-weighted
//     means whose kernel is never narrower than kc, so this many texels per kc
//     carries them through a bilinear read.
//
//     Calibrated on Mesa llvmpipe with an RGBA16F gather buffer (worst error
//     against 1.0 over `probe scenes`, and the default scene's frame time):
//
//       GATHER_TEXELS_PER_KERNEL   worst error   default 0.5   default 0.25
//                1.0                   4              -              -
//                1.5                   2           10.5 ms        8.0 ms
//                2.0                  1-2          10.8 ms        7.3 ms
//                4.0                   -           14-17 ms      11-12 ms
//         (gather at resolutionScale)  1-2          37.5 ms       12.3 ms
//
//     2.0 is the knee: 1.5 saves nothing measurable (the gather is no longer
//     the pass that costs) and 4.0 costs half as much again for no visible
//     gain. With an RGBA8 gather buffer - the fallback - 2.0 reads 3/255.
//     GATHER_MIN_SCALE barely matters - full-viewport rects
//     at 720p and 1080p read 1-2 at a floor of 0 and 0.0625 alike - and is
//     kept as a cheap guard: the gather buffer is clipped to the viewport, so
//     at the floor it is at most 128 x 80 texels at 1080p. See
//     docs/neon-resolution-scale-plan.md section 13. ---
#define GATHER_TEXELS_PER_KERNEL  2.0
#define GATHER_MIN_SCALE          0.0625

// --- The gather's ceiling. CPU only.
//
//     GetGatherScale never gathers finer than this, at any resolutionScale.
//     It binds only on a rect whose perimeter is under ~450 px (kc under 4
//     px), which would otherwise gather near full resolution: its gather
//     buffer - RGBA16F over the glow quad's box, two attachments with
//     segments - ran to 16 / 33 MB at 1080p for a 40 x 24 rect whose glow
//     fills the frame. At 0.5 that is a quarter, the same as the scaled path
//     at 0.5 always gathered at, and the check suite's smallest rect (perimeter
//     478 px) is not touched.
//
//     It replaces the split gate at 1.0 (FULL_RES_SPLIT_GATHER_MAX_SCALE /
//     FULL_RES_SPLIT_MIN_AREA_PX, removed - docs/neon-shader-cleanup-plan.md
//     step 3), which ran the gather INLINE in neon.frag where splitting it
//     out did not pay: on an AMD Radeon Pro 5300M, where the first offscreen
//     pass of a frame costs ~0.15 ms, for a still cutoff band under ~140k px
//     of quad and for a gather near full resolution. Since unchanged frames
//     stopped re-running the gather (I40) the still case no longer leaves
//     the caller's framebuffer at all; an Apple M2 Pro measured the split
//     1.1-4.75x faster at every size and mode, and the AMD, re-measured with
//     every rect split against its own gate, 0.98-1.02x in every animated
//     mode with one loss - a cutoff band under an intensity pulse, 0.84x.
//     So every scale splits, and the inline loop went with the gate. ---
#define GATHER_MAX_SCALE          0.5

// --- Stand-in distance for a cutoff that glowSide has already subsumed.
//
//     neon.frag hands this to the band distance on the side the one-sided cut
//     culls, so the discard and the mask there no-op through the arithmetic
//     exactly as a DISABLED cutoff does - same mechanism, same sentinel shape,
//     no extra branch downstream.
//
//     Matches CUTOFF_DISABLED_SIZE in neon-renderer.cpp, which is what a
//     disabled cutoff actually arrives as. It is a distance in the shader's own
//     px space and only has to be far enough that no realistic geometry reaches
//     it; see the note on that constant for the same reasoning. ---
#define CUTOFF_NEUTRALISED        1.0e6

// --- Glow reach (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity).
//
//     Named for the reach, not for an early-out: the per-fragment
//     `ad > earlyOut -> discard` this constant was originally calibrated for
//     no longer exists. Geometry culls the far region instead, which is
//     tiler-friendly, so what the factor sets is how far the glow is allowed to
//     reach before the quad stops covering it. The locals it feeds already say
//     so - `glowReach` in both setupGeometry implementations, `reach` in both
//     shaders.
//
//     glowRadius only: the companion sampleSpacing * SPACING_FACTOR term is
//     gone. sampleSpacing is perimeter / NEON_MAX_LOOP_SAMPLES, so it won on
//     any reasonably large rect at default glowRadius and made both the
//     distance the bloom got truncated at and the brightness it still had
//     there track the rect size. The bloom's reach is a function of glowRadius
//     alone, so that is all that sizes the quad.
//
//     Used by the renderers' setupGeometry AND by the shaders, which recompute
//     the same expression to place the bloom pedestal that lets this margin
//     stay tight without the truncation showing. Keep the two in step. ---
#define GLOW_REACH_RADIUS_FACTOR  48.0

// --- Where the shaders' quad-edge fade begins, as a FRACTION of the quad
//     margin. The emission ramps to zero over [FRAC * margin, margin], so the
//     bloom's 1/D tail - still ~10% of peak out at the quad edge - never clips
//     as a hard rectangle.
//
//     A fraction, not a pixel span, because the margin is proportional to
//     glowRadius (see GLOW_REACH_RADIUS_FACTOR) and so is the bloom profile it
//     hides. A fraction keeps the ramp at a constant proportion of the bloom's
//     reach, so the fade reads the same at every glow radius; a fixed px ramp
//     would vanish on a wide glow and dominate a narrow one. At the stock
//     glowRadius 5 / bloom 0.3 / intensity 1 the margin is 312 px, giving a
//     62 px ramp.
//
//     Nothing derives 0.8 - anything leaving a ramp wide enough to hide the
//     clip behaves the same. What it DOES assume is that the margin was set by
//     the glow, where 20% of it is a long distance. When outsideCutoff clamps
//     the margin instead (to where its fade ends, size + softness, plus 1),
//     20% of ~13 px is 2.6 px and
//     the ramp lands INSIDE the cutoff band, dimming the band's outer edge
//     ahead of the cutoff mask - and only on the exterior, since the fade keys
//     on positive d. Both shaders therefore floor the ramp's start at the
//     cutoff boundary whenever that boundary falls inside the quad; see the
//     fadeStart block in neon.frag.
//
//     Since V23 the bloom no longer relies on this fade to end smoothly - each
//     piece fades by its own distance from BLOOM_FADE_START_FRAC, below, which
//     has it under a level well before this ramp starts. ---
#define QUAD_FADE_START_FRAC      0.8

// --- Where each piece's bloom starts fading out, as a FRACTION of `reach`
//     (neon.frag's bloom end fade, V23).
//
//     The pedestal takes the bloom's 1/a tail to exactly 0 at `reach`, but
//     with its slope still on, so the glow stopped on a crease: inside a large
//     rect, a hard-edged dark rectangle `reach` in. The quad-edge fade above
//     hid it outside, but over [0.8, 1.0] of the margin, where the tail is
//     still ~5 levels up, and a fade that late STEEPENS the tail before it
//     flattens it - a shoulder, then flat, which reads as an edge too. So
//     each piece's bloom is multiplied by 1 - smoothstep(FRAC * reach, reach,
//     a) of its own distance, on both sides.
//
//     0.5 is about the latest start that does not steepen it: the steepest
//     slope inside the fade over the slope where it starts, on the
//     infinite-line bloom at glowRadius 5, bloomStrength 0.52, intensity 0.8,
//     is 1.40 at 0.8, 1.14 at 0.6, 1.035 at 0.5 and 1.00 from 0.4 down. (It
//     is also where c * (reach - a)^2 meets the pedestalled 1/a tail in value
//     and slope.) An earlier start costs more of the tail; at 0.5 the glow
//     reads up to 6 levels dimmer in the outer half of its reach. ---
#define BLOOM_FADE_START_FRAC     0.5

// --- Filament reach for the same quad sizing, expressed in sigmas.
//
//     glowRadius = 0 means "filament only" - the halo and bloom are gated off
//     by GLOW_GATE_FADE_PX - but the filament itself is still there, sized by
//     lineWidth, so the quad has to cover it or the exterior gets clipped.
//
//     The reach is NOT a constant, because the profile is
//     exp2(-(ad/sigma)^N) with N = 2 * filamentFalloff and the slider runs
//     down to falloff 0. Solving core * FILAMENT_GAIN < FILAMENT_CUTOFF gives
//
//         reach = sigma * log2(FILAMENT_GAIN / FILAMENT_CUTOFF) ^ (1/N)
//
//     which is 1.9 sigmas at falloff 2.0, 3.5 at 1.0, 12.6 at 0.5, 67.8 at 0.3
//     and 558 at 0.2 - it diverges as falloff -> 0. A fixed 12 (calibrated for
//     falloff 0.5) left the low end badly under-sized: the interior is
//     unbounded because the quad always covers it, so a soft filament flooded
//     inward while the exterior was chopped at 12 sigmas. That asymmetry is
//     the "no outer glow at glowRadius 0" report.
//
//     MAX_SIGMAS bounds the fill cost where the formula diverges; the shaders
//     pedestal-subtract the core at exactly this reach, so hitting the cap
//     shortens the glow symmetrically on BOTH sides instead of showing a cliff
//     on the outside. MIN_SIGMAS keeps a little room at very high falloff.
//     Keep the expression in step across neon-tuning.h, both setupGeometry
//     implementations, and both fragment shaders. ---
#define FILAMENT_CUTOFF           0.002
#define FILAMENT_REACH_MIN_SIGMAS 2.0
#define FILAMENT_REACH_MAX_SIGMAS 64.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_

#define NEON_GLOW_COVER_BAKE
// ---------------------------------------------------------------------------
// The emitter's pieces, shared by neon.frag and neon-glow-cover.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it, after the tuning
// header, into the two programs that have to agree on it to the texel:
// neon.frag, which sums the halo and bloom over the emitter's eight pieces and
// scales each by that piece's coverage from the glow coverage table, and
// neon-glow-cover.frag, which bakes the table. Three things live here because
// both sides need them:
//
//   - minimaxAtan, the atan both sides develop a corner with.
//   - arcTangentSegment, how a corner arc is developed onto a straight line.
//     neon.frag integrates a corner's halo and bloom along that line, and the
//     bake convolves the corner's coverage along the same line, so what scales
//     a corner's glow is the coverage of the extent that glow was integrated
//     over.
//   - the table's layout, as a forward map (where a piece's coverage is read,
//     for neon.frag) next to its inverse (what each texel holds, for the
//     bake). The two must be exact inverses, which is the reason they sit
//     side by side rather than one in each file.
//
// Every length here is in one unit chosen by the caller - px in neon.frag,
// perimeter fractions in the bake. The layout depends only on ratios of
// lengths (to the halo width kh, to a piece's own length), so the two agree
// whatever the unit and at every resolution scale.
// ---------------------------------------------------------------------------

precision highp float;

// Own names: neon-common.glsl, which neon.frag also includes, declares PI and
// HALF_PI as constants, and the bake does not include it.
#define PIECES_PI      3.141592653589793
#define PIECES_HALF_PI 1.5707963267948966

// --- A cheaper two-argument atan ----------------------------------------
// The shading's two hottest transcendentals are atans: bloomSegment's (every
// straight's bloom and every corner's, in neon.frag) and the corner
// development's `th` below. GLSL's atan(y, x) is the driver's, and measured on
// an AMD Radeon Pro 5300M this octant-reduced minimax polynomial in its place
// made every frame whose config animates 1.07-1.11x faster, at 1.0 and 0.5,
// moving at most 0.016% of a frame's channels by 1 level (I48).
// Its error is at most 1.7e-6 rad over the whole range - a few float ulps of
// PI - so nothing downstream can tell it from the built-in.
//
// Here rather than in neon.frag so the coverage bake develops the corners
// through the same function as the read: the two have to agree on `th`.
// Undefined only where atan(y, x) is (both zero): it returns 0 there.
// Measure on each target GPU before trusting the gain - a cheaper atan is the
// most GPU-specific change this file has.
float minimaxAtan(float y, float x) {
    float ax = abs(x);
    float ay = abs(y);
    float t  = min(ax, ay) / max(max(ax, ay), 1e-30);
    float s  = t * t;
    float r  = t * (0.99997726 + s * (-0.33262347 + s * (0.19354346 + s * (-0.11643287 +
               s * (0.05265332 + s * -0.01172120)))));
    r = (ay > ax) ? PIECES_HALF_PI - r : r;
    r = (x < 0.0) ? PIECES_PI - r : r;
    return (y < 0.0) ? -r : r;
}

// --- Corner arcs, developed onto their tangent -------------------------
// The four straights cover the rect's flat runs. Above cornerRadius 0 the
// emitter also turns through four quarter arcs, and a circular arc has no
// elementary antiderivative under either kernel. This develops each arc onto a
// straight line instead: the tangent at whichever ARC POINT IS NEAREST the
// fragment, carrying the arc's full length PI*r/2 and split about that point.
//
// That choice is what makes it accurate where the naive one is not. The
// perpendicular distance it reports is the true distance to the arc wherever
// the fragment faces it (|length(w) - r|), and the two halves run exactly as
// far as the real arc does in each direction, so the developed arc abuts the
// trimmed straights in arclength and the emitter is continuous - no gap and no
// overlap at the tangent points.
//
// `w` is the fragment's offset from the arc centre in that corner's own frame:
// x along the outward normal of one incident edge, y along the other's, both
// positive pointing away from the rect. So the arc occupies exactly the first
// quadrant of `w`, from +x (one tangent point) to +y (the other), and clamping
// the direction into that quadrant is max(w, 0).
//
// Off the ends the nearest arc point is a tangent point, and which one follows
// from |w - (r,0)|^2 - |w - (0,r)|^2 = 2r*(w.y - w.x): the +x end when
// w.x >= w.y. That is what the fallback picks, so it is the right clamp for the
// whole region it covers and not only for the degenerate point that forces it.
//
// --- THE DEVELOPMENT RATE IS NOT r. ------------------------------------
// Laying the arc out at its own arclength - one unit of tangent per unit of
// arc - is only right for a fragment ON the arc. The exact distance to the
// point at angle dphi from the nearest one is
//
//     D^2 = a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,   rho = length(w)
//
// so the tangent coordinate the kernels actually want is t = 2*sqrt(rho*r)*
// sin(dphi/2), whose slope at the foot is sqrt(rho*r), not r. Develop at rate
// `lam` instead of r and the emitter comes out short or long, so the measure
// is put back by scaling the whole segment by r/lam - `w` of the returned
// vec4. Rate x weight is r either way, so the arc always carries its full
// PI*r/2 of emitter.
//
// Two things follow, and they are the whole reason for the change:
//
//   - AT THE CENTRE OF CURVATURE IT IS NOW EXACT. rho -> 0 collapses the
//     segment to zero length against an infinite weight, and the limit is
//     f(r) * PI*r/2: every point of the arc at distance r, which is what a
//     fragment at the centre actually sees. At rate r the arc ran off to one
//     side of the foot instead, and since both kernels peak at t = 0 that
//     UNDER-counted - on a circle, where all four arc centres coincide at the
//     middle of the shape, to 54% of the true value.
//   - THE CLAMP STOPS CREASING. Crossing w.y = 0 the arc's endpoints slide at
//     -lam * d(th) on the facing side and at -d(off) on the clamped side; the
//     first is -lam/w.x and the second -1, and they agree only where lam is
//     length(w). At rate r they agreed only at w.x == r, so every other point
//     of the lines through the arc centre carried a C1 crease - a dark cross
//     at the centre of curvature, unmistakable on a circle. Measured as the
//     spurious curvature of (model - numerically integrated truth): 19.9% of
//     the local value at rate r, 0.5% here.
//
// lam is min(rho, sqrt(rho*r)), i.e. sqrt(rho * min(rho, r)). The inner
// branch's rate has to be rho for the clamp to join smoothly; outside the arc
// the linearisation above wants sqrt(rho*r), and the two meet at rho == r,
// where lam is r and the weight is 1 - so a fragment on the arc is bit-
// identical to the rate-r form this replaces, and the calibration the NORM
// factors carry is untouched. Against a numerically integrated perimeter the
// whole emitter's worst error drops as well, on every geometry tested: 16.5 ->
// 9.8% on a 600x400 r=40, 37.3 -> 26.3% on a circle.
//
// Cost is 1.03x of the neon pass at 1280x720, and - as the arc pedestal's note
// in neon.frag warns - that includes the cornerRadius 0 path, which never
// executes a line of this and still pays 1.026x for the register pressure.
// Re-time both after touching it. An inversesqrt formulation that trades the
// sqrt and the divide for two inversesqrts was measured and came out inside
// the noise, so the readable form stays.
//
// Returns (a, t1, t2, weight) for haloSegment / bloomSegment; the caller
// multiplies the segment by .w. The coverage bake reads the same four numbers
// to lay the arc's coverage along that line: arclength q from the +x end sits
// at t1 + q / weight.
//
// `u` is the direction of the arc point the arc is developed about, which
// arcTangentSegment below picks; the bake also calls this with each of the two
// fallback directions, behind the centre where the pick flips between them.
vec4 arcTangentSegmentAbout(vec2 w, float r, vec2 u) {
    // Arclength from the +x tangent point to the nearest arc point. u is a unit
    // vector in the first quadrant whenever it came from wq, so th is in
    // [0, HALF_PI] and needs no clamp of its own.
    float th  = minimaxAtan(u.y, u.x);
    float a   = abs(dot(w, u) - r);
    // Offset of the fragment ALONG the tangent, zero whenever u came from wq
    // (the foot of perpendicular is then the tangent point itself) and non-zero
    // only on the fallback, where it correctly pushes the whole arc to one side.
    // A LENGTH, not an arclength, so it is not scaled by the rate below.
    float off = dot(w, vec2(-u.y, u.x));
    // Floored so the exact centre of curvature cannot divide by zero. The
    // floor is far below one px, and the limit it lands on is the exact value
    // anyway - see above.
    float rho = max(length(w), ARC_FRAME_EPSILON);
    float lam = sqrt(rho * min(rho, r));
    return vec4(a, -lam * th - off, lam * (PIECES_HALF_PI - th) - off, r / lam);
}
// The direction arcTangentSegment develops about: the nearest arc point's.
vec2 arcTangentDirection(vec2 w) {
    vec2  wq = max(w, vec2(0.0));
    float ql = length(wq);
    return (ql > ARC_FRAME_EPSILON) ? wq / ql : ((w.x >= w.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
}
vec4 arcTangentSegment(vec2 w, float r) {
    return arcTangentSegmentAbout(w, r, arcTangentDirection(w));
}

// --- The glow coverage table's layout (V21) ----------------------------
// One table per PIECE of the emitter, because the coverage that scales a
// piece's halo and bloom is that piece's own: its coverage over its own extent,
// weighted by the layer's kernel at the fragment. V20's table was one per
// PERIMETER, indexed by the foot's perimeter position, and convolved along a
// straight line through the foot that ran on past the piece's ends - so near a
// corner where a lit stretch met a dark one, each piece borrowed its
// neighbour's coverage (V21 in docs/review-findings.md).
//
// `tableLayout.x` texels across - the table's width - and four bands of
// GLOW_COVER_ROWS rows down. Band b holds straight b at its left and corner b
// at its right:
//
//   straight   `tableLayout.y` overhang columns, the straight's own `inner`
//              columns, `tableLayout.y` more: across, the fragment's projection
//              along the straight -
//              uniform over the piece and rational past each end; down, its
//              distance a from the line, a / (a + kh), as V20's table had it. A
//              straight's clipped extent follows from the projection, so two
//              variables still describe it, and its table is symmetric about
//              the line.
//   corner     a guard texel, `tableLayout.z` overhang columns, the corner's own
//              `tableLayout.w` - inner columns, `tableLayout.z` more, a guard: across, the
//              fragment's direction from the arc's centre - uniform over the
//              arc's own quadrant, then on either side of it to the diagonal
//              behind the centre, where the two sides meet; down, its
//              distance from the centre, the arc's inside in the top half and
//              its outside in the bottom. Polar position determines everything
//              arcTangentSegment returns, inside, outside and behind the centre
//              alike, so two variables describe a corner too.
//
// THE LAYOUT IS A PARAMETER (`tableLayout`, the uniform uGlowCoverLayout): .x the
// columns - the texture's width, so the forward maps divide by the texture
// they read - .y a straight's overhang columns past each end, .z a corner's
// either side of its arc, and .w the columns a band's straight and corner
// share, .x - 2 - 2 .y - 2 .z. NeonRenderer sizes the table per config and
// stores the layout WITH it (GlowCoverLayout), so the bake and every read take
// the same four numbers; all are whole columns.
//
// `inner`, how many of a band's `tableLayout.w` columns its straight gets, is the
// uniform uGlowCoverSplit (.x the vertical straights' bands, .y the
// horizontal ones'), whole columns, set by NeonRenderer::GetGlowCoverSplit in
// proportion to the straight's and the arc's lengths. It comes from the CPU
// rather than from each shader's own arithmetic because the two shaders work
// in different units, and a split that rounded differently in one of them
// would read every texel of the band from the wrong place. So a circle's
// corners get the columns its zero-length straights do not need, and a long
// rect's straights get its small corners': at a fixed size, every piece gets
// a column density in proportion to its length.
//
// THE READ HAS TO BE CHEAP, AND THAT IS WHY EVERY MAP HERE IS A DIVISION.
// neon.frag computes these coordinates for up to eight pieces, and at scale 1.0
// the program also carries the gather loop, which makes it sensitive to the
// size of everything else in it - including code that a frame never runs.
// Measured on an AMD Radeon Pro 5300M, against the build before V21, on a fully
// lit ring that skips every read: with the corner direction an atan, the
// spacing past each end a log and the inside of the arc spaced by two more
// logs, the whole neon pass was 1.16x slower - 1.163x on a SHARP-cornered ring,
// which never enters the corner block at all - and stubbing the corner
// coordinates out took it to 1.04x. So the direction is a diamond angle (one
// division, see glowCoverDiamond), the spacing past the ends rational, and the
// inside a ratio of square roots, which took it to ~1.08x; the rest came from
// running neon.frag's corner block one arc at a time (addCornerPiece there).
// The bake inverts the maps, where cost does not matter. Do not put an atan or
// a log back in the forward maps without re-timing a fully lit ring at scale
// 1.0.
//
// WHICH PROGRAM COMPILES WHICH HALF. The forward maps (where a piece's
// coverage is READ) and their inverses (what each texel HOLDS) are compiled
// only into the programs that call them, since this compiler builds every
// function in a source whether main() reaches it or not: neon.frag takes the
// forward half, the bake (NEON_GLOW_COVER_BAKE) the inverse half, and the
// segment table's fill (NEON_GLOW_COVER_FILL), which decodes a texel of one
// table and reads its value from another, both.
#if !defined(NEON_GLOW_COVER_BAKE)
#define GLOW_COVER_FORWARD_MAPS
#endif
#if defined(NEON_GLOW_COVER_BAKE) || defined(NEON_GLOW_COVER_FILL)
#define GLOW_COVER_INVERSE_MAPS
#endif

// Which straight is which band, by the side of the rect in vPos it lies on.
// The vertical straights (x = -/+ halfSize.x) have length 2 * straight.y, the
// horizontal ones 2 * straight.x. Corner block b is in band b too.
#define GLOW_COVER_BAND_NEG_X  0
#define GLOW_COVER_BAND_POS_X  1
#define GLOW_COVER_BAND_NEG_Y  2
#define GLOW_COVER_BAND_POS_Y  3

// How far a corner's direction coordinate runs past each end of its arc: the
// arc is [0, 1] in it, and both sides reach the diagonal behind the centre at
// 1.5 past an end - 3/8 of a turn.
#define GLOW_COVER_CORNER_REACH 1.5

// Which corner block - and so which band - a corner's table is in: `signs` is
// the corner's quadrant in vPos, +-1 on each axis.
#ifdef GLOW_COVER_FORWARD_MAPS
int glowCoverCornerBlock(vec2 signs) {
    return (signs.x > 0.0 ? 1 : 0) + (signs.y > 0.0 ? 2 : 0);
}
#endif

// Band `band`'s straight interior columns, from uGlowCoverSplit.
float glowCoverInner(int band, vec2 split) {
    return (band < 2) ? split.x : split.y;
}

// A distance from the line as a row coordinate in [0, 1]: half the rows within
// one halo width, the rest out to the far field. V20's spacing.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverDistanceRow(float a, float kh) {
    return a / (a + kh);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverRowDistance(float v, float kh) {
    return kh * v / (1.0 - v);
}
#endif

// Past a straight's end, `e` the distance past it: half the overhang's columns
// within a quarter of them in halo widths, so a column is kh / 4 at the end
// and the spacing grows in proportion to e beyond. Rational rather than
// logarithmic for the reason above.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverStraightOver(float e, float kh, vec4 tableLayout) {
    return e / (e + 0.25 * tableLayout.y * kh);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverStraightPast(float over, float kh, vec4 tableLayout) {
    return 0.25 * tableLayout.y * kh * over / (1.0 - over);
}
#endif

// A direction as a "diamond angle": monotone in the true angle and continuous
// all the way round, without an atan. The arc's own quadrant is [0, 1] - 0 at
// its +x end, 1 at +y - and the rest of the turn runs on from either end to
// -1.5 / 2.5, which are both the diagonal behind the centre. `w` need not be
// normalised; at the centre itself any direction is as good as another.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverDiamond(vec2 w) {
    float l = abs(w.x) + abs(w.y);
    float p = (l > 0.0) ? w.x / l : 1.0;
    return (w.y >= 0.0) ? 1.0 - p : ((p < -0.5) ? 3.0 + p : p - 1.0);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
vec2 glowCoverDiamondDirection(float d) {
    float q = (d < 0.0) ? d + 4.0 : d;
    float p = (q <= 2.0) ? 1.0 - q : q - 3.0;
    return normalize(vec2(p, ((q <= 2.0) ? 1.0 : -1.0) * (1.0 - abs(p))));
}
#endif

// Past a corner's end, `d` the diamond angle past it: half the columns spaced
// evenly to the diagonal behind the centre, which is what a fragment far from
// the corner needs, and half concentrated at the end, a column about kh / 4 of
// arc there, which is what one beside the arc needs.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverCornerOver(float d, float r, float kh, vec4 tableLayout) {
    float k = tableLayout.z * kh / (8.0 * max(r, 1e-30));
    float R = GLOW_COVER_CORNER_REACH;
    return 0.5 * d / R + 0.5 * d * (R + k) / (R * (d + k));
}
#endif
// Its inverse: A d + B d / (d + k) = t is a quadratic in d, solved by whichever
// form of its positive root does not cancel.
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverCornerPast(float t, float r, float kh, vec4 tableLayout) {
    float k    = tableLayout.z * kh / (8.0 * max(r, 1e-30));
    float R    = GLOW_COVER_CORNER_REACH;
    float A    = 0.5 / R;
    float b    = A * k + 0.5 * (R + k) / R - t;
    float disc = sqrt(b * b + 4.0 * A * t * k);
    return (b >= 0.0) ? 2.0 * t * k / max(b + disc, 1e-30) : (disc - b) / (2.0 * A);
}
#endif

// Inside a corner's arc the coverage changes fast in two places - beside the
// arc, where the halo's width is kh, and near the centre, where the developed
// arc's width r / lam grows without bound - and slowly between. A ratio of
// square roots spaces the rows finely at both ends: `rho` the distance from the
// centre, 0 on the arc and 1 at the centre. Its inverse is closed form.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverInsideRow(float rho, float r) {
    float fromArc = sqrt(max(r - rho, 0.0));
    float fromCentre = sqrt(max(rho, 0.0));
    return fromArc / max(fromArc + fromCentre, 1e-30);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverInsideRadius(float v, float r) {
    return r * (1.0 - v) * (1.0 - v) / (v * v + (1.0 - v) * (1.0 - v));
}
#endif

// Texture coordinate of straight `band`'s coverage: `x` is the fragment's
// projection along it from its t1 end (haloSegment's), UNCLAMPED, `len` the
// straight's length, `a` the distance from its line, `kh` the halo width,
// `inner` glowCoverInner, `tableLayout` the table's. Clamped to the straight's own
// texels, since its corner's guard is the next one along. The inverse is
// glowCoverStraightAt.
#ifdef GLOW_COVER_FORWARD_MAPS
vec2 glowCoverStraightUV(int band, float x, float len, float a, float kh, float inner, vec4 tableLayout) {
    float over  = tableLayout.y * glowCoverStraightOver(max(-x, 0.0) + max(x - len, 0.0), kh, tableLayout);
    float along = (len > 0.0) ? clamp(x / len, 0.0, 1.0) : 0.5;
    float col   = tableLayout.y + inner * along + (x < 0.0 ? -over : over);
    col         = clamp(col, 0.5, 2.0 * tableLayout.y + inner - 0.5);
    float row   = clamp(glowCoverDistanceRow(a, kh) * float(GLOW_COVER_ROWS), 0.5, float(GLOW_COVER_ROWS) - 0.5);
    return vec2(col / tableLayout.x,
                (float(band * GLOW_COVER_ROWS) + row) / float(GLOW_COVER_HEIGHT));
}
#endif

// Texture coordinate of a corner's coverage: `w` is the fragment's offset from
// the arc centre in arcTangentSegment's frame, `block` from
// glowCoverCornerBlock, `inner` its band's straight's glowCoverInner,
// `tableLayout` the table's. The inverse is glowCoverCornerAt. The seam is the
// diagonal behind the centre, where the two overhangs meet; the guard column
// at each end of the block holds the other end's value, so a fetch is
// continuous across it.
#ifdef GLOW_COVER_FORWARD_MAPS
vec2 glowCoverCornerUV(int block, vec2 w, float r, float kh, float inner, vec4 tableLayout) {
    float arcCols = tableLayout.w - inner;
    float d       = glowCoverDiamond(w);
    float over    = tableLayout.z * glowCoverCornerOver(max(-d, 0.0) + max(d - 1.0, 0.0), r, kh, tableLayout);
    float col     = tableLayout.z + arcCols * clamp(d, 0.0, 1.0) + (d < 0.0 ? -over : over);
    float rho     = length(w);
    float side    = (rho >= r) ? glowCoverDistanceRow(rho - r, kh) : -glowCoverInsideRow(rho, r);
    float halfRows = 0.5 * float(GLOW_COVER_ROWS);
    float row     = clamp(halfRows * (1.0 + side), 0.5, float(GLOW_COVER_ROWS) - 0.5);
    return vec2((2.0 * tableLayout.y + inner + 1.0 + col) / tableLayout.x,
                (float(block * GLOW_COVER_ROWS) + row) / float(GLOW_COVER_HEIGHT));
}
#endif

// Inverse of glowCoverStraightUV for one texel: the projection `x` and the
// distance `a` its centre holds, from its column and row within the band.
#ifdef GLOW_COVER_INVERSE_MAPS
vec2 glowCoverStraightAt(float col, float row, float len, float kh, float inner, vec4 tableLayout) {
    float over = tableLayout.y;
    float x;
    if (col < over) {
        x = -glowCoverStraightPast((over - col) / over, kh, tableLayout);
    } else if (col > over + inner) {
        x = len + glowCoverStraightPast((col - over - inner) / over, kh, tableLayout);
    } else {
        x = (col - over) / max(inner, 1.0) * len;
    }
    return vec2(x, glowCoverRowDistance(row / float(GLOW_COVER_ROWS), kh));
}
#endif

// Inverse of glowCoverCornerUV for one texel: the offset `w` from the arc
// centre its centre holds, from its column within the corner's block (guards
// included, so 0 and the block's last are the guards) and its row.
#ifdef GLOW_COVER_INVERSE_MAPS
vec2 glowCoverCornerAt(float col, float row, float r, float kh, float inner, vec4 tableLayout) {
    float arcCols = tableLayout.w - inner;
    float over    = tableLayout.z;
    // Past the guard, the column is the same direction the other end's last
    // texel holds: the seam is one direction, approached from both sides.
    float cols    = arcCols + 2.0 * over;
    float c       = col - 1.0;
    c            -= cols * floor(c / cols);
    float d;
    if (c < over || c > over + arcCols) {
        float past = glowCoverCornerPast(((c < over) ? over - c : c - over - arcCols) / over, r, kh, tableLayout);
        d = (c < over) ? -past : 1.0 + past;
    } else {
        d = (c - over) / arcCols;
    }
    float halfRows = 0.5 * float(GLOW_COVER_ROWS);
    float rho = (row >= halfRows) ? r + glowCoverRowDistance((row - halfRows) / halfRows, kh)
                                  : glowCoverInsideRadius((halfRows - row) / halfRows, r);
    return rho * glowCoverDiamondDirection(d);
}
#endif

// Which texel of a table laid out as `tableLayout` and split as `split` the
// fragment at `fragCoord` is: its band, its row within the band (at the
// texel's centre), the band's straight interior columns, and the column its
// corner block starts at - left of it the band's straight, from it on its
// corner. The bake and the segment table's fill both decode a texel here, so
// the two cannot place one differently.
#ifdef GLOW_COVER_INVERSE_MAPS
void glowCoverTexel(vec2 fragCoord, vec2 split, vec4 tableLayout, out int band, out float row, out float inner,
                    out float splitColumn) {
    float y     = floor(fragCoord.y);
    band        = int(y) / GLOW_COVER_ROWS;
    row         = y - float(band * GLOW_COVER_ROWS) + 0.5;
    inner       = glowCoverInner(band, split);
    splitColumn = 2.0 * tableLayout.y + inner;
}
#endif

precision highp float;

// ---------------------------------------------------------------------------
// Glow coverage pre-pass (V20, V21).
//
// Bakes, for every PIECE of the emitter - the four straights and the four
// corner arcs neon.frag sums the halo and bloom over - how lit that piece is
// as each layer sees it from a fragment: the piece's own coverage over its own
// extent, weighted by the layer's kernel at the fragment's distance,
//
//     INTEGRAL_piece cover(q) K(q - x) dq  /  INTEGRAL_piece K(q - x) dq,
//
// with q the arclength along the piece and x the fragment's foot on it.
// neon.frag scales each piece's halo and bloom - itself that piece's integral
// of K - by one filtered fetch of it, so the product is the piece's integral
// of cover * K. The table's layout, and where a fragment reads it, is in
// neon-pieces.glsl, injected ahead of this file.
//
// Why a table
// -----------
// Each piece's halo and bloom has to be scaled by the coverage of THAT piece,
// near ITS foot, under ITS kernel - not by one coverage gathered around the
// fragment, which is dominated by whichever piece is nearest. Scaled by the
// fragment's, a dark stretch borrowed light from lit ones far along the line
// (V19, a thin bright line along it) and the lit far edges were dimmed by the
// dark near one (V20, a groove along it). The closed form evaluated in
// neon.frag for eight pieces per fragment cost 2-3x on a partly lit ring, and
// none of it depends on the fragment except through two numbers per piece, so
// it is baked here once per config change and read with one fetch per piece.
//
// Why one per piece (V21)
// -----------------------
// V20 baked one table per PERIMETER, indexed by perimeter position and
// distance, which convolved the coverage along a straight line through the
// foot that ran on past the piece's ends as if the outline did not turn. So
// near a corner where a lit stretch met a dark one, a lit piece read a little
// dim and a dark one a little lit: 8-15 levels over most of a partly lit
// frame, 33 in one segment scene. Here each piece's integral stops at its own
// ends, and a corner's runs along the line arcTangentSegment develops it onto
// - the line its halo and bloom are integrated along.
//
// The kernels
// -----------
// Along a line at distance a the halo's kernel is c^2 / (t^2 + c^2)^1.5 and
// the bloom's c / (t^2 + c^2), c = sqrt(a^2 + k^2) - kh for the halo, bw for
// the bloom (haloSegment / bloomSegment in neon.frag are the same integrands
// up to a constant, which the ratio above cancels). In the angle
// theta = atan(t / c) both are elementary - the bloom's mass is d(theta), the
// halo's cos(theta) d(theta) - which is how every integral below is written:
// it keeps a difference of two nearly equal masses from cancelling when the
// fragment is far off to one side.
//
// A corner is developed at a rate other than 1 (see arcTangentSegment), so its
// arclength q sits at t1 + q / weight on that line. Along the arc, then, the
// kernel is the same kernel with width c * weight, centred on the foot
// q = -t1 * weight, and the corner's integral is a straight one in its own
// arclength.
//
// The coverage
// ------------
// An arc's coverage is a plateau of its intensity between two linear ramps -
// its feathers - so against either kernel it is closed form: each linear piece
// of it needs the kernel's mass and first moment over its span. A segment is a
// Gaussian bell, which has no closed form against either kernel, so it is
// integrated - see bellMass for the two quadratures and the accuracy they were
// picked for.
//
// Units and output
// ----------------
// Every length here is a FRACTION OF THE PERIMETER, so one table serves both
// resolution paths: neon.frag's pixel lengths all scale with
// uResolutionScale, the perimeter included, and their ratios do not move.
//
// Output: (.r halo, .g bloom) arc coverage x intensity and (.b halo, .a bloom)
// segment boost x bell, each encoded c / (1 + c) like the gather buffer -
// intensity and boost are unbounded, and the RGBA8 fallback would otherwise
// clamp them at 1.
//
// Two targets (uBakeTarget). 0 is the table neon.frag reads, through
// uGlowCoverLayout - every channel above. 1 is the SEGMENT TABLE: the
// segments' two channels alone, on .r / .g of a narrower table of their own
// (uGlowCoverLayout is then ITS layout), since a bell is far wider than an
// arc's feather and needs far fewer columns. neon-glow-cover-fill.frag copies
// it into the main table's .b / .a. Either way a texel is decoded through the
// layout and split it is given, so the integrals below never know which.
// ---------------------------------------------------------------------------

out vec4 fragColor;

// Same layout and binding as neon.frag's - see NeonRenderer::packLightBlocks.
layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// Same layout and binding as neon-common.glsl's.
layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

uniform float uHeadFeather;    ///< HEAD_FEATHER_PX as a fraction of the perimeter.
uniform float uTailFeather;    ///< TAIL_FEATHER_PX as a fraction of the perimeter.
uniform float uHaloWidth;      ///< kh as a fraction of the perimeter; also the layout's length scale.
uniform float uBloomWidth;     ///< bw as a fraction of the perimeter.
uniform vec2  uStraightSize;   ///< The horizontal and vertical straights' lengths, fractions of the perimeter.
uniform float uRadius;         ///< The corner radius as a fraction of the perimeter.
uniform int   uWinding;        ///< 0 = CLOCKWISE, 1 = COUNTER_CLOCKWISE, as neon.frag's.
uniform vec2  uGlowCoverSplit; ///< Each band's straight interior columns, as neon.frag's (glowCoverInner).
uniform vec4  uGlowCoverLayout; ///< The table's layout, as neon.frag's: columns, overhangs, shared (neon-pieces.glsl).
uniform int   uBakeArcs;       ///< 1 to integrate the arcs (.r / .g); 0 when only the segments changed - those channels are masked off.
uniform int   uBakeSegments;   ///< 1 to integrate the segments (.b / .a); 0 when only the arcs changed, likewise.
uniform int   uBakeTarget;     ///< 0 the main table (all four channels); 1 the segment table (the segments' two, on .r / .g).

// See neon.frag's copies: the abutment bits pick each endpoint's feather
// direction. Values are 0..7, all exact in a float.
float coverTailAbut(float flags) { return mod(floor(flags * 0.5), 256.0) / 255.0; }
float coverHeadAbut(float flags) { return floor(flags / 512.0) / 255.0; }

// --- Where each piece is on the perimeter ------------------------------
// (.x) the perimeter position of a piece's t1 end - the end haloSegment's t1
// and arcTangentSegment's +x end measure from - and (.y) +1 if perimeter
// position grows toward its t2 end, -1 if it shrinks. `cwStart` / `cwSigma`
// are the CLOCKWISE values; neon.frag's perimeterAt lays
// COUNTER_CLOCKWISE out over the same geometry the other way round from the
// top-left tangent point, which is s -> 1 - arcLen - s. Checked against
// neon.frag's perimeter map (then perimeterPosition, whose layout perimeterAt
// keeps) for every piece, both windings, at four shapes.
vec2 pieceStart(float cwStart, float cwSigma, float arcLen) {
    if (uWinding == 0) {
        return vec2(cwStart, cwSigma);
    }
    float s = 1.0 - arcLen - cwStart;
    return vec2(s - floor(s), -cwSigma);
}

// The straight in band `band` (GLOW_COVER_BAND_*), and the corner arc in
// quadrant `signs` (+y the top edge, as neon.frag's pieces have it), whose t1 end
// is the tangent point on the vertical edge.
vec2 straightStart(int band, float ws, float hs, float arcLen) {
    if (band == GLOW_COVER_BAND_NEG_X) {
        return pieceStart(2.0 * ws + 3.0 * arcLen + hs, 1.0, arcLen);
    }
    if (band == GLOW_COVER_BAND_POS_X) {
        return pieceStart(ws + arcLen + hs, -1.0, arcLen);
    }
    if (band == GLOW_COVER_BAND_NEG_Y) {
        return pieceStart(2.0 * ws + 2.0 * arcLen + hs, -1.0, arcLen);
    }
    return pieceStart(0.0, 1.0, arcLen);
}
vec2 cornerStart(vec2 signs, float ws, float hs, float arcLen) {
    float s = (signs.x > 0.0) ? ws + arcLen + ((signs.y > 0.0) ? 0.0 : hs)
                              : 2.0 * ws + 3.0 * arcLen + hs + ((signs.y > 0.0) ? hs : 0.0);
    return pieceStart(s, -signs.x * signs.y, arcLen);
}

// --- The kernels -------------------------------------------------------
// ln((t1^2 + c^2) / (t0^2 + c^2)), to full precision where the ratio is close
// to 1 - there it is 2 atanh(z), z = (t1 - t0)(t1 + t0) / (t1^2 + t0^2 +
// 2 c^2), formed from the t's so c^2 never cancels, by its series - and the
// plain log of the ratio elsewhere, where that is exact and z, near 1, is not.
float logRatio(float t0, float t1, float c) {
    float a = t0 * t0 + c * c;
    float b = t1 * t1 + c * c;
    float z = (t1 - t0) * (t1 + t0) / (a + b);
    if (abs(z) < 0.1) {
        float z2 = z * z;
        return 2.0 * z * (1.0 + z2 * (1.0 / 3.0 + z2 * (0.2 + z2 * (1.0 / 7.0))));
    }
    return log(b / a);
}

// Mass (.x) and first moment (.y) of one kernel over t in [t0, t1], both up to
// the kernel's constant factor. In theta = atan(t / c):
//
//     bloom:  mass = theta1 - theta0     moment = c ln(cos theta0 / cos theta1)
//     halo:   mass = sin1 - sin0         moment = c (cos theta0 - cos theta1)
//
// each written so a span far off to one side, where both angles are near
// +-PI/2, keeps its precision: the bloom's angle difference in one atan, the
// halo's two terms algebraically (cos theta = c / R). The bloom's moment
// is the same care: as a plain log of (t1^2 + c^2) / (t0^2 + c^2) it lost
// three digits wherever c dwarfs the span - a corner seen from near its centre
// of curvature, where the developed kernel is 10^5 px wide - and linearMass
// subtracts it from a near-equal term. logRatio keeps them.
vec2 kernelMoments(float t0, float t1, float c, bool halo) {
    if (t1 <= t0) {
        return vec2(0.0);
    }
    if (halo) {
        // sin theta = t / R, R = sqrt(t^2 + c^2). A difference of two of them
        // cancels when both are near +-1 - a span far off to one side - so
        // there each is taken as its distance from +-1, c^2 / (R (R + |t|)),
        // and a span over the foot sums two positive terms. No atan: this pass
        // re-bakes every frame under an animation, and three atans here were
        // most of an arc's cost.
        float c2     = c * c;
        float r0     = sqrt(t0 * t0 + c2);
        float r1     = sqrt(t1 * t1 + c2);
        float mass   = (t0 >= 0.0) ? c2 / (r0 * (r0 + t0)) - c2 / (r1 * (r1 + t1))
                     : (t1 <= 0.0) ? c2 / (r1 * (r1 - t1)) - c2 / (r0 * (r0 - t0))
                                   : t1 / r1 - t0 / r0;
        return vec2(mass, c2 * (t1 - t0) * (t1 + t0) / (r0 * r1 * (r0 + r1)));
    }
    return vec2(atan(c * (t1 - t0), c * c + t0 * t1), 0.5 * c * logRatio(t0, t1, c));
}

// Both kernels at once - halo (.x, width cH) and bloom (.y, width cB) - over
// the same span, so each integral below has one copy in the program. Every
// copy is inlined, and the program's compile time follows how many there are:
// see main().
vec2 kernelMass2(float t0, float t1, float cH, float cB) {
    return vec2(kernelMoments(t0, t1, cH, true).x, kernelMoments(t0, t1, cB, false).x);
}

// INTEGRAL over q in [q0, q1] of v(q) K(q - x), v linear from v0 at q0 to v1
// at q1 - one straight piece of an arc's coverage - written about the span's
// own midpoint, so the moment term is the small centred one.
vec2 linearMass2(float q0, float q1, float v0, float v1, float x, float cH, float cB) {
    vec2  kh = kernelMoments(q0 - x, q1 - x, cH, true);
    vec2  kb = kernelMoments(q0 - x, q1 - x, cB, false);
    float tm = 0.5 * (q0 + q1) - x;
    float vm = 0.5 * (v0 + v1);
    float s  = (v1 - v0) / (q1 - q0);
    return vec2(vm * kh.x + s * (kh.y - tm * kh.x), vm * kb.x + s * (kb.y - tm * kb.x));
}

// INTEGRAL over q in [0, len] of an arc image's coverage times K(q - x): a
// trapezoid rising from 0 at `a` to 1 at `b`, holding 1 to `c`, falling to 0 at
// `d`. Each of its three straight pieces is clipped to the piece of outline and
// skipped if that leaves nothing, so an arc that only reaches a piece with one
// end costs one or two spans. The feather caps (ARC_FEATHER_MAX_SHARE 0.4 of the
// arc each) keep b <= c, so the three never overlap.
//
// ONE linearMass2 in a loop, deliberately: the compiler builds every inlined
// copy of it, and this bake is compiled on the first frame that needs it, so
// its size is a first-frame cost a host sees. The loop's bound comes from the
// data (`spans` is 3 whenever the trapezoid has any width) so it is not
// unrolled back into three copies - measured, three inlined copies of the
// spans cost ~20 ms of compile on an AMD Radeon Pro 5300M.
vec2 trapezoidMass2(float a, float b, float c, float d, float len, float x, float cH, float cB) {
    vec2 mass  = vec2(0.0);
    int  spans = (d > a) ? 3 : 0;
    for (int i = 0; i < spans; i++) {
        float p0 = (i == 0) ? a : ((i == 1) ? b : c);
        float p1 = (i == 0) ? b : ((i == 1) ? c : d);
        float q0 = max(p0, 0.0);
        float q1 = min(p1, len);
        if (q1 <= q0) {
            continue;
        }
        // The coverage at each clipped end: rising, flat at 1, or falling.
        float v0 = (i == 0) ? (q0 - a) / (b - a) : ((i == 1) ? 1.0 : (d - q0) / (d - c));
        float v1 = (i == 0) ? (q1 - a) / (b - a) : ((i == 1) ? 1.0 : (d - q1) / (d - c));
        mass += linearMass2(q0, q1, v0, v1, x, cH, cB);
    }
    return mass;
}

// --- The arcs ----------------------------------------------------------
// The arcs' coverage x intensity along a piece that starts at perimeter
// position `place.x` and runs `len` in direction `place.y`, weighted by the
// halo's kernel (width cH) and the bloom's (cB) about the foot `x` - the ratio
// in the header, (.x halo, .y bloom). `total` is each kernel's mass over the
// whole piece, the ratio's denominator.
vec2 arcsOnPiece(vec2 place, float len, float x, float cH, float cB, vec2 total) {
    vec2  lit  = vec2(0.0);
    float most = 0.0;
    for (int i = 0; i < uArcCount; i++) {
        vec4 arc = uArcs[i];
        if (arc.z <= 0.0 || arc.y <= 1e-6) {
            continue;
        }
        most = max(most, arc.z);
        if (arc.y >= 1.0 - 1e-6) {
            lit += arc.z * total;
            continue;
        }
        // Each end's ramp, as arcCoverContinuous draws it: a free end feathers
        // inward, an abutting one outward, each capped at a share of the arc.
        // c0 / c1 are the ramps' centres, in perimeter position.
        float cap = arc.y * ARC_FEATHER_MAX_SHARE;
        float fT  = min(uTailFeather, cap);
        float fH  = min(uHeadFeather, cap);
        float c0  = arc.x + (0.5 - coverTailAbut(arc.w)) * fT;
        float c1  = arc.x + arc.y + (coverHeadAbut(arc.w) - 0.5) * fH;
        // The same, along the piece: the trapezoid's four corners, its rising
        // end first, swapped when the piece runs against the perimeter.
        float lo  = (place.y > 0.0) ? c0 - place.x : place.x - c1;
        float hi  = (place.y > 0.0) ? c1 - place.x : place.x - c0;
        float wLo = (place.y > 0.0) ? fT : fH;
        float wHi = (place.y > 0.0) ? fH : fT;
        float k   = floor(lo + 0.5);
        float qa  = lo - k - 0.5 * wLo;
        float qb  = lo - k + 0.5 * wLo;
        float qc  = hi - k - 0.5 * wHi;
        float qd  = hi - k + 0.5 * wHi;
        // Every image of the arc a whole perimeter along that reaches the
        // piece - qd + j > 0 and qa + j < len. Bounds from the data rather than
        // a fixed -1..1, so the compiler keeps this a loop instead of three
        // inlined copies; there is seldom more than one.
        int j0 = int(floor(-qd)) + 1;
        int j1 = int(ceil(len - qa)) - 1;
        for (int j = j0; j <= j1; j++) {
            float f = float(j);
            lit += arc.z * trapezoidMass2(qa + f, qb + f, qc + f, qd + f, len, x, cH, cB);
        }
    }
    // Overlapping arcs sum here where the emission takes their max.
    return min(max(lit / total, vec2(0.0)), vec2(most));
}

// --- The segments ------------------------------------------------------
// 16-point Gauss-Legendre on [-1, 1], positive half; the rule is symmetric.
// Not 8: measured against it (I61 in docs/review-findings.md), an 8-node rule
// read 3 levels off a long, bright segment - a bell spanning many kernel
// widths needs the nodes.
const vec4 GL16_X0 = vec4(0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438);
const vec4 GL16_X1 = vec4(0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499);
const vec4 GL16_W0 = vec4(0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767);
const vec4 GL16_W1 = vec4(0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541);

// INTEGRAL over q in [q0, q1] of bell(q) K(q - x), the bell exp(-((q - qb)
// sy)^2), in the kernel's units, for one kernel of width c. Gauss-Legendre in
// theta = atan((q - x) / c), where the kernel is flat (the bloom) or a cosine
// (the halo) and the bell is smooth whatever the two widths: a narrow kernel's
// peak, which no rule in q resolves, is spread over the whole range, and a wide
// kernel leaves the bell a smooth bump in a range a few radians across at
// most, [q0, q1] being the bell's support.
//
// The 16 nodes run in a loop whose bound comes from the data, so the compiler
// keeps one copy of the node rather than unrolling sixteen - the same
// first-frame compile cost as trapezoidMass2's.
float bellMass(float q0, float q1, float qb, float sy, float x, float c, bool halo) {
    float th0   = atan(q0 - x, c);
    float th1   = atan(q1 - x, c);
    float m     = 0.5 * (th0 + th1);
    float h     = 0.5 * (th1 - th0);
    float sum   = 0.0;
    int   nodes = (h > 0.0) ? 16 : 0;
    for (int i = 0; i < nodes; i++) {
        int   k  = i / 2;
        float n  = (k < 4) ? GL16_X0[k] : GL16_X1[k - 4];
        float wt = (k < 4) ? GL16_W0[k] : GL16_W1[k - 4];
        float th = m + (((i & 1) == 0) ? -n : n) * h;
        float d  = (x + c * tan(th) - qb) * sy;
        sum += wt * exp(-d * d) * (halo ? cos(th) : 1.0);
    }
    return sum * h;
}

// The segments' boost x bell along a piece, as arcsOnPiece has the arcs.
// Summed, as neon.frag's segCoverPt sums them, each from its nearest image as
// segCoverPt takes it: the bell is cut half a perimeter from its centre.
vec2 segmentsOnPiece(vec2 place, float len, float x, float cH, float cB, vec2 total) {
    vec2 lit = vec2(0.0);
    for (int i = 0; i < uSegmentCount; i++) {
        vec4 seg = uSegments[i];
        if (seg.z <= 0.0) {
            continue;
        }
        if (seg.y <= 1e-6) {
            lit += seg.z * total;
            continue;
        }
        float reach = min(GLOW_COVER_BELL_REACH / seg.y, 0.5);
        float qb    = (place.y > 0.0) ? seg.x - place.x : place.x - seg.x;
        qb -= floor(qb + 0.5);
        // Every image whose support reaches the piece, bounds from the data as
        // in arcsOnPiece.
        int j0 = int(floor(-reach - qb)) + 1;
        int j1 = int(ceil(len + reach - qb)) - 1;
        for (int j = j0; j <= j1; j++) {
            float centre = qb + float(j);
            float q0     = max(centre - reach, 0.0);
            float q1     = min(centre + reach, len);
            if (q1 <= q0) {
                continue;
            }
            // Both kernels through one call site, for the reason above.
            vec2 mass = vec2(0.0);
            int  kernels = (q1 > q0) ? 2 : 0;
            for (int k = 0; k < kernels; k++) {
                mass[k] = bellMass(q0, q1, centre, seg.y, x, (k == 0) ? cH : cB, k == 0);
            }
            lit += seg.z * mass;
        }
    }
    return max(lit / total, vec2(0.0));
}

// One piece's four numbers, in the output's channel order. Zero where the
// piece has no length - a straight of a circle - whose halo and bloom are zero
// in neon.frag anyway.
vec4 pieceCover(vec2 place, float len, float x, float cH, float cB) {
    vec2 total = kernelMass2(-x, len - x, cH, cB);
    if (total.x <= 0.0 || total.y <= 0.0) {
        return vec4(0.0);
    }
    // A piece's arc channels depend on the arcs and the geometry alone, its
    // segment channels on the segments alone, so a bake that only one kind of
    // light needs integrates that kind and writes its channels (the pass masks
    // the others off) - the rest already hold what this would write.
    vec2 arcs = (uBakeArcs != 0) ? arcsOnPiece(place, len, x, cH, cB, total) : vec2(0.0);
    vec2 segs = (uBakeSegments != 0) ? segmentsOnPiece(place, len, x, cH, cB, total) : vec2(0.0);
    return vec4(arcs.x, arcs.y, segs.x, segs.y);
}

void main() {
    float kh     = uHaloWidth;
    float bw     = uBloomWidth;
    float ws     = uStraightSize.x;
    float hs     = uStraightSize.y;
    float r      = uRadius;
    float arcLen = PIECES_HALF_PI * r;
    int   band;
    float row;
    float inner;
    float split;
    glowCoverTexel(gl_FragCoord.xy, uGlowCoverSplit, uGlowCoverLayout, band, row, inner, split);

    // This texel's piece, and up to two developments of it to weight
    // together. pieceCover - and, for a corner, the development - is called
    // from ONE place, a loop over them, and that is load-bearing: everything
    // below it is inlined at each call, the compiler builds every copy, and
    // this program is compiled on the first frame that needs it.
    vec2  place  = vec2(0.0);
    float len    = 0.0;
    int   n      = 0;
    bool  corner = false;
    vec2  xa     = vec2(0.0);
    vec2  w      = vec2(0.0);
    vec2  weight = vec2(1.0, 0.0);
    // The two directions a corner is developed about, when there are two.
    vec2  about0 = vec2(0.0);
    vec2  about1 = vec2(0.0);
    if (gl_FragCoord.x < split) {
        // A straight: the projection and the distance this texel holds.
        len = (band == GLOW_COVER_BAND_NEG_X || band == GLOW_COVER_BAND_POS_X) ? hs : ws;
        if (len > 0.0) {
            xa    = glowCoverStraightAt(gl_FragCoord.x, row, len, kh, inner, uGlowCoverLayout);
            place = straightStart(band, ws, hs, arcLen);
            n     = 1;
        }
    } else if (r > 0.0) {
        // A corner: the offset from its centre this texel holds, in
        // arcTangentSegment's frame.
        vec2 signs = vec2((band == 1 || band == 3) ? 1.0 : -1.0, (band >= 2) ? 1.0 : -1.0);
        w          = glowCoverCornerAt(gl_FragCoord.x - split, row, r, kh, inner, uGlowCoverLayout);
        place      = cornerStart(signs, ws, hs, arcLen);
        len        = arcLen;
        corner     = true;
        about0     = arcTangentDirection(w);
        n          = 1;
        if (w.x < 0.0 && w.y < 0.0) {
            // Behind the centre arcTangentSegment develops the arc about
            // whichever end is nearer, which flips across the diagonal, and with
            // it which end of the arc the foot is at. Its halo and bloom are
            // symmetric across that flip; the coverage is not. So both
            // developments are taken and swept between: all of the +y one where
            // this region meets w.x < 0 < w.y (which develops about +y alone),
            // all of the +x one where it meets w.y < 0 < w.x, and half each on
            // the diagonal - neon.frag's cornerFootAngle, before V21, swept the
            // same way for the same reason.
            float toX = 0.5 - 0.5 * (w.y - w.x) / max(-w.x - w.y, ARC_FRAME_EPSILON);
            about0    = vec2(0.0, 1.0);
            about1    = vec2(1.0, 0.0);
            weight    = vec2(1.0 - toX, toX);
            n         = 2;
        }
    }
    vec4 cover = vec4(0.0);
    for (int i = 0; i < n; i++) {
        float foot;
        float cH;
        float cB;
        if (corner) {
            // Arclength q sits at t1 + q / weight along the developed line, so
            // the foot is at -t1 * weight and each kernel is weight times as wide.
            vec4 seg = arcTangentSegmentAbout(w, r, (i == 0) ? about0 : about1);
            foot     = -seg.y * seg.w;
            cH       = seg.w * sqrt(seg.x * seg.x + kh * kh);
            cB       = seg.w * sqrt(seg.x * seg.x + bw * bw);
        } else {
            foot = xa.x;
            cH   = sqrt(xa.y * xa.y + kh * kh);
            cB   = sqrt(xa.y * xa.y + bw * bw);
        }
        cover += weight[i] * pieceCover(place, len, foot, cH, cB);
    }
    cover     = max(cover, vec4(0.0));
    // The segment table holds the segments' halo and bloom on .r / .g.
    cover     = (uBakeTarget == 1) ? vec4(cover.ba, 0.0, 0.0) : cover;
    // Into an RG16F table when the config has no segments (.b / .a are zero
    // then and are dropped), RGBA16F otherwise - see ensureGlowCoverBuffer.
    fragColor = cover / (1.0 + cover);
}

)shadersrc";

                static const char *const NEON_GLOW_COVER_FILL_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - the neon shaders (neon.frag, neon-emission.frag), where CMake
//     text-injects this file via @NEON_TUNING@ in shaders.h.in, and
//   - the C++ renderer (neon-renderer.cpp), which #includes it for the
//     glow-reach quad-sizing factors.
//
// The px constants below are written in FULL-RESOLUTION pixels. Where the
// renderer draws at a reduced NeonConfig::resolutionScale, the shader converts
// each one with uResolutionScale at the point of use; the notes on the
// individual constants say which need it and which do not.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a `const float X = 0.9f;` cannot
// be written once and stay valid in both languages. Plain #define is the only
// form that compiles identically as GLSL and as C++ from a single file.
// (For C++-only constants, prefer constexpr - macros are used here strictly
// because these values must also live inside the shaders.)
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
//
//     FILAMENT_MIN_HALF_WIDTH is a STATED-width floor: it is what makes
//     lineWidth 0 mean "no line" rather than a single bright dot, and it is
//     paired with lineGate, which fades the filament in over the same span. It
//     describes the width the caller asked for, so it is a FULL-RES px constant
//     and the shader converts it with uResolutionScale like every other one.
//
//     The SAMPLING floor is the pair below, and the distinction from the
//     stated floor is the whole point of having two of them. Below
//     resolutionScale 1.0 the gather rasterises into a reduced buffer and
//     neon-blit.frag bilinearly upsamples it, so a filament the buffer cannot
//     sample is not reconstructed - it is resampled, and what comes out
//     depends on where the rect edge happens to fall between buffer texel
//     centres. That is a property of the BUFFER, so these are in BUFFER px and
//     are NOT converted with uResolutionScale.
//
//     Getting that backwards is what made a 1 px line at scale 0.5 disagree
//     with the same line at 1.0. sigma was floored only by the converted
//     stated constant, which at scale 0.5 is 0.25 buffer px, so the line sat
//     under the buffer's Nyquist limit and its peak swung with the rect's
//     sub-pixel position - 217 on a texel centre, 180 half a texel off,
//     against a 1.0 reference that holds 230-241 at every position.
//
//     THE FLOOR IS NOT A FIXED HALF WIDTH. It was one - a flat 0.5 buffer px -
//     and a fixed half width asks the wrong question. What survives the blit
//     is not decided by the profile's width at half maximum; it is decided by
//     how much signal the NEIGHBOURING buffer texel still carries, because
//     that is what the bilinear filter rebuilds the peak from. So the floor
//     says exactly that: the profile must still be at
//     FILAMENT_NYQUIST_MIN_SHARE of its peak, FILAMENT_NYQUIST_SAMPLE_PX out.
//     Inverting the generalized Gaussian for sigma gives
//
//         sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
//
//     which neon.frag and NeonRenderer::setupGeometry both evaluate.
//
//     0.0625 and 1.0 are today's behaviour restated, not a retune: at the
//     DEFAULT falloff (N = 2) the expression is exactly 0.5 buffer px, the
//     constant it replaces, so nothing at or near the default moves.
//
//     The two questions diverge at a SOFT falloff, and that is what the change
//     is for. sigma does not only set the core - it multiplies reachSigmas, so
//     it sets the whole tail. At filamentFalloff 0.27 (N = 0.54) reachSigmas
//     clamps at FILAMENT_REACH_MAX_SIGMAS and the tail is 64 sigmas, so a flat
//     0.5 floor stretched a 31 px filament to 61 px at scale 0.5 and past
//     120 px at 0.25 - to buy 5 levels of peak accuracy (max per-phase error
//     13 -> 8) on a profile 31 px wide that the buffer was sampling perfectly
//     well. The share form asks for 0.082 buffer px there, below what the
//     caller already supplies, so it does not engage.
//
//     Where it DOES still engage it is doing the work it was added for: at the
//     default falloff, without any floor, the peak collapses to 153 against a
//     235 reference, and at filamentFalloff 1.5 to 24. The known cost of the
//     share form is the band around filamentFalloff 0.4 to 0.5, where it stops
//     engaging while the unfloored peak error is still 12 to 18 levels;
//     tightening MIN_SHARE to cover that would stop the default case being
//     bit-identical, so the crossover sits here deliberately. See
//     docs/corner-crease-and-filament-nyquist.md section 2.8.
//
//     The line does read WIDER at a reduced scale wherever the floor engages,
//     and that part is not a defect to tune away: a line the buffer cannot
//     sample cannot be reconstructed from it. The floor buys a stable,
//     correctly-bright line of the narrowest width the buffer can actually
//     carry. Deliberately NOT paired with an amplitude compensation to
//     conserve the line's integral - the grade at the end of neon.frag
//     tonemaps per-fragment INSIDE the buffer and FILAMENT_GAIN puts the core
//     deep in saturation, so scaling the linear amplitude barely moves the
//     8-bit value while it does measurably dim the peak (error 15-23/255
//     against 0-10 without it). See
//     docs/corner-crease-and-filament-nyquist.md section 2.
//
//     GATED to scales below 1.0 in both consumers. At scale 1.0 the reduced
//     buffer is full size and the blit copies it 1:1, so there is nothing to
//     survive, and the ring always shades at 1.0. The old flat constant was a
//     no-op at 1.0 by arithmetic
//     coincidence - it equalled the converted stated floor; this expression
//     would not be, above N = 2, so the gate is explicit now. ---
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_NYQUIST_SAMPLE_PX 1.0
#define FILAMENT_NYQUIST_MIN_SHARE 0.0625
#define FILAMENT_GAIN             12.0

// --- Continuous-arc filament gate feathers (neon.frag).
//     INWARD FEATHER: the smooth ramp sits INSIDE the arc's own perimeter
//     span, so nothing outside the arc gets lit -> no corner bleed regardless
//     of width, no perpendicular spike, and the profile is a plain smoothstep
//     that reads the same on straight edges and at corners. Trade-off: the
//     visible arc appears inset by these widths (arc lights up at
//     start + TAIL_FEATHER_PX and ends at start + length - HEAD_FEATHER_PX).
//     Values are pixel-space spans, divided by the current perimeter at the
//     call site, and capped per-arc by ARC_FEATHER_MAX_SHARE below. ---
#define HEAD_FEATHER_PX           14.0
#define TAIL_FEATHER_PX           14.0

// --- Cap on each feather, as a share of the arc's own length.
//
//     The feathers above are a fixed pixel span while an arc's length is a
//     FRACTION of the perimeter, so the same arc config is a different pixel
//     length on every rect. Below ~(HEAD + TAIL) px the two ramps overlapped
//     and ate into the arc's peak: an L = 0.02 arc peaked at 1.00 on an
//     800x600 rect but only 0.21 on a 200x150 one - the last place rect size
//     still reached brightness. Capping each feather at a share of the arc
//     length holds the peak at exactly 1.0 for any length at any size; a short
//     arc gets a proportionally shorter ramp instead of a truncated top.
//
//     0.4 leaves a 0.2 * length plateau at full brightness between the two
//     ramps. Up to 0.5 also holds the peak, but with no plateau the arc reads
//     as a spike rather than a flat-topped segment. Arcs longer than
//     (HEAD + TAIL) / 0.4 px never reach the cap, so the normal case is
//     bit-identical to before. ---
#define ARC_FEATHER_MAX_SHARE     0.4

// --- Halo (sharp coloured glow).
//
//     The halo and bloom are evaluated ANALYTICALLY from the rounded-box SDF
//     distance, not summed over the perimeter gather. For a locally straight
//     emitter of unit density the old sums converge exactly to:
//
//       sum g*sqrt(g) * spacing*kh^2*HALO_NORM  -> HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
//       sum 1/(dd+bw^2) * spacing*bw*BLOOM_NORM -> BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
//
//     so the NORM factors keep their meaning and their calibration: peak
//     values at ad = 0 are unchanged (0.86 and 1.005 respectively).
//
//     The closed form is what makes the effect geometry-independent. The
//     gather had to floor its kernel at a multiple of sampleSpacing to stop
//     128 discrete samples beading into dots, and sampleSpacing is
//     perimeter / NEON_MAX_LOOP_SAMPLES - so halo width, and via the gate its
//     brightness, both tracked the rect size, and glowRadius did nothing at
//     all until it exceeded the floor (~56 px on a 1920x1080 rect, i.e. most
//     of its usable range). An analytic profile cannot bead at any radius, so
//     no floor is needed and glowRadius sets the width directly.
//
//     Those two limits are of an INFINITE emitter, and evaluating one of them
//     at ad = the nearest-edge distance is what used to produce the interior
//     medial-axis creases - the dark wedges running in from each corner, plus
//     the central spine. A fragment on a corner diagonal has two edges equally
//     near and was lit by exactly one of them: measured against a fragment with
//     a single edge at the same distance, the ratio was 1.000 at every distance
//     tested, where physics says roughly 2.
//
//     neon.frag now sums the FINITE-SEGMENT form of the same two integrals over
//     the four straight edges instead (haloSegment / bloomSegment). Integrating
//     the same kernels from t1 to t2 along a segment at perpendicular distance
//     a gives elementary antiderivatives, and as t1 -> -inf, t2 -> +inf each one
//     reduces to exactly the limit above - so this is a strict generalisation
//     and the NORM factors keep the calibration they already had. The corner
//     sweep went from a V kinked at exactly 45 degrees (21 levels deep at
//     r = 160) to a smooth basin of 12, which is genuine falloff rather than a
//     crease. Cost is ~1.06-1.10x of the neon frame; the gather loop is still
//     the overwhelming majority of it.
//
//     The CREASE is what the four-segment form was adopted for. It is not the
//     largest thing it moves. The old expressions were functions of ad and of
//     nothing else, so the old profile was SYMMETRIC about the line - a
//     fragment 120 px inside the edge and one 120 px outside were lit
//     identically, by construction. Outside, the emitter recedes; inside, it
//     wraps around you. The segment sum knows the difference, so the exterior
//     now falls off slightly faster (mid-edge peak itself moves by 2/255) and
//     the INTERIOR stops falling off past roughly one rect-half and settles on
//     a floor. On the 1000x500 / glowRadius 60 probe the centre goes 162 ->
//     186 and the interior mean 174.5 -> 188.8; at glowRadius 30 on 800x400 it
//     is 81 -> 113. The glow reads as bigger, and the part that grew is the
//     interior - the exterior mean goes slightly DOWN.
//
//     Intended, and not tunable back out: a real rectangular tube does light
//     its own interior, and HALO_GAIN cannot be lowered to undo it without
//     dimming the line with it. The knobs for a perimeter-hugging glow are the
//     ones that always meant that - insideCutoff, and glowSide OUTSIDE.
//
//     THE CORNERS ARE A FIFTH THROUGH EIGHTH SEGMENT, not an extension of the
//     four. The straights are trimmed to their TANGENT POINTS, and each quarter
//     arc contributes one more segment: the arc DEVELOPED onto its own tangent
//     at whichever arc point is nearest the fragment, carrying the arc's full
//     length and split about that point, so it abuts the straights exactly in
//     arclength and the emitter is continuous. See neon.frag's
//     arcTangentSegment.
//
//     DEVELOPED AT RATE sqrt(rho * min(rho, r)), NOT AT RATE r, with the
//     measure put back by scaling the segment by r/rate - `rho` being the
//     fragment's distance from the arc centre. Laying an arc out one unit of
//     tangent per unit of arc is only right for a fragment ON it; the exact
//     distance to the point dphi away is a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,
//     whose slope at the foot is sqrt(rho*r). Rate r cost two things:
//
//       - at the CENTRE OF CURVATURE the whole arc ran off to one side of the
//         foot, and both kernels peak at t = 0, so it under-counted - to 54%
//         of the true value on a circle, where all four centres coincide at
//         the middle of the shape;
//       - and it CREASED, because the arc's endpoints slide at the tangent
//         rate on one side of the clamp and at rate 1 on the other, which
//         agree only where the rate is length(w). Every other point of the two
//         lines through an arc centre carried a C1 kink: an L-shaped seam per
//         corner, and on a circle an unmistakable dark cross. See
//         docs/review-findings.md V12.
//
//     rho == r gives rate r and weight 1, so a fragment on the tube is
//     bit-identical to the rate-r form and this calibration is untouched. The
//     whole emitter's worst error against a numerically integrated perimeter
//     falls on every geometry tested (16.5 -> 9.8% on a 600x400 r=40,
//     37.3 -> 26.3% on a circle).
//
//     The straights used to run to the SHARP corner instead, on the grounds
//     that over-extending them past the tangent point stood in for the arc that
//     has no elementary closed form. It does not stand in for it. The
//     over-extension is a phantom emitter a few px from a fragment that is tens
//     of px from the real tube: on an 800x400 rect at cornerRadius 40, a point
//     20 px outside the arc on the diagonal sits 2.4 px from EACH of the two
//     phantoms, and rendered 98 -> 146 across the commit that introduced them.
//     Against a numerically integrated rounded-rect perimeter it was 40 levels
//     too dark ON the tube and 50 too bright just outside it, which is what
//     made a rounded corner read square at a narrow glowRadius.
//
//     The developed arc costs one extra segment per corner - one atan and a
//     length on top - and takes the worst error over a quadrant from 76 to 6
//     levels at glowRadius 5, 112 to 6 at cornerRadius 120, 130 to 8 at 200.
//     A full circle (cornerRadius == halfMin) is still the residual case,
//     since its perimeter is four developed arcs and no straights at all.
//
//     Gated on uCornerRadius > 0, which is a UNIFORM - safe branching, and the
//     reason a sharp rect pays nothing and stays bit-identical (nine scenes
//     cmp-equal, not argued, and still cmp-equal after the rate fix above).
//     Rounded costs 1.19x of the neon pass against no arcs at all, and the rate
//     fix adds 1.03x on top - but the sharp path pays 1.026x of THAT for code
//     it never runs, because what decides this block is REGISTER PRESSURE, not
//     structure: with a per-arc bloom pedestal it was heavy enough to cost the
//     sharp path 1.14x. Re-time a cornerRadius 0 scene as well as a rounded one
//     after touching this. See docs/corner-crease-and-filament-nyquist.md
//     sections 1.7 to 1.9, and docs/review-findings.md V4, V10 and V12. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Width of the COLOUR gather kernel, as a FRACTION OF THE PERIMETER.
//
//     The perimeter colour blend is the one part of the shader that is still a
//     discrete sum over the loop samples, and it is the only place a length has
//     to be expressed this way rather than in pixels. The reason is that the
//     signal it filters - the gradient LUT - is itself parameterised by
//     perimeter fraction: sample i contributes LUT(i / NEON_MAX_LOOP_SAMPLES).
//     A kernel measured in pixels therefore covers a DIFFERENT span of the
//     gradient on every geometry, and the same colour stops render washed out
//     on a small rect and crisp on a large one. Measured on the stock 4-stop
//     ring, the colour sampled exactly on the red stop ran (0.95, 0.33, 0.06)
//     at 200x150 against (0.96, 0.04, 0.00) at 2800x2200 - roughly 8x the hue
//     bleed from geometry alone. Scaling the kernel with the perimeter is what
//     makes the gradient read identically at any size.
//
//     Two further properties fall out of the same choice:
//
//       - A constant anti-bead margin. The kernel has to stay >= about one
//         sample spacing, or it collapses between samples and the blend beads
//         into dots. Spacing is perimeter / NEON_MAX_LOOP_SAMPLES, so a
//         perimeter fraction pins the ratio at
//         COLOR_BLEND_PERIM_FRAC * NEON_MAX_LOOP_SAMPLES = 1.13 spacings on
//         every geometry, where the previous fixed 24 px span met the bound
//         only up to a ~3000 px perimeter and was down to 0.31 spacings by
//         9900 px. (No beading was actually measurable there - a DFT of the
//         hue around the perimeter put the 128-cycle component at the noise
//         floor - because the gradient varies slowly over one spacing on a
//         rect that large. The margin is a guarantee for dense colour stops,
//         not a fix for an observed artifact.)
//
//       - Sample-count independence. The kernel comes from the
//         NEON_MAX_LOOP_SAMPLES-based fraction rather than from the runtime
//         sample count, so the numSamples knob does not move the colour
//         blend. (An earlier sampleSpacing-derived floor divided by the live
//         count and landed 2x wider at reduced counts.)
//
//     Deliberately NOT coupled to glowRadius. The gather produces colour only -
//     the halo and bloom have been closed-form since they stopped riding on it -
//     so a wide glow has no reason to desaturate the gradient, and the old
//     max(glowRadius, floor) reintroduced exactly the pixel-space dependence
//     this constant exists to remove.
//
//     0.0088 puts the kernel at 24.0 px on the stock 800x600 / radius 40
//     geometry, matching the previous fixed span, so default-sized output is
//     unchanged.
//
//     NOTE: unit-free, unlike the px constants around it. The shader
//     multiplies it by a perimeter that is already in scaled px, so it needs
//     no uResolutionScale correction - applying one would double-apply. ---
#define COLOR_BLEND_PERIM_FRAC    0.0088

// --- Emission on/off ramp. glowRadius = 0 must read as "filament only", but
//     an analytic profile at radius 0 is a sub-pixel spike of full height
//     rather than nothing, so the halo and bloom fade in over
//     glowRadius = [0, this]. A FIXED pixel width: gating against the
//     sampleSpacing-derived floor instead would re-couple brightness to the
//     rect size, which is the whole thing this design removes.
//
//     NOTE: full-res pixel span, compared against a uGlowRadius that arrives
//     already scaled, so the shader multiplies this by uResolutionScale at
//     the point of use. Identity at scale 1.0. ---
#define GLOW_GATE_FADE_PX         2.0

// --- Lower bound on the analytic emission widths. Guards the divides only;
//     anything this small is already multiplied out by GLOW_GATE_FADE_PX. ---
#define EMISSION_MIN_WIDTH        1e-3

// --- Degenerate guard for the corner arc's tangent frame (neon.frag,
//     arcTangentSegment). The frame is built by normalising the fragment's
//     offset from the arc centre, clamped into the quarter the arc occupies;
//     that offset is the zero vector only for a fragment sitting exactly on an
//     arc centre, where the direction is undefined and atan(0, 0) is undefined
//     with it. Below this the frame falls back to the nearer of the two tangent
//     points, which is the correct clamp for the whole region the guard covers.
//
//     It floors `rho` in the same function, for the same fragment: the
//     development rate divides into the segment's weight, so an exactly-zero
//     rho would be a division by zero. The floored value is nowhere near a
//     pixel and the limit it lands on is the exact answer anyway, so this only
//     keeps a NaN out.
//
//     Unit-free: compared against a length in the shader's own px space, and
//     small enough that nothing but the exact degeneracy reaches it. ---
#define ARC_FRAME_EPSILON         1e-6

// --- Bloom (wide background spill). See the halo note above for the closed
//     form; BLOOM_SPACING_FLOOR is gone with the gather. ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_NORM_FACTOR         0.32

// --- Travelling-segment array size (shared by C++ vector cap + GLSL uniform array) ---
#define MAX_SEGMENT_BOOSTS        8

// --- Arc range array size (shared by C++ vector cap + GLSL uniform array).
//     Each arc gates a slice of the perimeter with its own colour + intensity.
//     Overlap resolves winner-take-all (max mask*intensity wins). ---
#define MAX_ARCS                  8

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO and
//     the shader's array. The gather iterates only uNumSamples of them
//     (NeonConfig::numSamples, which defaults to this), so it is a ceiling,
//     not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Glow coverage table (neon-glow-cover.frag, V20 and V21): for each piece
//     of the emitter and each fragment position round it, the arcs' and the
//     segments' coverage of that piece as the halo and the bloom see it. Laid
//     out in neon-pieces.glsl: four bands of ROWS rows, each holding one
//     straight at its left and one corner at its right. A band's columns are
//     shared between the two in proportion to their lengths (uGlowCoverSplit,
//     NeonRenderer::GetGlowCoverSplit), so a circle's corners get the columns
//     its straights do not need and a long rect's straights get its corners':
//     the straight has OVERHANG columns past each end, the corner
//     CORNER_OVERHANG on each side of its arc and a guard texel at each end of
//     its block, and the columns left over (the layout's `shared`) are split
//     between the two interiors, each keeping at least MIN_INTERIOR. ROWS 32 is half of them
//     within one halo width of a straight, and 16 inside and 16 outside a
//     corner's arc. 1024 x 128 RGBA16F is 1.0 MB - half V20's single table per
//     perimeter. Sizing it, against an exact per-piece reference (V21 in
//     docs/review-findings.md): 48 rows (1.5 MB) read at most 2 levels off where
//     32 reads 3 on a few pixels of a segment scene, for 1.3x the bake's cost
//     under animation; 768 columns read 6 levels off a 1 px halo on a 4K-sized
//     rect.
//
//     The WIDTH is not a constant: the table's layout is a parameter
//     (NeonRenderer's GlowCoverLayout, the shaders' uGlowCoverLayout), between
//     MIN_WIDTH and MAX_WIDTH in steps of WIDTH_STEP. ROWS and HEIGHT are. ---
#define GLOW_COVER_MAX_WIDTH      1024
#define GLOW_COVER_MIN_WIDTH      256
#define GLOW_COVER_WIDTH_STEP     64
#define GLOW_COVER_ROWS           32
#define GLOW_COVER_OVERHANG       64
#define GLOW_COVER_CORNER_OVERHANG 32
#define GLOW_COVER_MIN_INTERIOR   16
#define GLOW_COVER_HEIGHT         (4 * GLOW_COVER_ROWS)

// --- The glow coverage table's width (NeonRenderer's GetGlowCoverWidth):
//     enough interior columns that the longest band - its straight plus its
//     quarter arc, L full-res px - gets one every S px, S growing with the
//     halo width kh (max(glowRadius, EMISSION_MIN_WIDTH), px):
//
//         S    = clamp(PX_PER_COLUMN_PER_KH * kh, MIN_PX_PER_COLUMN, MAX_PX_PER_COLUMN)
//         need = 2 + 2 OVERHANG + 2 CORNER_OVERHANG + ceil(L / S)
//         W    = clamp(need rounded up to WIDTH_STEP, MIN_WIDTH, MAX_WIDTH)
//
//     In full-res px because the edge ring reads the table at full
//     resolution; lengths enter the layout only as ratios, so every scale
//     shares one table.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the 1024-wide
//     table (docs/neon-glow-cover-resolution-plan.md step 2): 250 configs -
//     rects 200 x 120 to 3600 x 2000, glowRadius 1 / 2 / 5 / 10 / 20,
//     cornerRadius 0 / 40 / a circle, both windings, three partial arcs with
//     and without colour stops and with two segments, at scales 1.0 / 0.5 /
//     0.25 - each at every width from 256 to 960, the frame after a light
//     change, against the same frame at 1024. The widest spacing that still
//     met the plan's criterion (max 2 levels, 99.9% of lit pixels within 1)
//     on every config was ~2.2 px per column at kh 1, 2.5 at 2, 4.6 at 5, 6.8
//     at 10 and 10 at 20. These constants sit under that envelope - 12% at
//     kh 5, 27% or more elsewhere - and at the widths they give, every config
//     read at most 2 levels with at least 99.988% of lit pixels within 1.
//     Spacing 20% wider failed at kh 5 (4 levels on a 200 px circle). A
//     narrow glow on a large rect still gets MAX_WIDTH: at kh 1-2 a
//     3600 x 2000 rect needed all 1024 columns. ---
#define GLOW_COVER_PX_PER_COLUMN_PER_KH 0.8
#define GLOW_COVER_MIN_PX_PER_COLUMN    1.6
#define GLOW_COVER_MAX_PX_PER_COLUMN    5.0

// --- The segment table (NeonRenderer's GetGlowCoverSegmentWidth,
//     neon-glow-cover-fill.frag): the segments' two channels of the glow
//     coverage table, baked at a width of their own and copied in. A bell's
//     standard deviation is 0.3536 x its length x the perimeter - ~100 px for
//     a length-0.05 segment on a 1840 x 1000 rect, against an arc's 14 px
//     feather - so it needs far fewer columns than the arcs:
//
//         sigma = 0.3536 x the shortest lit segment's length x perimeter   (full-res px)
//         need  = 2 + 2 SEG_OVERHANG + 2 SEG_CORNER_OVERHANG + ceil(L / (sigma / SEG_COLUMNS_PER_SIGMA))
//         W     = clamp(need rounded up to WIDTH_STEP, SEG_MIN_WIDTH, the main table's width)
//
//     with L the longest band, as for the main table. Where W would be at
//     least SEG_DIRECT_SHARE of the main table's width (very short segments)
//     the separate table saves nothing, and the segments are baked into the
//     main table directly, as before it existed.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the segments
//     baked into the main table directly (plan step 3): 135 configs - rects
//     200 x 120 to 3600 x 2000, glowRadius 2 / 5 / 10, cornerRadius 0 / 40 /
//     a circle, scales 1.0 / 0.5 / 0.25, segments of length 0.01-0.2 with
//     boosts 0.5-2, overlapping, abutting, over the corners, travelling, and
//     under a partial arc - the frame after a light change, the segment table
//     forced on. With overhangs 16 / 8, every COLUMNS_PER_SIGMA from 3 to 8
//     read at most 2 levels with 99.9992% of lit pixels within 1; 2 read 3
//     levels on a thin glow at 3840 x 2160. 4 is one step of margin. Overhangs
//     8 / 4 held at every value tried (99.988%), 32 / 16 read at most 1 level
//     from 4 up but do not fit SEG_MIN_WIDTH. ---
#define GLOW_COVER_SEG_COLUMNS_PER_SIGMA 4.0

// --- How far the glow coverage bake integrates a segment's bell either side
//     of its centre, in units of 1 / invSigma (the bell is exp(-(d invSigma)^2),
//     so 3 is 4.24 standard deviations, where it has fallen to 1.2e-4 of its
//     peak). The bake (neon-glow-cover.frag, segmentsOnPiece) cuts the bell
//     here and NeonRenderer's GetGlowCoverDirtySegmentPieces mirrors it to
//     find the pieces a moved segment reaches: change one and the other
//     follows, or a piece the bell reaches is not re-baked. It was 5 / sqrt(2)
//     (5 sigma); at 3 a moved segment dirties fewer pieces, which took the
//     segment table's bake on a band of 8 segments from 0.75 to 0.58 (timer
//     units, AMD Radeon Pro 5300M), at most 1 level, on up to 1.6% of a
//     frame's lit pixels (0.15% on average over the cover set; I61 in
//     docs/review-findings.md). An 8-node rule on top halved it again but
//     read 3 levels off a long, bright segment, and was not taken. ---
#define GLOW_COVER_BELL_REACH            3.0
#define GLOW_COVER_SEG_OVERHANG          16
#define GLOW_COVER_SEG_CORNER_OVERHANG   8
#define GLOW_COVER_SEG_MIN_WIDTH         128
#define GLOW_COVER_SEG_DIRECT_SHARE      0.75

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- The output dither (R7), peak to peak in 8-bit levels: neonDither
//     (neon-grade.glsl) adds noise in +/- half this to every colour the neon
//     writes to the caller's framebuffer - the blit, the edge ring and the
//     ring's field composite, never an offscreen buffer. The halo and bloom
//     are slow gradients that cross a level only every few px, up to tens of
//     px in the dark tail, so an undithered write draws concentric contour
//     rings around the rect. 1.0 is the least that removes them; 0 turns the
//     dither off. It assumes an 8-bit target. ---
#define OUTPUT_DITHER_LSB         1.0

// --- Epsilons ---
#define WSUM_EPSILON              1e-6

// --- One-sided cut guard band, in BUFFER pixels. Scaled path only.
//
//     One of the two constants in this file stated in buffer px rather than
//     full-res px (BLIT_CUTOFF_GUARD_PX below is the other): it describes the
//     BUFFER's own sampling, so it must NOT be converted with uResolutionScale.
//
//     For pass 1b the one-sided cut is not applied by neon.frag at all -
//     neon-blit.frag applies it at DESTINATION resolution, where a pixel is a
//     pixel and the edge can land exactly where the full-resolution shading
//     puts it. What neon.frag still does is cull the dark side, and the bound
//     it culls at cannot be the cut itself: the blit reconstructs each
//     destination pixel from the 2x2 buffer texels around it, so a texel
//     killed at the cut leaves the first LIT destination pixel rebuilt partly
//     from black. Measured at scale 0.5, glowSide OUTSIDE: the first lit pixel
//     came back at 178 against the 239 full resolution puts there, a dark seam
//     hugging the inside of the glow's own edge.
//
//     So the cull runs this far PAST the cut, and the guard band's emission is
//     what the blit's filter reconstructs the boundary from. One buffer texel
//     is the filter's reach; 2.0 covers that with room for the diagonal, where
//     fwidth of an SDF is sqrt(2) rather than 1, and for the half-texel of
//     phase between the cut and the nearest texel centre.
//
//     The band costs a two-texel strip along the perimeter - nothing beside
//     the half-plane the cull still removes - and none of it is visible: the
//     blit's mask is zero everywhere past the cut, including at the hard step
//     where this bound finally discards.
//
//     NOT used by the edge ring, which owns its own cut (uBlitOwnsCut 0). See
//     neon.frag's sideCull and the post-grade cut block. ---
#define BLIT_SIDE_GUARD_PX        2.0

// --- Cutoff guard band, in BUFFER pixels. Pass 1b only.
//
//     BLIT_SIDE_GUARD_PX's argument, applied to the inside/outside cutoffs.
//     For pass 1b neon-blit.frag applies the cutoff masks at
//     DESTINATION resolution, and neon.frag only culls - this far past the end
//     of each ramp, so the blit's bilinear filter rebuilds every boundary from
//     lit texels rather than from black. Same value as the side guard, for the
//     same reasons: one buffer texel of filter reach, plus room for the
//     diagonal and for the phase between the boundary and the nearest texel
//     centre.
//
//     It replaces CUTOFF_SOFT_FLOOR_PX, a feather in buffer px that let a mask
//     drawn INTO the buffer place its boundary sub-texel. That was the best the
//     buffer could do, and it still softened the boundary across a buffer
//     texel and snapped it toward the buffer grid: on the bounded_band scene of
//     docs/neon-resolution-scale-comparison.html it read p99 41/255 against the
//     1.0 render at scale 0.5, and 69 at 0.25, and the band spread past its
//     own cutoffs as the scale fell (50,703 lit px at 0.5 and 53,568 at 0.25,
//     against 47,012 at 1.0). Drawn by the blit instead it reads p99 4 at 0.5
//     and 12 at 0.25, with 47,012 lit px at every scale; what is left is the
//     reduced-resolution glow inside the band, not its edges. See
//     docs/neon-resolution-scale-plan.md, step 2.
//
//     NOT used by the edge ring, whose masks and culls are its own. See
//     neon.frag's cutGuard and NeonRenderer::setupGeometry's cutGuardPx. ---
#define BLIT_CUTOFF_GUARD_PX      2.0

// --- Edge ring width. CPU only.
//
//     At every scale, NeonRenderer redraws a ring around the rect
//     edge at FULL resolution (neon.frag's ring program),
//     reading only the gather's result from the gather buffer. The ring reaches R
//     full-res px either side of the edge (GetRingWidth in neon-renderer.cpp):
//
//         R = max(reach(1.0), reach(scale)) + RING_GUARD_TEXELS / scale
//
//     where reach(s) is the filament's reach as neon.frag computes it at scale
//     s, in full-res px. BOTH, because inside R the reduced pass's filament -
//     floored to what its buffer can sample, so wider than the real one
//     whenever the line is thin - is replaced by the real one, and R has to
//     clear whichever reaches further. Uncapped: the reach is where a filament
//     has fallen to FILAMENT_CUTOFF of its gain, about 2/255 after the grade,
//     which is exactly how far its error can show. RING_GUARD_TEXELS adds the
//     reduced buffer's bilinear footprint past that.
//
//     Calibrated over lineWidth {1, 2, 4, 8, 16} x filamentFalloff
//     {0.5, 1, 2, 4} x glowRadius {0, 2, 5, 20}, each config rendered at
//     scales 0.75, 0.5 and 0.25 and compared against its own 1.0 render
//     (docs/neon-resolution-scale-plan.md, step 5). Worst error over all 80:
//
//       RING_GUARD_TEXELS    scale 0.75    scale 0.5    scale 0.25
//              0.5               3            12            3
//              1.0               4             2            2
//              2.0               2             2            2
//
//     1.0 is the smallest that holds 2/255 at 0.5 and 0.25, the targets. 2.0
//     also brings 0.75 within 2 (its worst at 1.0 is a 1 px flat-top line
//     with no glow) for a ring one buffer texel wider each side.
//
//     The two things that made the first version of this ring fail that sweep,
//     both now handled: R used to cover only the full-res filament (fixed by
//     taking the reduced pass's too, above), and at a small glowRadius pass 1's
//     quad stopped short of R, so the ring read gather texels nothing wrote
//     (fixed by covering the ring with the quad; since the gather was split
//     out, the gather pass's own quad, built in setupRingGeometry, covers
//     it). ---
#define RING_GUARD_TEXELS         1.0

// --- Gather resolution. CPU only.
//
//     At every resolutionScale the gather - ~95% of neon.frag's cost when it
//     ran inline - runs in a pass of its own (neon-gather.frag) at its own
//     scale, and pass 1 and the
//     edge ring shade from its result. Its scale is (GetGatherScale in
//     neon-renderer.cpp)
//
//         clamp(GATHER_TEXELS_PER_KERNEL / kc, GATHER_MIN_SCALE,
//               min(resolutionScale, GATHER_MAX_SCALE))
//
//     with kc the colour kernel's width in full-res px, perimeter *
//     COLOR_BLEND_PERIM_FRAC: the gather's outputs are Lorentzian-weighted
//     means whose kernel is never narrower than kc, so this many texels per kc
//     carries them through a bilinear read.
//
//     Calibrated on Mesa llvmpipe with an RGBA16F gather buffer (worst error
//     against 1.0 over `probe scenes`, and the default scene's frame time):
//
//       GATHER_TEXELS_PER_KERNEL   worst error   default 0.5   default 0.25
//                1.0                   4              -              -
//                1.5                   2           10.5 ms        8.0 ms
//                2.0                  1-2          10.8 ms        7.3 ms
//                4.0                   -           14-17 ms      11-12 ms
//         (gather at resolutionScale)  1-2          37.5 ms       12.3 ms
//
//     2.0 is the knee: 1.5 saves nothing measurable (the gather is no longer
//     the pass that costs) and 4.0 costs half as much again for no visible
//     gain. With an RGBA8 gather buffer - the fallback - 2.0 reads 3/255.
//     GATHER_MIN_SCALE barely matters - full-viewport rects
//     at 720p and 1080p read 1-2 at a floor of 0 and 0.0625 alike - and is
//     kept as a cheap guard: the gather buffer is clipped to the viewport, so
//     at the floor it is at most 128 x 80 texels at 1080p. See
//     docs/neon-resolution-scale-plan.md section 13. ---
#define GATHER_TEXELS_PER_KERNEL  2.0
#define GATHER_MIN_SCALE          0.0625

// --- The gather's ceiling. CPU only.
//
//     GetGatherScale never gathers finer than this, at any resolutionScale.
//     It binds only on a rect whose perimeter is under ~450 px (kc under 4
//     px), which would otherwise gather near full resolution: its gather
//     buffer - RGBA16F over the glow quad's box, two attachments with
//     segments - ran to 16 / 33 MB at 1080p for a 40 x 24 rect whose glow
//     fills the frame. At 0.5 that is a quarter, the same as the scaled path
//     at 0.5 always gathered at, and the check suite's smallest rect (perimeter
//     478 px) is not touched.
//
//     It replaces the split gate at 1.0 (FULL_RES_SPLIT_GATHER_MAX_SCALE /
//     FULL_RES_SPLIT_MIN_AREA_PX, removed - docs/neon-shader-cleanup-plan.md
//     step 3), which ran the gather INLINE in neon.frag where splitting it
//     out did not pay: on an AMD Radeon Pro 5300M, where the first offscreen
//     pass of a frame costs ~0.15 ms, for a still cutoff band under ~140k px
//     of quad and for a gather near full resolution. Since unchanged frames
//     stopped re-running the gather (I40) the still case no longer leaves
//     the caller's framebuffer at all; an Apple M2 Pro measured the split
//     1.1-4.75x faster at every size and mode, and the AMD, re-measured with
//     every rect split against its own gate, 0.98-1.02x in every animated
//     mode with one loss - a cutoff band under an intensity pulse, 0.84x.
//     So every scale splits, and the inline loop went with the gate. ---
#define GATHER_MAX_SCALE          0.5

// --- Stand-in distance for a cutoff that glowSide has already subsumed.
//
//     neon.frag hands this to the band distance on the side the one-sided cut
//     culls, so the discard and the mask there no-op through the arithmetic
//     exactly as a DISABLED cutoff does - same mechanism, same sentinel shape,
//     no extra branch downstream.
//
//     Matches CUTOFF_DISABLED_SIZE in neon-renderer.cpp, which is what a
//     disabled cutoff actually arrives as. It is a distance in the shader's own
//     px space and only has to be far enough that no realistic geometry reaches
//     it; see the note on that constant for the same reasoning. ---
#define CUTOFF_NEUTRALISED        1.0e6

// --- Glow reach (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity).
//
//     Named for the reach, not for an early-out: the per-fragment
//     `ad > earlyOut -> discard` this constant was originally calibrated for
//     no longer exists. Geometry culls the far region instead, which is
//     tiler-friendly, so what the factor sets is how far the glow is allowed to
//     reach before the quad stops covering it. The locals it feeds already say
//     so - `glowReach` in both setupGeometry implementations, `reach` in both
//     shaders.
//
//     glowRadius only: the companion sampleSpacing * SPACING_FACTOR term is
//     gone. sampleSpacing is perimeter / NEON_MAX_LOOP_SAMPLES, so it won on
//     any reasonably large rect at default glowRadius and made both the
//     distance the bloom got truncated at and the brightness it still had
//     there track the rect size. The bloom's reach is a function of glowRadius
//     alone, so that is all that sizes the quad.
//
//     Used by the renderers' setupGeometry AND by the shaders, which recompute
//     the same expression to place the bloom pedestal that lets this margin
//     stay tight without the truncation showing. Keep the two in step. ---
#define GLOW_REACH_RADIUS_FACTOR  48.0

// --- Where the shaders' quad-edge fade begins, as a FRACTION of the quad
//     margin. The emission ramps to zero over [FRAC * margin, margin], so the
//     bloom's 1/D tail - still ~10% of peak out at the quad edge - never clips
//     as a hard rectangle.
//
//     A fraction, not a pixel span, because the margin is proportional to
//     glowRadius (see GLOW_REACH_RADIUS_FACTOR) and so is the bloom profile it
//     hides. A fraction keeps the ramp at a constant proportion of the bloom's
//     reach, so the fade reads the same at every glow radius; a fixed px ramp
//     would vanish on a wide glow and dominate a narrow one. At the stock
//     glowRadius 5 / bloom 0.3 / intensity 1 the margin is 312 px, giving a
//     62 px ramp.
//
//     Nothing derives 0.8 - anything leaving a ramp wide enough to hide the
//     clip behaves the same. What it DOES assume is that the margin was set by
//     the glow, where 20% of it is a long distance. When outsideCutoff clamps
//     the margin instead (to where its fade ends, size + softness, plus 1),
//     20% of ~13 px is 2.6 px and
//     the ramp lands INSIDE the cutoff band, dimming the band's outer edge
//     ahead of the cutoff mask - and only on the exterior, since the fade keys
//     on positive d. Both shaders therefore floor the ramp's start at the
//     cutoff boundary whenever that boundary falls inside the quad; see the
//     fadeStart block in neon.frag.
//
//     Since V23 the bloom no longer relies on this fade to end smoothly - each
//     piece fades by its own distance from BLOOM_FADE_START_FRAC, below, which
//     has it under a level well before this ramp starts. ---
#define QUAD_FADE_START_FRAC      0.8

// --- Where each piece's bloom starts fading out, as a FRACTION of `reach`
//     (neon.frag's bloom end fade, V23).
//
//     The pedestal takes the bloom's 1/a tail to exactly 0 at `reach`, but
//     with its slope still on, so the glow stopped on a crease: inside a large
//     rect, a hard-edged dark rectangle `reach` in. The quad-edge fade above
//     hid it outside, but over [0.8, 1.0] of the margin, where the tail is
//     still ~5 levels up, and a fade that late STEEPENS the tail before it
//     flattens it - a shoulder, then flat, which reads as an edge too. So
//     each piece's bloom is multiplied by 1 - smoothstep(FRAC * reach, reach,
//     a) of its own distance, on both sides.
//
//     0.5 is about the latest start that does not steepen it: the steepest
//     slope inside the fade over the slope where it starts, on the
//     infinite-line bloom at glowRadius 5, bloomStrength 0.52, intensity 0.8,
//     is 1.40 at 0.8, 1.14 at 0.6, 1.035 at 0.5 and 1.00 from 0.4 down. (It
//     is also where c * (reach - a)^2 meets the pedestalled 1/a tail in value
//     and slope.) An earlier start costs more of the tail; at 0.5 the glow
//     reads up to 6 levels dimmer in the outer half of its reach. ---
#define BLOOM_FADE_START_FRAC     0.5

// --- Filament reach for the same quad sizing, expressed in sigmas.
//
//     glowRadius = 0 means "filament only" - the halo and bloom are gated off
//     by GLOW_GATE_FADE_PX - but the filament itself is still there, sized by
//     lineWidth, so the quad has to cover it or the exterior gets clipped.
//
//     The reach is NOT a constant, because the profile is
//     exp2(-(ad/sigma)^N) with N = 2 * filamentFalloff and the slider runs
//     down to falloff 0. Solving core * FILAMENT_GAIN < FILAMENT_CUTOFF gives
//
//         reach = sigma * log2(FILAMENT_GAIN / FILAMENT_CUTOFF) ^ (1/N)
//
//     which is 1.9 sigmas at falloff 2.0, 3.5 at 1.0, 12.6 at 0.5, 67.8 at 0.3
//     and 558 at 0.2 - it diverges as falloff -> 0. A fixed 12 (calibrated for
//     falloff 0.5) left the low end badly under-sized: the interior is
//     unbounded because the quad always covers it, so a soft filament flooded
//     inward while the exterior was chopped at 12 sigmas. That asymmetry is
//     the "no outer glow at glowRadius 0" report.
//
//     MAX_SIGMAS bounds the fill cost where the formula diverges; the shaders
//     pedestal-subtract the core at exactly this reach, so hitting the cap
//     shortens the glow symmetrically on BOTH sides instead of showing a cliff
//     on the outside. MIN_SIGMAS keeps a little room at very high falloff.
//     Keep the expression in step across neon-tuning.h, both setupGeometry
//     implementations, and both fragment shaders. ---
#define FILAMENT_CUTOFF           0.002
#define FILAMENT_REACH_MIN_SIGMAS 2.0
#define FILAMENT_REACH_MAX_SIGMAS 64.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_

#define NEON_GLOW_COVER_FILL
// ---------------------------------------------------------------------------
// The emitter's pieces, shared by neon.frag and neon-glow-cover.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it, after the tuning
// header, into the two programs that have to agree on it to the texel:
// neon.frag, which sums the halo and bloom over the emitter's eight pieces and
// scales each by that piece's coverage from the glow coverage table, and
// neon-glow-cover.frag, which bakes the table. Three things live here because
// both sides need them:
//
//   - minimaxAtan, the atan both sides develop a corner with.
//   - arcTangentSegment, how a corner arc is developed onto a straight line.
//     neon.frag integrates a corner's halo and bloom along that line, and the
//     bake convolves the corner's coverage along the same line, so what scales
//     a corner's glow is the coverage of the extent that glow was integrated
//     over.
//   - the table's layout, as a forward map (where a piece's coverage is read,
//     for neon.frag) next to its inverse (what each texel holds, for the
//     bake). The two must be exact inverses, which is the reason they sit
//     side by side rather than one in each file.
//
// Every length here is in one unit chosen by the caller - px in neon.frag,
// perimeter fractions in the bake. The layout depends only on ratios of
// lengths (to the halo width kh, to a piece's own length), so the two agree
// whatever the unit and at every resolution scale.
// ---------------------------------------------------------------------------

precision highp float;

// Own names: neon-common.glsl, which neon.frag also includes, declares PI and
// HALF_PI as constants, and the bake does not include it.
#define PIECES_PI      3.141592653589793
#define PIECES_HALF_PI 1.5707963267948966

// --- A cheaper two-argument atan ----------------------------------------
// The shading's two hottest transcendentals are atans: bloomSegment's (every
// straight's bloom and every corner's, in neon.frag) and the corner
// development's `th` below. GLSL's atan(y, x) is the driver's, and measured on
// an AMD Radeon Pro 5300M this octant-reduced minimax polynomial in its place
// made every frame whose config animates 1.07-1.11x faster, at 1.0 and 0.5,
// moving at most 0.016% of a frame's channels by 1 level (I48).
// Its error is at most 1.7e-6 rad over the whole range - a few float ulps of
// PI - so nothing downstream can tell it from the built-in.
//
// Here rather than in neon.frag so the coverage bake develops the corners
// through the same function as the read: the two have to agree on `th`.
// Undefined only where atan(y, x) is (both zero): it returns 0 there.
// Measure on each target GPU before trusting the gain - a cheaper atan is the
// most GPU-specific change this file has.
float minimaxAtan(float y, float x) {
    float ax = abs(x);
    float ay = abs(y);
    float t  = min(ax, ay) / max(max(ax, ay), 1e-30);
    float s  = t * t;
    float r  = t * (0.99997726 + s * (-0.33262347 + s * (0.19354346 + s * (-0.11643287 +
               s * (0.05265332 + s * -0.01172120)))));
    r = (ay > ax) ? PIECES_HALF_PI - r : r;
    r = (x < 0.0) ? PIECES_PI - r : r;
    return (y < 0.0) ? -r : r;
}

// --- Corner arcs, developed onto their tangent -------------------------
// The four straights cover the rect's flat runs. Above cornerRadius 0 the
// emitter also turns through four quarter arcs, and a circular arc has no
// elementary antiderivative under either kernel. This develops each arc onto a
// straight line instead: the tangent at whichever ARC POINT IS NEAREST the
// fragment, carrying the arc's full length PI*r/2 and split about that point.
//
// That choice is what makes it accurate where the naive one is not. The
// perpendicular distance it reports is the true distance to the arc wherever
// the fragment faces it (|length(w) - r|), and the two halves run exactly as
// far as the real arc does in each direction, so the developed arc abuts the
// trimmed straights in arclength and the emitter is continuous - no gap and no
// overlap at the tangent points.
//
// `w` is the fragment's offset from the arc centre in that corner's own frame:
// x along the outward normal of one incident edge, y along the other's, both
// positive pointing away from the rect. So the arc occupies exactly the first
// quadrant of `w`, from +x (one tangent point) to +y (the other), and clamping
// the direction into that quadrant is max(w, 0).
//
// Off the ends the nearest arc point is a tangent point, and which one follows
// from |w - (r,0)|^2 - |w - (0,r)|^2 = 2r*(w.y - w.x): the +x end when
// w.x >= w.y. That is what the fallback picks, so it is the right clamp for the
// whole region it covers and not only for the degenerate point that forces it.
//
// --- THE DEVELOPMENT RATE IS NOT r. ------------------------------------
// Laying the arc out at its own arclength - one unit of tangent per unit of
// arc - is only right for a fragment ON the arc. The exact distance to the
// point at angle dphi from the nearest one is
//
//     D^2 = a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,   rho = length(w)
//
// so the tangent coordinate the kernels actually want is t = 2*sqrt(rho*r)*
// sin(dphi/2), whose slope at the foot is sqrt(rho*r), not r. Develop at rate
// `lam` instead of r and the emitter comes out short or long, so the measure
// is put back by scaling the whole segment by r/lam - `w` of the returned
// vec4. Rate x weight is r either way, so the arc always carries its full
// PI*r/2 of emitter.
//
// Two things follow, and they are the whole reason for the change:
//
//   - AT THE CENTRE OF CURVATURE IT IS NOW EXACT. rho -> 0 collapses the
//     segment to zero length against an infinite weight, and the limit is
//     f(r) * PI*r/2: every point of the arc at distance r, which is what a
//     fragment at the centre actually sees. At rate r the arc ran off to one
//     side of the foot instead, and since both kernels peak at t = 0 that
//     UNDER-counted - on a circle, where all four arc centres coincide at the
//     middle of the shape, to 54% of the true value.
//   - THE CLAMP STOPS CREASING. Crossing w.y = 0 the arc's endpoints slide at
//     -lam * d(th) on the facing side and at -d(off) on the clamped side; the
//     first is -lam/w.x and the second -1, and they agree only where lam is
//     length(w). At rate r they agreed only at w.x == r, so every other point
//     of the lines through the arc centre carried a C1 crease - a dark cross
//     at the centre of curvature, unmistakable on a circle. Measured as the
//     spurious curvature of (model - numerically integrated truth): 19.9% of
//     the local value at rate r, 0.5% here.
//
// lam is min(rho, sqrt(rho*r)), i.e. sqrt(rho * min(rho, r)). The inner
// branch's rate has to be rho for the clamp to join smoothly; outside the arc
// the linearisation above wants sqrt(rho*r), and the two meet at rho == r,
// where lam is r and the weight is 1 - so a fragment on the arc is bit-
// identical to the rate-r form this replaces, and the calibration the NORM
// factors carry is untouched. Against a numerically integrated perimeter the
// whole emitter's worst error drops as well, on every geometry tested: 16.5 ->
// 9.8% on a 600x400 r=40, 37.3 -> 26.3% on a circle.
//
// Cost is 1.03x of the neon pass at 1280x720, and - as the arc pedestal's note
// in neon.frag warns - that includes the cornerRadius 0 path, which never
// executes a line of this and still pays 1.026x for the register pressure.
// Re-time both after touching it. An inversesqrt formulation that trades the
// sqrt and the divide for two inversesqrts was measured and came out inside
// the noise, so the readable form stays.
//
// Returns (a, t1, t2, weight) for haloSegment / bloomSegment; the caller
// multiplies the segment by .w. The coverage bake reads the same four numbers
// to lay the arc's coverage along that line: arclength q from the +x end sits
// at t1 + q / weight.
//
// `u` is the direction of the arc point the arc is developed about, which
// arcTangentSegment below picks; the bake also calls this with each of the two
// fallback directions, behind the centre where the pick flips between them.
vec4 arcTangentSegmentAbout(vec2 w, float r, vec2 u) {
    // Arclength from the +x tangent point to the nearest arc point. u is a unit
    // vector in the first quadrant whenever it came from wq, so th is in
    // [0, HALF_PI] and needs no clamp of its own.
    float th  = minimaxAtan(u.y, u.x);
    float a   = abs(dot(w, u) - r);
    // Offset of the fragment ALONG the tangent, zero whenever u came from wq
    // (the foot of perpendicular is then the tangent point itself) and non-zero
    // only on the fallback, where it correctly pushes the whole arc to one side.
    // A LENGTH, not an arclength, so it is not scaled by the rate below.
    float off = dot(w, vec2(-u.y, u.x));
    // Floored so the exact centre of curvature cannot divide by zero. The
    // floor is far below one px, and the limit it lands on is the exact value
    // anyway - see above.
    float rho = max(length(w), ARC_FRAME_EPSILON);
    float lam = sqrt(rho * min(rho, r));
    return vec4(a, -lam * th - off, lam * (PIECES_HALF_PI - th) - off, r / lam);
}
// The direction arcTangentSegment develops about: the nearest arc point's.
vec2 arcTangentDirection(vec2 w) {
    vec2  wq = max(w, vec2(0.0));
    float ql = length(wq);
    return (ql > ARC_FRAME_EPSILON) ? wq / ql : ((w.x >= w.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
}
vec4 arcTangentSegment(vec2 w, float r) {
    return arcTangentSegmentAbout(w, r, arcTangentDirection(w));
}

// --- The glow coverage table's layout (V21) ----------------------------
// One table per PIECE of the emitter, because the coverage that scales a
// piece's halo and bloom is that piece's own: its coverage over its own extent,
// weighted by the layer's kernel at the fragment. V20's table was one per
// PERIMETER, indexed by the foot's perimeter position, and convolved along a
// straight line through the foot that ran on past the piece's ends - so near a
// corner where a lit stretch met a dark one, each piece borrowed its
// neighbour's coverage (V21 in docs/review-findings.md).
//
// `tableLayout.x` texels across - the table's width - and four bands of
// GLOW_COVER_ROWS rows down. Band b holds straight b at its left and corner b
// at its right:
//
//   straight   `tableLayout.y` overhang columns, the straight's own `inner`
//              columns, `tableLayout.y` more: across, the fragment's projection
//              along the straight -
//              uniform over the piece and rational past each end; down, its
//              distance a from the line, a / (a + kh), as V20's table had it. A
//              straight's clipped extent follows from the projection, so two
//              variables still describe it, and its table is symmetric about
//              the line.
//   corner     a guard texel, `tableLayout.z` overhang columns, the corner's own
//              `tableLayout.w` - inner columns, `tableLayout.z` more, a guard: across, the
//              fragment's direction from the arc's centre - uniform over the
//              arc's own quadrant, then on either side of it to the diagonal
//              behind the centre, where the two sides meet; down, its
//              distance from the centre, the arc's inside in the top half and
//              its outside in the bottom. Polar position determines everything
//              arcTangentSegment returns, inside, outside and behind the centre
//              alike, so two variables describe a corner too.
//
// THE LAYOUT IS A PARAMETER (`tableLayout`, the uniform uGlowCoverLayout): .x the
// columns - the texture's width, so the forward maps divide by the texture
// they read - .y a straight's overhang columns past each end, .z a corner's
// either side of its arc, and .w the columns a band's straight and corner
// share, .x - 2 - 2 .y - 2 .z. NeonRenderer sizes the table per config and
// stores the layout WITH it (GlowCoverLayout), so the bake and every read take
// the same four numbers; all are whole columns.
//
// `inner`, how many of a band's `tableLayout.w` columns its straight gets, is the
// uniform uGlowCoverSplit (.x the vertical straights' bands, .y the
// horizontal ones'), whole columns, set by NeonRenderer::GetGlowCoverSplit in
// proportion to the straight's and the arc's lengths. It comes from the CPU
// rather than from each shader's own arithmetic because the two shaders work
// in different units, and a split that rounded differently in one of them
// would read every texel of the band from the wrong place. So a circle's
// corners get the columns its zero-length straights do not need, and a long
// rect's straights get its small corners': at a fixed size, every piece gets
// a column density in proportion to its length.
//
// THE READ HAS TO BE CHEAP, AND THAT IS WHY EVERY MAP HERE IS A DIVISION.
// neon.frag computes these coordinates for up to eight pieces, and at scale 1.0
// the program also carries the gather loop, which makes it sensitive to the
// size of everything else in it - including code that a frame never runs.
// Measured on an AMD Radeon Pro 5300M, against the build before V21, on a fully
// lit ring that skips every read: with the corner direction an atan, the
// spacing past each end a log and the inside of the arc spaced by two more
// logs, the whole neon pass was 1.16x slower - 1.163x on a SHARP-cornered ring,
// which never enters the corner block at all - and stubbing the corner
// coordinates out took it to 1.04x. So the direction is a diamond angle (one
// division, see glowCoverDiamond), the spacing past the ends rational, and the
// inside a ratio of square roots, which took it to ~1.08x; the rest came from
// running neon.frag's corner block one arc at a time (addCornerPiece there).
// The bake inverts the maps, where cost does not matter. Do not put an atan or
// a log back in the forward maps without re-timing a fully lit ring at scale
// 1.0.
//
// WHICH PROGRAM COMPILES WHICH HALF. The forward maps (where a piece's
// coverage is READ) and their inverses (what each texel HOLDS) are compiled
// only into the programs that call them, since this compiler builds every
// function in a source whether main() reaches it or not: neon.frag takes the
// forward half, the bake (NEON_GLOW_COVER_BAKE) the inverse half, and the
// segment table's fill (NEON_GLOW_COVER_FILL), which decodes a texel of one
// table and reads its value from another, both.
#if !defined(NEON_GLOW_COVER_BAKE)
#define GLOW_COVER_FORWARD_MAPS
#endif
#if defined(NEON_GLOW_COVER_BAKE) || defined(NEON_GLOW_COVER_FILL)
#define GLOW_COVER_INVERSE_MAPS
#endif

// Which straight is which band, by the side of the rect in vPos it lies on.
// The vertical straights (x = -/+ halfSize.x) have length 2 * straight.y, the
// horizontal ones 2 * straight.x. Corner block b is in band b too.
#define GLOW_COVER_BAND_NEG_X  0
#define GLOW_COVER_BAND_POS_X  1
#define GLOW_COVER_BAND_NEG_Y  2
#define GLOW_COVER_BAND_POS_Y  3

// How far a corner's direction coordinate runs past each end of its arc: the
// arc is [0, 1] in it, and both sides reach the diagonal behind the centre at
// 1.5 past an end - 3/8 of a turn.
#define GLOW_COVER_CORNER_REACH 1.5

// Which corner block - and so which band - a corner's table is in: `signs` is
// the corner's quadrant in vPos, +-1 on each axis.
#ifdef GLOW_COVER_FORWARD_MAPS
int glowCoverCornerBlock(vec2 signs) {
    return (signs.x > 0.0 ? 1 : 0) + (signs.y > 0.0 ? 2 : 0);
}
#endif

// Band `band`'s straight interior columns, from uGlowCoverSplit.
float glowCoverInner(int band, vec2 split) {
    return (band < 2) ? split.x : split.y;
}

// A distance from the line as a row coordinate in [0, 1]: half the rows within
// one halo width, the rest out to the far field. V20's spacing.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverDistanceRow(float a, float kh) {
    return a / (a + kh);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverRowDistance(float v, float kh) {
    return kh * v / (1.0 - v);
}
#endif

// Past a straight's end, `e` the distance past it: half the overhang's columns
// within a quarter of them in halo widths, so a column is kh / 4 at the end
// and the spacing grows in proportion to e beyond. Rational rather than
// logarithmic for the reason above.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverStraightOver(float e, float kh, vec4 tableLayout) {
    return e / (e + 0.25 * tableLayout.y * kh);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverStraightPast(float over, float kh, vec4 tableLayout) {
    return 0.25 * tableLayout.y * kh * over / (1.0 - over);
}
#endif

// A direction as a "diamond angle": monotone in the true angle and continuous
// all the way round, without an atan. The arc's own quadrant is [0, 1] - 0 at
// its +x end, 1 at +y - and the rest of the turn runs on from either end to
// -1.5 / 2.5, which are both the diagonal behind the centre. `w` need not be
// normalised; at the centre itself any direction is as good as another.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverDiamond(vec2 w) {
    float l = abs(w.x) + abs(w.y);
    float p = (l > 0.0) ? w.x / l : 1.0;
    return (w.y >= 0.0) ? 1.0 - p : ((p < -0.5) ? 3.0 + p : p - 1.0);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
vec2 glowCoverDiamondDirection(float d) {
    float q = (d < 0.0) ? d + 4.0 : d;
    float p = (q <= 2.0) ? 1.0 - q : q - 3.0;
    return normalize(vec2(p, ((q <= 2.0) ? 1.0 : -1.0) * (1.0 - abs(p))));
}
#endif

// Past a corner's end, `d` the diamond angle past it: half the columns spaced
// evenly to the diagonal behind the centre, which is what a fragment far from
// the corner needs, and half concentrated at the end, a column about kh / 4 of
// arc there, which is what one beside the arc needs.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverCornerOver(float d, float r, float kh, vec4 tableLayout) {
    float k = tableLayout.z * kh / (8.0 * max(r, 1e-30));
    float R = GLOW_COVER_CORNER_REACH;
    return 0.5 * d / R + 0.5 * d * (R + k) / (R * (d + k));
}
#endif
// Its inverse: A d + B d / (d + k) = t is a quadratic in d, solved by whichever
// form of its positive root does not cancel.
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverCornerPast(float t, float r, float kh, vec4 tableLayout) {
    float k    = tableLayout.z * kh / (8.0 * max(r, 1e-30));
    float R    = GLOW_COVER_CORNER_REACH;
    float A    = 0.5 / R;
    float b    = A * k + 0.5 * (R + k) / R - t;
    float disc = sqrt(b * b + 4.0 * A * t * k);
    return (b >= 0.0) ? 2.0 * t * k / max(b + disc, 1e-30) : (disc - b) / (2.0 * A);
}
#endif

// Inside a corner's arc the coverage changes fast in two places - beside the
// arc, where the halo's width is kh, and near the centre, where the developed
// arc's width r / lam grows without bound - and slowly between. A ratio of
// square roots spaces the rows finely at both ends: `rho` the distance from the
// centre, 0 on the arc and 1 at the centre. Its inverse is closed form.
#ifdef GLOW_COVER_FORWARD_MAPS
float glowCoverInsideRow(float rho, float r) {
    float fromArc = sqrt(max(r - rho, 0.0));
    float fromCentre = sqrt(max(rho, 0.0));
    return fromArc / max(fromArc + fromCentre, 1e-30);
}
#endif
#ifdef GLOW_COVER_INVERSE_MAPS
float glowCoverInsideRadius(float v, float r) {
    return r * (1.0 - v) * (1.0 - v) / (v * v + (1.0 - v) * (1.0 - v));
}
#endif

// Texture coordinate of straight `band`'s coverage: `x` is the fragment's
// projection along it from its t1 end (haloSegment's), UNCLAMPED, `len` the
// straight's length, `a` the distance from its line, `kh` the halo width,
// `inner` glowCoverInner, `tableLayout` the table's. Clamped to the straight's own
// texels, since its corner's guard is the next one along. The inverse is
// glowCoverStraightAt.
#ifdef GLOW_COVER_FORWARD_MAPS
vec2 glowCoverStraightUV(int band, float x, float len, float a, float kh, float inner, vec4 tableLayout) {
    float over  = tableLayout.y * glowCoverStraightOver(max(-x, 0.0) + max(x - len, 0.0), kh, tableLayout);
    float along = (len > 0.0) ? clamp(x / len, 0.0, 1.0) : 0.5;
    float col   = tableLayout.y + inner * along + (x < 0.0 ? -over : over);
    col         = clamp(col, 0.5, 2.0 * tableLayout.y + inner - 0.5);
    float row   = clamp(glowCoverDistanceRow(a, kh) * float(GLOW_COVER_ROWS), 0.5, float(GLOW_COVER_ROWS) - 0.5);
    return vec2(col / tableLayout.x,
                (float(band * GLOW_COVER_ROWS) + row) / float(GLOW_COVER_HEIGHT));
}
#endif

// Texture coordinate of a corner's coverage: `w` is the fragment's offset from
// the arc centre in arcTangentSegment's frame, `block` from
// glowCoverCornerBlock, `inner` its band's straight's glowCoverInner,
// `tableLayout` the table's. The inverse is glowCoverCornerAt. The seam is the
// diagonal behind the centre, where the two overhangs meet; the guard column
// at each end of the block holds the other end's value, so a fetch is
// continuous across it.
#ifdef GLOW_COVER_FORWARD_MAPS
vec2 glowCoverCornerUV(int block, vec2 w, float r, float kh, float inner, vec4 tableLayout) {
    float arcCols = tableLayout.w - inner;
    float d       = glowCoverDiamond(w);
    float over    = tableLayout.z * glowCoverCornerOver(max(-d, 0.0) + max(d - 1.0, 0.0), r, kh, tableLayout);
    float col     = tableLayout.z + arcCols * clamp(d, 0.0, 1.0) + (d < 0.0 ? -over : over);
    float rho     = length(w);
    float side    = (rho >= r) ? glowCoverDistanceRow(rho - r, kh) : -glowCoverInsideRow(rho, r);
    float halfRows = 0.5 * float(GLOW_COVER_ROWS);
    float row     = clamp(halfRows * (1.0 + side), 0.5, float(GLOW_COVER_ROWS) - 0.5);
    return vec2((2.0 * tableLayout.y + inner + 1.0 + col) / tableLayout.x,
                (float(block * GLOW_COVER_ROWS) + row) / float(GLOW_COVER_HEIGHT));
}
#endif

// Inverse of glowCoverStraightUV for one texel: the projection `x` and the
// distance `a` its centre holds, from its column and row within the band.
#ifdef GLOW_COVER_INVERSE_MAPS
vec2 glowCoverStraightAt(float col, float row, float len, float kh, float inner, vec4 tableLayout) {
    float over = tableLayout.y;
    float x;
    if (col < over) {
        x = -glowCoverStraightPast((over - col) / over, kh, tableLayout);
    } else if (col > over + inner) {
        x = len + glowCoverStraightPast((col - over - inner) / over, kh, tableLayout);
    } else {
        x = (col - over) / max(inner, 1.0) * len;
    }
    return vec2(x, glowCoverRowDistance(row / float(GLOW_COVER_ROWS), kh));
}
#endif

// Inverse of glowCoverCornerUV for one texel: the offset `w` from the arc
// centre its centre holds, from its column within the corner's block (guards
// included, so 0 and the block's last are the guards) and its row.
#ifdef GLOW_COVER_INVERSE_MAPS
vec2 glowCoverCornerAt(float col, float row, float r, float kh, float inner, vec4 tableLayout) {
    float arcCols = tableLayout.w - inner;
    float over    = tableLayout.z;
    // Past the guard, the column is the same direction the other end's last
    // texel holds: the seam is one direction, approached from both sides.
    float cols    = arcCols + 2.0 * over;
    float c       = col - 1.0;
    c            -= cols * floor(c / cols);
    float d;
    if (c < over || c > over + arcCols) {
        float past = glowCoverCornerPast(((c < over) ? over - c : c - over - arcCols) / over, r, kh, tableLayout);
        d = (c < over) ? -past : 1.0 + past;
    } else {
        d = (c - over) / arcCols;
    }
    float halfRows = 0.5 * float(GLOW_COVER_ROWS);
    float rho = (row >= halfRows) ? r + glowCoverRowDistance((row - halfRows) / halfRows, kh)
                                  : glowCoverInsideRadius((halfRows - row) / halfRows, r);
    return rho * glowCoverDiamondDirection(d);
}
#endif

// Which texel of a table laid out as `tableLayout` and split as `split` the
// fragment at `fragCoord` is: its band, its row within the band (at the
// texel's centre), the band's straight interior columns, and the column its
// corner block starts at - left of it the band's straight, from it on its
// corner. The bake and the segment table's fill both decode a texel here, so
// the two cannot place one differently.
#ifdef GLOW_COVER_INVERSE_MAPS
void glowCoverTexel(vec2 fragCoord, vec2 split, vec4 tableLayout, out int band, out float row, out float inner,
                    out float splitColumn) {
    float y     = floor(fragCoord.y);
    band        = int(y) / GLOW_COVER_ROWS;
    row         = y - float(band * GLOW_COVER_ROWS) + 0.5;
    inner       = glowCoverInner(band, split);
    splitColumn = 2.0 * tableLayout.y + inner;
}
#endif

precision highp float;

// ---------------------------------------------------------------------------
// The segment table's fill (pass 0f).
//
// The glow coverage table (neon-glow-cover.frag) holds, per piece of the
// emitter, the arcs' coverage on .r / .g and the segments' on .b / .a. An
// arc's coverage has edges as sharp as its 14 px feather, so the table is as
// wide as the arcs need (NeonRenderer::GetGlowCoverWidth). A segment is a
// Gaussian bell whose standard deviation is a third of its length - tens to
// hundreds of px - and baking its channels at the arcs' width integrated
// every bell at four to eight times the columns it needs, which made the bake
// the largest pass of every frame that moved a segment.
//
// So the segments are baked into a narrow table of their own (the bake with
// uBakeTarget 1, its own layout), and this copies them into the main table's
// .b / .a, under a colour mask that leaves .r / .g alone: for each texel of
// a piece, decode what the MAIN table's texel holds - a straight's projection
// and distance, or a corner's offset from its centre - through the main
// layout's inverse map, find where the segment table holds the same position
// through ITS layout's forward map, and take one linear fetch there. So
// neon.frag still reads one texel per piece, unchanged, and pays nothing for
// the split; the cost is this pass, on the frames a segment moved.
//
// Both maps are neon-pieces.glsl's (NEON_GLOW_COVER_FILL compiles both
// halves), with the same texel decode as the bake (glowCoverTexel), so this
// places a texel exactly where the bake does. Lengths are fractions of the
// perimeter, as the bake's.
//
// The value is copied ENCODED, c / (1 + c), and filtered so, as neon.frag
// filters the main table: the segment table's columns are a small fraction
// of a bell wide, so the curvature that filtering in the encoded space adds
// is far below a level.
// ---------------------------------------------------------------------------

out vec4 fragColor;

uniform sampler2D uSegCover;       ///< The segment table, encoded segment (halo, bloom) on .r / .g.
uniform vec4  uGlowCoverLayout;    ///< The MAIN table's layout - the texel being written.
uniform vec2  uGlowCoverSplit;     ///< The main table's split.
uniform vec4  uSegCoverLayout;     ///< The segment table's layout - where the value is read.
uniform vec2  uSegCoverSplit;      ///< The segment table's split.
uniform float uHaloWidth;          ///< kh as a fraction of the perimeter, as the bake's.
uniform vec2  uStraightSize;       ///< The horizontal and vertical straights' lengths, as the bake's.
uniform float uRadius;             ///< The corner radius, as the bake's.

void main() {
    float kh = uHaloWidth;
    float r  = uRadius;
    int   band;
    float row;
    float inner;
    float split;
    glowCoverTexel(gl_FragCoord.xy, uGlowCoverSplit, uGlowCoverLayout, band, row, inner, split);
    float innerSeg = glowCoverInner(band, uSegCoverSplit);

    // Zero where the bake writes zero: a straight with no length (a circle's)
    // and a corner with no radius.
    vec2 segs = vec2(0.0);
    if (gl_FragCoord.x < split) {
        float len = (band == GLOW_COVER_BAND_NEG_X || band == GLOW_COVER_BAND_POS_X) ? uStraightSize.y
                                                                                     : uStraightSize.x;
        if (len > 0.0) {
            vec2 xa = glowCoverStraightAt(gl_FragCoord.x, row, len, kh, inner, uGlowCoverLayout);
            segs    = textureLod(uSegCover, glowCoverStraightUV(band, xa.x, len, xa.y, kh, innerSeg, uSegCoverLayout),
                                 0.0).rg;
        }
    } else if (r > 0.0) {
        // Corner block b is in band b, in both tables.
        vec2 w = glowCoverCornerAt(gl_FragCoord.x - split, row, r, kh, inner, uGlowCoverLayout);
        segs   = textureLod(uSegCover, glowCoverCornerUV(band, w, r, kh, innerSeg, uSegCoverLayout), 0.0).rg;
    }
    fragColor = vec4(0.0, 0.0, segs);
}

)shadersrc";

                static const char *const NEON_BLIT_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - the neon shaders (neon.frag, neon-emission.frag), where CMake
//     text-injects this file via @NEON_TUNING@ in shaders.h.in, and
//   - the C++ renderer (neon-renderer.cpp), which #includes it for the
//     glow-reach quad-sizing factors.
//
// The px constants below are written in FULL-RESOLUTION pixels. Where the
// renderer draws at a reduced NeonConfig::resolutionScale, the shader converts
// each one with uResolutionScale at the point of use; the notes on the
// individual constants say which need it and which do not.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a `const float X = 0.9f;` cannot
// be written once and stay valid in both languages. Plain #define is the only
// form that compiles identically as GLSL and as C++ from a single file.
// (For C++-only constants, prefer constexpr - macros are used here strictly
// because these values must also live inside the shaders.)
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
//
//     FILAMENT_MIN_HALF_WIDTH is a STATED-width floor: it is what makes
//     lineWidth 0 mean "no line" rather than a single bright dot, and it is
//     paired with lineGate, which fades the filament in over the same span. It
//     describes the width the caller asked for, so it is a FULL-RES px constant
//     and the shader converts it with uResolutionScale like every other one.
//
//     The SAMPLING floor is the pair below, and the distinction from the
//     stated floor is the whole point of having two of them. Below
//     resolutionScale 1.0 the gather rasterises into a reduced buffer and
//     neon-blit.frag bilinearly upsamples it, so a filament the buffer cannot
//     sample is not reconstructed - it is resampled, and what comes out
//     depends on where the rect edge happens to fall between buffer texel
//     centres. That is a property of the BUFFER, so these are in BUFFER px and
//     are NOT converted with uResolutionScale.
//
//     Getting that backwards is what made a 1 px line at scale 0.5 disagree
//     with the same line at 1.0. sigma was floored only by the converted
//     stated constant, which at scale 0.5 is 0.25 buffer px, so the line sat
//     under the buffer's Nyquist limit and its peak swung with the rect's
//     sub-pixel position - 217 on a texel centre, 180 half a texel off,
//     against a 1.0 reference that holds 230-241 at every position.
//
//     THE FLOOR IS NOT A FIXED HALF WIDTH. It was one - a flat 0.5 buffer px -
//     and a fixed half width asks the wrong question. What survives the blit
//     is not decided by the profile's width at half maximum; it is decided by
//     how much signal the NEIGHBOURING buffer texel still carries, because
//     that is what the bilinear filter rebuilds the peak from. So the floor
//     says exactly that: the profile must still be at
//     FILAMENT_NYQUIST_MIN_SHARE of its peak, FILAMENT_NYQUIST_SAMPLE_PX out.
//     Inverting the generalized Gaussian for sigma gives
//
//         sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
//
//     which neon.frag and NeonRenderer::setupGeometry both evaluate.
//
//     0.0625 and 1.0 are today's behaviour restated, not a retune: at the
//     DEFAULT falloff (N = 2) the expression is exactly 0.5 buffer px, the
//     constant it replaces, so nothing at or near the default moves.
//
//     The two questions diverge at a SOFT falloff, and that is what the change
//     is for. sigma does not only set the core - it multiplies reachSigmas, so
//     it sets the whole tail. At filamentFalloff 0.27 (N = 0.54) reachSigmas
//     clamps at FILAMENT_REACH_MAX_SIGMAS and the tail is 64 sigmas, so a flat
//     0.5 floor stretched a 31 px filament to 61 px at scale 0.5 and past
//     120 px at 0.25 - to buy 5 levels of peak accuracy (max per-phase error
//     13 -> 8) on a profile 31 px wide that the buffer was sampling perfectly
//     well. The share form asks for 0.082 buffer px there, below what the
//     caller already supplies, so it does not engage.
//
//     Where it DOES still engage it is doing the work it was added for: at the
//     default falloff, without any floor, the peak collapses to 153 against a
//     235 reference, and at filamentFalloff 1.5 to 24. The known cost of the
//     share form is the band around filamentFalloff 0.4 to 0.5, where it stops
//     engaging while the unfloored peak error is still 12 to 18 levels;
//     tightening MIN_SHARE to cover that would stop the default case being
//     bit-identical, so the crossover sits here deliberately. See
//     docs/corner-crease-and-filament-nyquist.md section 2.8.
//
//     The line does read WIDER at a reduced scale wherever the floor engages,
//     and that part is not a defect to tune away: a line the buffer cannot
//     sample cannot be reconstructed from it. The floor buys a stable,
//     correctly-bright line of the narrowest width the buffer can actually
//     carry. Deliberately NOT paired with an amplitude compensation to
//     conserve the line's integral - the grade at the end of neon.frag
//     tonemaps per-fragment INSIDE the buffer and FILAMENT_GAIN puts the core
//     deep in saturation, so scaling the linear amplitude barely moves the
//     8-bit value while it does measurably dim the peak (error 15-23/255
//     against 0-10 without it). See
//     docs/corner-crease-and-filament-nyquist.md section 2.
//
//     GATED to scales below 1.0 in both consumers. At scale 1.0 the reduced
//     buffer is full size and the blit copies it 1:1, so there is nothing to
//     survive, and the ring always shades at 1.0. The old flat constant was a
//     no-op at 1.0 by arithmetic
//     coincidence - it equalled the converted stated floor; this expression
//     would not be, above N = 2, so the gate is explicit now. ---
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_NYQUIST_SAMPLE_PX 1.0
#define FILAMENT_NYQUIST_MIN_SHARE 0.0625
#define FILAMENT_GAIN             12.0

// --- Continuous-arc filament gate feathers (neon.frag).
//     INWARD FEATHER: the smooth ramp sits INSIDE the arc's own perimeter
//     span, so nothing outside the arc gets lit -> no corner bleed regardless
//     of width, no perpendicular spike, and the profile is a plain smoothstep
//     that reads the same on straight edges and at corners. Trade-off: the
//     visible arc appears inset by these widths (arc lights up at
//     start + TAIL_FEATHER_PX and ends at start + length - HEAD_FEATHER_PX).
//     Values are pixel-space spans, divided by the current perimeter at the
//     call site, and capped per-arc by ARC_FEATHER_MAX_SHARE below. ---
#define HEAD_FEATHER_PX           14.0
#define TAIL_FEATHER_PX           14.0

// --- Cap on each feather, as a share of the arc's own length.
//
//     The feathers above are a fixed pixel span while an arc's length is a
//     FRACTION of the perimeter, so the same arc config is a different pixel
//     length on every rect. Below ~(HEAD + TAIL) px the two ramps overlapped
//     and ate into the arc's peak: an L = 0.02 arc peaked at 1.00 on an
//     800x600 rect but only 0.21 on a 200x150 one - the last place rect size
//     still reached brightness. Capping each feather at a share of the arc
//     length holds the peak at exactly 1.0 for any length at any size; a short
//     arc gets a proportionally shorter ramp instead of a truncated top.
//
//     0.4 leaves a 0.2 * length plateau at full brightness between the two
//     ramps. Up to 0.5 also holds the peak, but with no plateau the arc reads
//     as a spike rather than a flat-topped segment. Arcs longer than
//     (HEAD + TAIL) / 0.4 px never reach the cap, so the normal case is
//     bit-identical to before. ---
#define ARC_FEATHER_MAX_SHARE     0.4

// --- Halo (sharp coloured glow).
//
//     The halo and bloom are evaluated ANALYTICALLY from the rounded-box SDF
//     distance, not summed over the perimeter gather. For a locally straight
//     emitter of unit density the old sums converge exactly to:
//
//       sum g*sqrt(g) * spacing*kh^2*HALO_NORM  -> HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
//       sum 1/(dd+bw^2) * spacing*bw*BLOOM_NORM -> BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
//
//     so the NORM factors keep their meaning and their calibration: peak
//     values at ad = 0 are unchanged (0.86 and 1.005 respectively).
//
//     The closed form is what makes the effect geometry-independent. The
//     gather had to floor its kernel at a multiple of sampleSpacing to stop
//     128 discrete samples beading into dots, and sampleSpacing is
//     perimeter / NEON_MAX_LOOP_SAMPLES - so halo width, and via the gate its
//     brightness, both tracked the rect size, and glowRadius did nothing at
//     all until it exceeded the floor (~56 px on a 1920x1080 rect, i.e. most
//     of its usable range). An analytic profile cannot bead at any radius, so
//     no floor is needed and glowRadius sets the width directly.
//
//     Those two limits are of an INFINITE emitter, and evaluating one of them
//     at ad = the nearest-edge distance is what used to produce the interior
//     medial-axis creases - the dark wedges running in from each corner, plus
//     the central spine. A fragment on a corner diagonal has two edges equally
//     near and was lit by exactly one of them: measured against a fragment with
//     a single edge at the same distance, the ratio was 1.000 at every distance
//     tested, where physics says roughly 2.
//
//     neon.frag now sums the FINITE-SEGMENT form of the same two integrals over
//     the four straight edges instead (haloSegment / bloomSegment). Integrating
//     the same kernels from t1 to t2 along a segment at perpendicular distance
//     a gives elementary antiderivatives, and as t1 -> -inf, t2 -> +inf each one
//     reduces to exactly the limit above - so this is a strict generalisation
//     and the NORM factors keep the calibration they already had. The corner
//     sweep went from a V kinked at exactly 45 degrees (21 levels deep at
//     r = 160) to a smooth basin of 12, which is genuine falloff rather than a
//     crease. Cost is ~1.06-1.10x of the neon frame; the gather loop is still
//     the overwhelming majority of it.
//
//     The CREASE is what the four-segment form was adopted for. It is not the
//     largest thing it moves. The old expressions were functions of ad and of
//     nothing else, so the old profile was SYMMETRIC about the line - a
//     fragment 120 px inside the edge and one 120 px outside were lit
//     identically, by construction. Outside, the emitter recedes; inside, it
//     wraps around you. The segment sum knows the difference, so the exterior
//     now falls off slightly faster (mid-edge peak itself moves by 2/255) and
//     the INTERIOR stops falling off past roughly one rect-half and settles on
//     a floor. On the 1000x500 / glowRadius 60 probe the centre goes 162 ->
//     186 and the interior mean 174.5 -> 188.8; at glowRadius 30 on 800x400 it
//     is 81 -> 113. The glow reads as bigger, and the part that grew is the
//     interior - the exterior mean goes slightly DOWN.
//
//     Intended, and not tunable back out: a real rectangular tube does light
//     its own interior, and HALO_GAIN cannot be lowered to undo it without
//     dimming the line with it. The knobs for a perimeter-hugging glow are the
//     ones that always meant that - insideCutoff, and glowSide OUTSIDE.
//
//     THE CORNERS ARE A FIFTH THROUGH EIGHTH SEGMENT, not an extension of the
//     four. The straights are trimmed to their TANGENT POINTS, and each quarter
//     arc contributes one more segment: the arc DEVELOPED onto its own tangent
//     at whichever arc point is nearest the fragment, carrying the arc's full
//     length and split about that point, so it abuts the straights exactly in
//     arclength and the emitter is continuous. See neon.frag's
//     arcTangentSegment.
//
//     DEVELOPED AT RATE sqrt(rho * min(rho, r)), NOT AT RATE r, with the
//     measure put back by scaling the segment by r/rate - `rho` being the
//     fragment's distance from the arc centre. Laying an arc out one unit of
//     tangent per unit of arc is only right for a fragment ON it; the exact
//     distance to the point dphi away is a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,
//     whose slope at the foot is sqrt(rho*r). Rate r cost two things:
//
//       - at the CENTRE OF CURVATURE the whole arc ran off to one side of the
//         foot, and both kernels peak at t = 0, so it under-counted - to 54%
//         of the true value on a circle, where all four centres coincide at
//         the middle of the shape;
//       - and it CREASED, because the arc's endpoints slide at the tangent
//         rate on one side of the clamp and at rate 1 on the other, which
//         agree only where the rate is length(w). Every other point of the two
//         lines through an arc centre carried a C1 kink: an L-shaped seam per
//         corner, and on a circle an unmistakable dark cross. See
//         docs/review-findings.md V12.
//
//     rho == r gives rate r and weight 1, so a fragment on the tube is
//     bit-identical to the rate-r form and this calibration is untouched. The
//     whole emitter's worst error against a numerically integrated perimeter
//     falls on every geometry tested (16.5 -> 9.8% on a 600x400 r=40,
//     37.3 -> 26.3% on a circle).
//
//     The straights used to run to the SHARP corner instead, on the grounds
//     that over-extending them past the tangent point stood in for the arc that
//     has no elementary closed form. It does not stand in for it. The
//     over-extension is a phantom emitter a few px from a fragment that is tens
//     of px from the real tube: on an 800x400 rect at cornerRadius 40, a point
//     20 px outside the arc on the diagonal sits 2.4 px from EACH of the two
//     phantoms, and rendered 98 -> 146 across the commit that introduced them.
//     Against a numerically integrated rounded-rect perimeter it was 40 levels
//     too dark ON the tube and 50 too bright just outside it, which is what
//     made a rounded corner read square at a narrow glowRadius.
//
//     The developed arc costs one extra segment per corner - one atan and a
//     length on top - and takes the worst error over a quadrant from 76 to 6
//     levels at glowRadius 5, 112 to 6 at cornerRadius 120, 130 to 8 at 200.
//     A full circle (cornerRadius == halfMin) is still the residual case,
//     since its perimeter is four developed arcs and no straights at all.
//
//     Gated on uCornerRadius > 0, which is a UNIFORM - safe branching, and the
//     reason a sharp rect pays nothing and stays bit-identical (nine scenes
//     cmp-equal, not argued, and still cmp-equal after the rate fix above).
//     Rounded costs 1.19x of the neon pass against no arcs at all, and the rate
//     fix adds 1.03x on top - but the sharp path pays 1.026x of THAT for code
//     it never runs, because what decides this block is REGISTER PRESSURE, not
//     structure: with a per-arc bloom pedestal it was heavy enough to cost the
//     sharp path 1.14x. Re-time a cornerRadius 0 scene as well as a rounded one
//     after touching this. See docs/corner-crease-and-filament-nyquist.md
//     sections 1.7 to 1.9, and docs/review-findings.md V4, V10 and V12. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Width of the COLOUR gather kernel, as a FRACTION OF THE PERIMETER.
//
//     The perimeter colour blend is the one part of the shader that is still a
//     discrete sum over the loop samples, and it is the only place a length has
//     to be expressed this way rather than in pixels. The reason is that the
//     signal it filters - the gradient LUT - is itself parameterised by
//     perimeter fraction: sample i contributes LUT(i / NEON_MAX_LOOP_SAMPLES).
//     A kernel measured in pixels therefore covers a DIFFERENT span of the
//     gradient on every geometry, and the same colour stops render washed out
//     on a small rect and crisp on a large one. Measured on the stock 4-stop
//     ring, the colour sampled exactly on the red stop ran (0.95, 0.33, 0.06)
//     at 200x150 against (0.96, 0.04, 0.00) at 2800x2200 - roughly 8x the hue
//     bleed from geometry alone. Scaling the kernel with the perimeter is what
//     makes the gradient read identically at any size.
//
//     Two further properties fall out of the same choice:
//
//       - A constant anti-bead margin. The kernel has to stay >= about one
//         sample spacing, or it collapses between samples and the blend beads
//         into dots. Spacing is perimeter / NEON_MAX_LOOP_SAMPLES, so a
//         perimeter fraction pins the ratio at
//         COLOR_BLEND_PERIM_FRAC * NEON_MAX_LOOP_SAMPLES = 1.13 spacings on
//         every geometry, where the previous fixed 24 px span met the bound
//         only up to a ~3000 px perimeter and was down to 0.31 spacings by
//         9900 px. (No beading was actually measurable there - a DFT of the
//         hue around the perimeter put the 128-cycle component at the noise
//         floor - because the gradient varies slowly over one spacing on a
//         rect that large. The margin is a guarantee for dense colour stops,
//         not a fix for an observed artifact.)
//
//       - Sample-count independence. The kernel comes from the
//         NEON_MAX_LOOP_SAMPLES-based fraction rather than from the runtime
//         sample count, so the numSamples knob does not move the colour
//         blend. (An earlier sampleSpacing-derived floor divided by the live
//         count and landed 2x wider at reduced counts.)
//
//     Deliberately NOT coupled to glowRadius. The gather produces colour only -
//     the halo and bloom have been closed-form since they stopped riding on it -
//     so a wide glow has no reason to desaturate the gradient, and the old
//     max(glowRadius, floor) reintroduced exactly the pixel-space dependence
//     this constant exists to remove.
//
//     0.0088 puts the kernel at 24.0 px on the stock 800x600 / radius 40
//     geometry, matching the previous fixed span, so default-sized output is
//     unchanged.
//
//     NOTE: unit-free, unlike the px constants around it. The shader
//     multiplies it by a perimeter that is already in scaled px, so it needs
//     no uResolutionScale correction - applying one would double-apply. ---
#define COLOR_BLEND_PERIM_FRAC    0.0088

// --- Emission on/off ramp. glowRadius = 0 must read as "filament only", but
//     an analytic profile at radius 0 is a sub-pixel spike of full height
//     rather than nothing, so the halo and bloom fade in over
//     glowRadius = [0, this]. A FIXED pixel width: gating against the
//     sampleSpacing-derived floor instead would re-couple brightness to the
//     rect size, which is the whole thing this design removes.
//
//     NOTE: full-res pixel span, compared against a uGlowRadius that arrives
//     already scaled, so the shader multiplies this by uResolutionScale at
//     the point of use. Identity at scale 1.0. ---
#define GLOW_GATE_FADE_PX         2.0

// --- Lower bound on the analytic emission widths. Guards the divides only;
//     anything this small is already multiplied out by GLOW_GATE_FADE_PX. ---
#define EMISSION_MIN_WIDTH        1e-3

// --- Degenerate guard for the corner arc's tangent frame (neon.frag,
//     arcTangentSegment). The frame is built by normalising the fragment's
//     offset from the arc centre, clamped into the quarter the arc occupies;
//     that offset is the zero vector only for a fragment sitting exactly on an
//     arc centre, where the direction is undefined and atan(0, 0) is undefined
//     with it. Below this the frame falls back to the nearer of the two tangent
//     points, which is the correct clamp for the whole region the guard covers.
//
//     It floors `rho` in the same function, for the same fragment: the
//     development rate divides into the segment's weight, so an exactly-zero
//     rho would be a division by zero. The floored value is nowhere near a
//     pixel and the limit it lands on is the exact answer anyway, so this only
//     keeps a NaN out.
//
//     Unit-free: compared against a length in the shader's own px space, and
//     small enough that nothing but the exact degeneracy reaches it. ---
#define ARC_FRAME_EPSILON         1e-6

// --- Bloom (wide background spill). See the halo note above for the closed
//     form; BLOOM_SPACING_FLOOR is gone with the gather. ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_NORM_FACTOR         0.32

// --- Travelling-segment array size (shared by C++ vector cap + GLSL uniform array) ---
#define MAX_SEGMENT_BOOSTS        8

// --- Arc range array size (shared by C++ vector cap + GLSL uniform array).
//     Each arc gates a slice of the perimeter with its own colour + intensity.
//     Overlap resolves winner-take-all (max mask*intensity wins). ---
#define MAX_ARCS                  8

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO and
//     the shader's array. The gather iterates only uNumSamples of them
//     (NeonConfig::numSamples, which defaults to this), so it is a ceiling,
//     not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Glow coverage table (neon-glow-cover.frag, V20 and V21): for each piece
//     of the emitter and each fragment position round it, the arcs' and the
//     segments' coverage of that piece as the halo and the bloom see it. Laid
//     out in neon-pieces.glsl: four bands of ROWS rows, each holding one
//     straight at its left and one corner at its right. A band's columns are
//     shared between the two in proportion to their lengths (uGlowCoverSplit,
//     NeonRenderer::GetGlowCoverSplit), so a circle's corners get the columns
//     its straights do not need and a long rect's straights get its corners':
//     the straight has OVERHANG columns past each end, the corner
//     CORNER_OVERHANG on each side of its arc and a guard texel at each end of
//     its block, and the columns left over (the layout's `shared`) are split
//     between the two interiors, each keeping at least MIN_INTERIOR. ROWS 32 is half of them
//     within one halo width of a straight, and 16 inside and 16 outside a
//     corner's arc. 1024 x 128 RGBA16F is 1.0 MB - half V20's single table per
//     perimeter. Sizing it, against an exact per-piece reference (V21 in
//     docs/review-findings.md): 48 rows (1.5 MB) read at most 2 levels off where
//     32 reads 3 on a few pixels of a segment scene, for 1.3x the bake's cost
//     under animation; 768 columns read 6 levels off a 1 px halo on a 4K-sized
//     rect.
//
//     The WIDTH is not a constant: the table's layout is a parameter
//     (NeonRenderer's GlowCoverLayout, the shaders' uGlowCoverLayout), between
//     MIN_WIDTH and MAX_WIDTH in steps of WIDTH_STEP. ROWS and HEIGHT are. ---
#define GLOW_COVER_MAX_WIDTH      1024
#define GLOW_COVER_MIN_WIDTH      256
#define GLOW_COVER_WIDTH_STEP     64
#define GLOW_COVER_ROWS           32
#define GLOW_COVER_OVERHANG       64
#define GLOW_COVER_CORNER_OVERHANG 32
#define GLOW_COVER_MIN_INTERIOR   16
#define GLOW_COVER_HEIGHT         (4 * GLOW_COVER_ROWS)

// --- The glow coverage table's width (NeonRenderer's GetGlowCoverWidth):
//     enough interior columns that the longest band - its straight plus its
//     quarter arc, L full-res px - gets one every S px, S growing with the
//     halo width kh (max(glowRadius, EMISSION_MIN_WIDTH), px):
//
//         S    = clamp(PX_PER_COLUMN_PER_KH * kh, MIN_PX_PER_COLUMN, MAX_PX_PER_COLUMN)
//         need = 2 + 2 OVERHANG + 2 CORNER_OVERHANG + ceil(L / S)
//         W    = clamp(need rounded up to WIDTH_STEP, MIN_WIDTH, MAX_WIDTH)
//
//     In full-res px because the edge ring reads the table at full
//     resolution; lengths enter the layout only as ratios, so every scale
//     shares one table.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the 1024-wide
//     table (docs/neon-glow-cover-resolution-plan.md step 2): 250 configs -
//     rects 200 x 120 to 3600 x 2000, glowRadius 1 / 2 / 5 / 10 / 20,
//     cornerRadius 0 / 40 / a circle, both windings, three partial arcs with
//     and without colour stops and with two segments, at scales 1.0 / 0.5 /
//     0.25 - each at every width from 256 to 960, the frame after a light
//     change, against the same frame at 1024. The widest spacing that still
//     met the plan's criterion (max 2 levels, 99.9% of lit pixels within 1)
//     on every config was ~2.2 px per column at kh 1, 2.5 at 2, 4.6 at 5, 6.8
//     at 10 and 10 at 20. These constants sit under that envelope - 12% at
//     kh 5, 27% or more elsewhere - and at the widths they give, every config
//     read at most 2 levels with at least 99.988% of lit pixels within 1.
//     Spacing 20% wider failed at kh 5 (4 levels on a 200 px circle). A
//     narrow glow on a large rect still gets MAX_WIDTH: at kh 1-2 a
//     3600 x 2000 rect needed all 1024 columns. ---
#define GLOW_COVER_PX_PER_COLUMN_PER_KH 0.8
#define GLOW_COVER_MIN_PX_PER_COLUMN    1.6
#define GLOW_COVER_MAX_PX_PER_COLUMN    5.0

// --- The segment table (NeonRenderer's GetGlowCoverSegmentWidth,
//     neon-glow-cover-fill.frag): the segments' two channels of the glow
//     coverage table, baked at a width of their own and copied in. A bell's
//     standard deviation is 0.3536 x its length x the perimeter - ~100 px for
//     a length-0.05 segment on a 1840 x 1000 rect, against an arc's 14 px
//     feather - so it needs far fewer columns than the arcs:
//
//         sigma = 0.3536 x the shortest lit segment's length x perimeter   (full-res px)
//         need  = 2 + 2 SEG_OVERHANG + 2 SEG_CORNER_OVERHANG + ceil(L / (sigma / SEG_COLUMNS_PER_SIGMA))
//         W     = clamp(need rounded up to WIDTH_STEP, SEG_MIN_WIDTH, the main table's width)
//
//     with L the longest band, as for the main table. Where W would be at
//     least SEG_DIRECT_SHARE of the main table's width (very short segments)
//     the separate table saves nothing, and the segments are baked into the
//     main table directly, as before it existed.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the segments
//     baked into the main table directly (plan step 3): 135 configs - rects
//     200 x 120 to 3600 x 2000, glowRadius 2 / 5 / 10, cornerRadius 0 / 40 /
//     a circle, scales 1.0 / 0.5 / 0.25, segments of length 0.01-0.2 with
//     boosts 0.5-2, overlapping, abutting, over the corners, travelling, and
//     under a partial arc - the frame after a light change, the segment table
//     forced on. With overhangs 16 / 8, every COLUMNS_PER_SIGMA from 3 to 8
//     read at most 2 levels with 99.9992% of lit pixels within 1; 2 read 3
//     levels on a thin glow at 3840 x 2160. 4 is one step of margin. Overhangs
//     8 / 4 held at every value tried (99.988%), 32 / 16 read at most 1 level
//     from 4 up but do not fit SEG_MIN_WIDTH. ---
#define GLOW_COVER_SEG_COLUMNS_PER_SIGMA 4.0

// --- How far the glow coverage bake integrates a segment's bell either side
//     of its centre, in units of 1 / invSigma (the bell is exp(-(d invSigma)^2),
//     so 3 is 4.24 standard deviations, where it has fallen to 1.2e-4 of its
//     peak). The bake (neon-glow-cover.frag, segmentsOnPiece) cuts the bell
//     here and NeonRenderer's GetGlowCoverDirtySegmentPieces mirrors it to
//     find the pieces a moved segment reaches: change one and the other
//     follows, or a piece the bell reaches is not re-baked. It was 5 / sqrt(2)
//     (5 sigma); at 3 a moved segment dirties fewer pieces, which took the
//     segment table's bake on a band of 8 segments from 0.75 to 0.58 (timer
//     units, AMD Radeon Pro 5300M), at most 1 level, on up to 1.6% of a
//     frame's lit pixels (0.15% on average over the cover set; I61 in
//     docs/review-findings.md). An 8-node rule on top halved it again but
//     read 3 levels off a long, bright segment, and was not taken. ---
#define GLOW_COVER_BELL_REACH            3.0
#define GLOW_COVER_SEG_OVERHANG          16
#define GLOW_COVER_SEG_CORNER_OVERHANG   8
#define GLOW_COVER_SEG_MIN_WIDTH         128
#define GLOW_COVER_SEG_DIRECT_SHARE      0.75

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- The output dither (R7), peak to peak in 8-bit levels: neonDither
//     (neon-grade.glsl) adds noise in +/- half this to every colour the neon
//     writes to the caller's framebuffer - the blit, the edge ring and the
//     ring's field composite, never an offscreen buffer. The halo and bloom
//     are slow gradients that cross a level only every few px, up to tens of
//     px in the dark tail, so an undithered write draws concentric contour
//     rings around the rect. 1.0 is the least that removes them; 0 turns the
//     dither off. It assumes an 8-bit target. ---
#define OUTPUT_DITHER_LSB         1.0

// --- Epsilons ---
#define WSUM_EPSILON              1e-6

// --- One-sided cut guard band, in BUFFER pixels. Scaled path only.
//
//     One of the two constants in this file stated in buffer px rather than
//     full-res px (BLIT_CUTOFF_GUARD_PX below is the other): it describes the
//     BUFFER's own sampling, so it must NOT be converted with uResolutionScale.
//
//     For pass 1b the one-sided cut is not applied by neon.frag at all -
//     neon-blit.frag applies it at DESTINATION resolution, where a pixel is a
//     pixel and the edge can land exactly where the full-resolution shading
//     puts it. What neon.frag still does is cull the dark side, and the bound
//     it culls at cannot be the cut itself: the blit reconstructs each
//     destination pixel from the 2x2 buffer texels around it, so a texel
//     killed at the cut leaves the first LIT destination pixel rebuilt partly
//     from black. Measured at scale 0.5, glowSide OUTSIDE: the first lit pixel
//     came back at 178 against the 239 full resolution puts there, a dark seam
//     hugging the inside of the glow's own edge.
//
//     So the cull runs this far PAST the cut, and the guard band's emission is
//     what the blit's filter reconstructs the boundary from. One buffer texel
//     is the filter's reach; 2.0 covers that with room for the diagonal, where
//     fwidth of an SDF is sqrt(2) rather than 1, and for the half-texel of
//     phase between the cut and the nearest texel centre.
//
//     The band costs a two-texel strip along the perimeter - nothing beside
//     the half-plane the cull still removes - and none of it is visible: the
//     blit's mask is zero everywhere past the cut, including at the hard step
//     where this bound finally discards.
//
//     NOT used by the edge ring, which owns its own cut (uBlitOwnsCut 0). See
//     neon.frag's sideCull and the post-grade cut block. ---
#define BLIT_SIDE_GUARD_PX        2.0

// --- Cutoff guard band, in BUFFER pixels. Pass 1b only.
//
//     BLIT_SIDE_GUARD_PX's argument, applied to the inside/outside cutoffs.
//     For pass 1b neon-blit.frag applies the cutoff masks at
//     DESTINATION resolution, and neon.frag only culls - this far past the end
//     of each ramp, so the blit's bilinear filter rebuilds every boundary from
//     lit texels rather than from black. Same value as the side guard, for the
//     same reasons: one buffer texel of filter reach, plus room for the
//     diagonal and for the phase between the boundary and the nearest texel
//     centre.
//
//     It replaces CUTOFF_SOFT_FLOOR_PX, a feather in buffer px that let a mask
//     drawn INTO the buffer place its boundary sub-texel. That was the best the
//     buffer could do, and it still softened the boundary across a buffer
//     texel and snapped it toward the buffer grid: on the bounded_band scene of
//     docs/neon-resolution-scale-comparison.html it read p99 41/255 against the
//     1.0 render at scale 0.5, and 69 at 0.25, and the band spread past its
//     own cutoffs as the scale fell (50,703 lit px at 0.5 and 53,568 at 0.25,
//     against 47,012 at 1.0). Drawn by the blit instead it reads p99 4 at 0.5
//     and 12 at 0.25, with 47,012 lit px at every scale; what is left is the
//     reduced-resolution glow inside the band, not its edges. See
//     docs/neon-resolution-scale-plan.md, step 2.
//
//     NOT used by the edge ring, whose masks and culls are its own. See
//     neon.frag's cutGuard and NeonRenderer::setupGeometry's cutGuardPx. ---
#define BLIT_CUTOFF_GUARD_PX      2.0

// --- Edge ring width. CPU only.
//
//     At every scale, NeonRenderer redraws a ring around the rect
//     edge at FULL resolution (neon.frag's ring program),
//     reading only the gather's result from the gather buffer. The ring reaches R
//     full-res px either side of the edge (GetRingWidth in neon-renderer.cpp):
//
//         R = max(reach(1.0), reach(scale)) + RING_GUARD_TEXELS / scale
//
//     where reach(s) is the filament's reach as neon.frag computes it at scale
//     s, in full-res px. BOTH, because inside R the reduced pass's filament -
//     floored to what its buffer can sample, so wider than the real one
//     whenever the line is thin - is replaced by the real one, and R has to
//     clear whichever reaches further. Uncapped: the reach is where a filament
//     has fallen to FILAMENT_CUTOFF of its gain, about 2/255 after the grade,
//     which is exactly how far its error can show. RING_GUARD_TEXELS adds the
//     reduced buffer's bilinear footprint past that.
//
//     Calibrated over lineWidth {1, 2, 4, 8, 16} x filamentFalloff
//     {0.5, 1, 2, 4} x glowRadius {0, 2, 5, 20}, each config rendered at
//     scales 0.75, 0.5 and 0.25 and compared against its own 1.0 render
//     (docs/neon-resolution-scale-plan.md, step 5). Worst error over all 80:
//
//       RING_GUARD_TEXELS    scale 0.75    scale 0.5    scale 0.25
//              0.5               3            12            3
//              1.0               4             2            2
//              2.0               2             2            2
//
//     1.0 is the smallest that holds 2/255 at 0.5 and 0.25, the targets. 2.0
//     also brings 0.75 within 2 (its worst at 1.0 is a 1 px flat-top line
//     with no glow) for a ring one buffer texel wider each side.
//
//     The two things that made the first version of this ring fail that sweep,
//     both now handled: R used to cover only the full-res filament (fixed by
//     taking the reduced pass's too, above), and at a small glowRadius pass 1's
//     quad stopped short of R, so the ring read gather texels nothing wrote
//     (fixed by covering the ring with the quad; since the gather was split
//     out, the gather pass's own quad, built in setupRingGeometry, covers
//     it). ---
#define RING_GUARD_TEXELS         1.0

// --- Gather resolution. CPU only.
//
//     At every resolutionScale the gather - ~95% of neon.frag's cost when it
//     ran inline - runs in a pass of its own (neon-gather.frag) at its own
//     scale, and pass 1 and the
//     edge ring shade from its result. Its scale is (GetGatherScale in
//     neon-renderer.cpp)
//
//         clamp(GATHER_TEXELS_PER_KERNEL / kc, GATHER_MIN_SCALE,
//               min(resolutionScale, GATHER_MAX_SCALE))
//
//     with kc the colour kernel's width in full-res px, perimeter *
//     COLOR_BLEND_PERIM_FRAC: the gather's outputs are Lorentzian-weighted
//     means whose kernel is never narrower than kc, so this many texels per kc
//     carries them through a bilinear read.
//
//     Calibrated on Mesa llvmpipe with an RGBA16F gather buffer (worst error
//     against 1.0 over `probe scenes`, and the default scene's frame time):
//
//       GATHER_TEXELS_PER_KERNEL   worst error   default 0.5   default 0.25
//                1.0                   4              -              -
//                1.5                   2           10.5 ms        8.0 ms
//                2.0                  1-2          10.8 ms        7.3 ms
//                4.0                   -           14-17 ms      11-12 ms
//         (gather at resolutionScale)  1-2          37.5 ms       12.3 ms
//
//     2.0 is the knee: 1.5 saves nothing measurable (the gather is no longer
//     the pass that costs) and 4.0 costs half as much again for no visible
//     gain. With an RGBA8 gather buffer - the fallback - 2.0 reads 3/255.
//     GATHER_MIN_SCALE barely matters - full-viewport rects
//     at 720p and 1080p read 1-2 at a floor of 0 and 0.0625 alike - and is
//     kept as a cheap guard: the gather buffer is clipped to the viewport, so
//     at the floor it is at most 128 x 80 texels at 1080p. See
//     docs/neon-resolution-scale-plan.md section 13. ---
#define GATHER_TEXELS_PER_KERNEL  2.0
#define GATHER_MIN_SCALE          0.0625

// --- The gather's ceiling. CPU only.
//
//     GetGatherScale never gathers finer than this, at any resolutionScale.
//     It binds only on a rect whose perimeter is under ~450 px (kc under 4
//     px), which would otherwise gather near full resolution: its gather
//     buffer - RGBA16F over the glow quad's box, two attachments with
//     segments - ran to 16 / 33 MB at 1080p for a 40 x 24 rect whose glow
//     fills the frame. At 0.5 that is a quarter, the same as the scaled path
//     at 0.5 always gathered at, and the check suite's smallest rect (perimeter
//     478 px) is not touched.
//
//     It replaces the split gate at 1.0 (FULL_RES_SPLIT_GATHER_MAX_SCALE /
//     FULL_RES_SPLIT_MIN_AREA_PX, removed - docs/neon-shader-cleanup-plan.md
//     step 3), which ran the gather INLINE in neon.frag where splitting it
//     out did not pay: on an AMD Radeon Pro 5300M, where the first offscreen
//     pass of a frame costs ~0.15 ms, for a still cutoff band under ~140k px
//     of quad and for a gather near full resolution. Since unchanged frames
//     stopped re-running the gather (I40) the still case no longer leaves
//     the caller's framebuffer at all; an Apple M2 Pro measured the split
//     1.1-4.75x faster at every size and mode, and the AMD, re-measured with
//     every rect split against its own gate, 0.98-1.02x in every animated
//     mode with one loss - a cutoff band under an intensity pulse, 0.84x.
//     So every scale splits, and the inline loop went with the gate. ---
#define GATHER_MAX_SCALE          0.5

// --- Stand-in distance for a cutoff that glowSide has already subsumed.
//
//     neon.frag hands this to the band distance on the side the one-sided cut
//     culls, so the discard and the mask there no-op through the arithmetic
//     exactly as a DISABLED cutoff does - same mechanism, same sentinel shape,
//     no extra branch downstream.
//
//     Matches CUTOFF_DISABLED_SIZE in neon-renderer.cpp, which is what a
//     disabled cutoff actually arrives as. It is a distance in the shader's own
//     px space and only has to be far enough that no realistic geometry reaches
//     it; see the note on that constant for the same reasoning. ---
#define CUTOFF_NEUTRALISED        1.0e6

// --- Glow reach (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity).
//
//     Named for the reach, not for an early-out: the per-fragment
//     `ad > earlyOut -> discard` this constant was originally calibrated for
//     no longer exists. Geometry culls the far region instead, which is
//     tiler-friendly, so what the factor sets is how far the glow is allowed to
//     reach before the quad stops covering it. The locals it feeds already say
//     so - `glowReach` in both setupGeometry implementations, `reach` in both
//     shaders.
//
//     glowRadius only: the companion sampleSpacing * SPACING_FACTOR term is
//     gone. sampleSpacing is perimeter / NEON_MAX_LOOP_SAMPLES, so it won on
//     any reasonably large rect at default glowRadius and made both the
//     distance the bloom got truncated at and the brightness it still had
//     there track the rect size. The bloom's reach is a function of glowRadius
//     alone, so that is all that sizes the quad.
//
//     Used by the renderers' setupGeometry AND by the shaders, which recompute
//     the same expression to place the bloom pedestal that lets this margin
//     stay tight without the truncation showing. Keep the two in step. ---
#define GLOW_REACH_RADIUS_FACTOR  48.0

// --- Where the shaders' quad-edge fade begins, as a FRACTION of the quad
//     margin. The emission ramps to zero over [FRAC * margin, margin], so the
//     bloom's 1/D tail - still ~10% of peak out at the quad edge - never clips
//     as a hard rectangle.
//
//     A fraction, not a pixel span, because the margin is proportional to
//     glowRadius (see GLOW_REACH_RADIUS_FACTOR) and so is the bloom profile it
//     hides. A fraction keeps the ramp at a constant proportion of the bloom's
//     reach, so the fade reads the same at every glow radius; a fixed px ramp
//     would vanish on a wide glow and dominate a narrow one. At the stock
//     glowRadius 5 / bloom 0.3 / intensity 1 the margin is 312 px, giving a
//     62 px ramp.
//
//     Nothing derives 0.8 - anything leaving a ramp wide enough to hide the
//     clip behaves the same. What it DOES assume is that the margin was set by
//     the glow, where 20% of it is a long distance. When outsideCutoff clamps
//     the margin instead (to where its fade ends, size + softness, plus 1),
//     20% of ~13 px is 2.6 px and
//     the ramp lands INSIDE the cutoff band, dimming the band's outer edge
//     ahead of the cutoff mask - and only on the exterior, since the fade keys
//     on positive d. Both shaders therefore floor the ramp's start at the
//     cutoff boundary whenever that boundary falls inside the quad; see the
//     fadeStart block in neon.frag.
//
//     Since V23 the bloom no longer relies on this fade to end smoothly - each
//     piece fades by its own distance from BLOOM_FADE_START_FRAC, below, which
//     has it under a level well before this ramp starts. ---
#define QUAD_FADE_START_FRAC      0.8

// --- Where each piece's bloom starts fading out, as a FRACTION of `reach`
//     (neon.frag's bloom end fade, V23).
//
//     The pedestal takes the bloom's 1/a tail to exactly 0 at `reach`, but
//     with its slope still on, so the glow stopped on a crease: inside a large
//     rect, a hard-edged dark rectangle `reach` in. The quad-edge fade above
//     hid it outside, but over [0.8, 1.0] of the margin, where the tail is
//     still ~5 levels up, and a fade that late STEEPENS the tail before it
//     flattens it - a shoulder, then flat, which reads as an edge too. So
//     each piece's bloom is multiplied by 1 - smoothstep(FRAC * reach, reach,
//     a) of its own distance, on both sides.
//
//     0.5 is about the latest start that does not steepen it: the steepest
//     slope inside the fade over the slope where it starts, on the
//     infinite-line bloom at glowRadius 5, bloomStrength 0.52, intensity 0.8,
//     is 1.40 at 0.8, 1.14 at 0.6, 1.035 at 0.5 and 1.00 from 0.4 down. (It
//     is also where c * (reach - a)^2 meets the pedestalled 1/a tail in value
//     and slope.) An earlier start costs more of the tail; at 0.5 the glow
//     reads up to 6 levels dimmer in the outer half of its reach. ---
#define BLOOM_FADE_START_FRAC     0.5

// --- Filament reach for the same quad sizing, expressed in sigmas.
//
//     glowRadius = 0 means "filament only" - the halo and bloom are gated off
//     by GLOW_GATE_FADE_PX - but the filament itself is still there, sized by
//     lineWidth, so the quad has to cover it or the exterior gets clipped.
//
//     The reach is NOT a constant, because the profile is
//     exp2(-(ad/sigma)^N) with N = 2 * filamentFalloff and the slider runs
//     down to falloff 0. Solving core * FILAMENT_GAIN < FILAMENT_CUTOFF gives
//
//         reach = sigma * log2(FILAMENT_GAIN / FILAMENT_CUTOFF) ^ (1/N)
//
//     which is 1.9 sigmas at falloff 2.0, 3.5 at 1.0, 12.6 at 0.5, 67.8 at 0.3
//     and 558 at 0.2 - it diverges as falloff -> 0. A fixed 12 (calibrated for
//     falloff 0.5) left the low end badly under-sized: the interior is
//     unbounded because the quad always covers it, so a soft filament flooded
//     inward while the exterior was chopped at 12 sigmas. That asymmetry is
//     the "no outer glow at glowRadius 0" report.
//
//     MAX_SIGMAS bounds the fill cost where the formula diverges; the shaders
//     pedestal-subtract the core at exactly this reach, so hitting the cap
//     shortens the glow symmetrically on BOTH sides instead of showing a cliff
//     on the outside. MIN_SIGMAS keeps a little room at very high falloff.
//     Keep the expression in step across neon-tuning.h, both setupGeometry
//     implementations, and both fragment shaders. ---
#define FILAMENT_CUTOFF           0.002
#define FILAMENT_REACH_MIN_SIGMAS 2.0
#define FILAMENT_REACH_MAX_SIGMAS 64.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// The neon's grade: the tone map from linear light to the output, shared by
// neon.frag and neon-field.frag, and the output dither, shared by those two and
// neon-blit.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it, after the tuning
// header (TONE_MAP_SHOULDER, GAMMA_EXPONENT, OUTPUT_DITHER_LSB), into the glow,
// the hue-invariant field's composite, which tone-maps col * Fa where neon.frag
// tone-maps its own result - the two have to agree to the bit for the field
// to stand in for pass 1 (I46) - and the blit, which only dithers.
// ---------------------------------------------------------------------------

precision highp float;

// Hue-preserving Reinhard: tonemap the peak channel and scale the others by
// the same ratio. Per-channel tonemap desaturates warm mixes (orange -> peach)
// because R saturates while G/B are still linear; scaling by the peak's
// compression preserves the original R:G:B ratio.
vec3 neonToneMap(vec3 result) {
    float peak = max(max(result.r, result.g), result.b);
    float mapped = peak / (peak + TONE_MAP_SHOULDER);
    result = result * (mapped / max(peak, 1e-6));
    return pow(result, vec3(GAMMA_EXPONENT));
}

// The output dither (R7): interleaved gradient noise in +/- OUTPUT_DITHER_LSB / 2
// levels added to a colour about to be written to the caller's 8-bit target,
// so the rounding there breaks the glow's slow gradients into noise instead of
// contour rings. A pure function of the pixel - no time term - so a frame is
// reproducible, and the blit and the edge ring, which tile one frame, draw one
// noise field across their seam. Colour only: the blend is premultiplied, and
// the same noise on alpha would cancel it over a bright background. Only for
// a write to the caller's framebuffer; an offscreen buffer is read back
// filtered, which averages the noise into blotches.
vec3 neonDither(vec3 c) {
    // A constant test, so OUTPUT_DITHER_LSB 0 compiles the noise out rather
    // than computing it and multiplying it by 0.
    if (OUTPUT_DITHER_LSB <= 0.0) {
        return c;
    }
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    return c + (n - 0.5) * (OUTPUT_DITHER_LSB / 255.0);
}

// ---------------------------------------------------------------------------
// The rounded box's signed distance, its antialiasing width and the cutoff
// band's two boundaries, shared by neon.frag, neon-blit.frag, neon-field.frag
// and black-rect.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it into the four
// programs that have to agree on the shape to the pixel - the glow, the
// scaled path's blit (which re-applies the glow's cut and cutoffs at full
// resolution), the ring field's composite (which re-applies the ring's) and
// the opaque fill (which has to register with the glow's edge) - so the four
// stay one copy. It reads no uniform and no constant.
// ---------------------------------------------------------------------------

precision highp float;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// One pixel's width in sdRoundBox's units, for the box filter across the
// outline: what fwidth(d) returns along a straight stretch of it, taken from
// the distance's analytic gradient instead of from d's own differences.
//
// fwidth(d) differences d across a 2x2 quad, and at a sharp corner that is not
// a derivative. On the vertex pixel the quad can hold BOTH outside neighbours:
// d then steps a whole pixel along each axis, fwidth reads 2, and every ramp
// sized from it doubles there - the corner pixel of a one-sided cut came out
// at 0.84 of the other three corners', the fill's at 0.75. The quad's parity
// decides which corner, so one rect showed it at its top-left and the next at
// no corner at all (V26). Along the inside diagonal it read 0 or 2 the same
// way. The gradient is one-sided by construction - a pixel takes the edge it
// is nearest - so it is 1 on every corner pixel, as along the edges, and
// fwidth's value wherever d is smooth.
//
// |gradient| per axis times that axis's pixel, fwidth(p): exact because every
// caller's p is the rect's own frame through an unrotated orthographic
// transform - vPos, or gl_FragCoord less the centre - so each screen axis
// moves one axis of p. q and length(max(q, 0)) are sdRoundBox's own, so a
// caller taking both pays for the length once. A derivative: take it in
// uniform control flow.
float sdRoundBoxFwidth(vec2 p, vec2 b, float r) {
    vec2  q  = abs(p) - b + r;
    vec2  qp = max(q, 0.0);
    float l  = length(qp);
    vec2  g  = (q.x > 0.0 && q.y > 0.0) ? qp / l
             : ((q.x > q.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    return dot(g, fwidth(p));
}

// --- Band boundary distances -------------------------------------------
// The band's two boundaries, expressed as signed distances: dIn >= 0 means
// "past the inside cutoff", dOut <= 0 means "within the outside cutoff".
//
// INNER: plain Euclidean (d + cut). Inside the shape the rounded-box SDF is
// already per-axis, so the inner boundary is square at cornerRadius 0 and a
// correct parallel curve (radius r - cut) above it. Nothing to fix.
//
// OUTER: Euclidean too whenever cornerRadius > 0, where it is exactly d - cut.
// That is the parallel curve, so the band keeps a uniform width and the opaque
// fill covers precisely as far as the light reaches - no black bulging past
// the glow at the corners.
//
// The one exception is cornerRadius == 0: the parallel curve of a SHARP corner
// is an arc of radius `cut`, so a rect the designer asked to be square comes
// out with rounded outer corners. There, and only there, offset the box
// per-axis instead to keep the corner square. The band is then ~1.41x wider
// measured diagonally across that corner, which is unavoidable - a uniform
// width and a square outer corner cannot both hold at a sharp corner.
//
// Disabled cutoffs arrive as a huge sentinel and still no-op: dIn goes hugely
// positive, dOut hugely negative, so both masks evaluate to 1.
float bandOuterDistance(vec2 p, float d, vec2 halfSize, float r, float cut) {
    if (r > 1e-4) { return d - cut; }
    vec2 b = halfSize + vec2(cut);
    return sdRoundBox(p, b, 0.0);
}
float bandInnerDistance(float d, float cut) {
    return d + cut;
}

precision highp float;

// Pass 2b of the scaled neon path: bilinear composite of the reduced-resolution
// neon FBO (premultiplied colour + coverage alpha) onto the backbuffer, over
// the part of the frame outside the thin edge ring that pass 2c re-shades at
// full resolution - and only where the glow can still be non-zero there: past
// the cut, the cutoffs and the glow's own fade margin this shader would write
// exactly 0, so NeonRenderer::setupRingGeometry, which builds both areas,
// leaves those pixels out. The
// opaque-mode silhouette is handled entirely by the black-rect fullscreen pass
// drawn just before this blit in NeonRenderer::Render - the black quad's
// analytic SDF anti-aliasing lands cleanly on rounded corners regardless of
// softness, whereas the old per-fragment discard here stair-stepped at any
// corner radius > 0.
//
// THE ONE-SIDED CUT IS APPLIED HERE, not in neon.frag, whenever this pass runs
// at all. That is the whole reason this shader knows anything about geometry.
//
// neon.frag rasterises into a buffer at NeonConfig::resolutionScale and this
// pass bilinearly upsamples it, so a cut made in there is a cut made in the
// wrong units: each destination pixel is rebuilt from the 2x2 buffer texels
// around it, and a texel whose centre is on the lit side contributes to
// destination pixels 1/scale away in BOTH directions. Measured at scale 0.5,
// glowSide OUTSIDE, softness 0, with the cut made in neon.frag at d = 0: the
// destination pixel at d = -0.5 - on the DARK side of the line, over backdrop
// an OpaqueMode fill never covers - came back at 59/255. At scale 0.25 the
// same wash ran four destination pixels deep, 105 / 75 / 45 / 15 going inward
// from the line.
//
// No placement inside the buffer fixes that, because the buffer does not
// contain a destination-resolution edge to place. This pass does: it runs
// full-res on the caller's framebuffer, exactly like black-rect.frag, so
// its pixel width here (sdRoundBoxFwidth) is one DESTINATION pixel and the cut lands where the direct
// path puts it. Measured on the same scene after the move, first lit pixel
// against the direct path's 239: scale 0.5 gives 237, scale 0.25 gives 230,
// and the dark side is 0 at both.
//
// The emission it cuts is real rather than reconstructed-from-black because
// neon.frag culls BLIT_SIDE_GUARD_PX past the cut instead of at it, leaving a
// lit guard band for the filter to rebuild the boundary from. See
// neon-tuning.h. The hard step where that cull finally bites sits well inside
// the region this mask zeroes, so it is never visible.
//
// It also puts glowSideSoftness back on a destination-pixel footing, which is
// what makes the feather scale-invariant. Sized in buffer px it lost its whole
// bottom range: at scale 0.25 a softness of 0, 2 and 4 rendered BYTE-IDENTICAL,
// because 4 px is one buffer texel there and the filter is wider than that.
// The first lit pixel now reads 37 / 37 / 36 at softness 4 across scales
// 1.0 / 0.5 / 0.25, against 37 / 56 / 29 with the cut made in the gather and
// everything else held here. That sequence is the point: it tracks neither the
// scale nor the 4 px the caller asked for, it just wanders with where the
// boundary falls between buffer texels.
//
// THE INSIDE AND OUTSIDE CUTOFFS ARE APPLIED HERE TOO, for the same reason and
// in the same way. They are edges as hard as the cut - softness 0 is legal -
// and drawn into the buffer they were softened across a buffer texel and
// snapped toward its grid. neon.frag now culls BLIT_CUTOFF_GUARD_PX past the end
// of each ramp and leaves the ramps themselves to this pass.

// NEON ONLY. SpotlightRenderer and LensFlareRenderer used to compile this
// shader as a plain composite of their own reduced-resolution buffers, and had
// to upload GLOW_SIDE_BOTH to switch off a cut they have no rect for. They use
// blit.frag now, so anything added here lands on the neon alone - which is
// also why the neon's tuning header is injected into this shader (see
// shaders.h.in) and not into that one.
//
// These three values are the ordinals of EdgeLighting::GlowSide, which
// neon-renderer.cpp casts straight to an int. The two numberings are one
// numbering, kept in step by hand: renumber the enum and these follow.
#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

// vPos -> uSource UV. This pass draws the EDGE RING'S COMPLEMENT (see
// NeonRenderer::setupRingGeometry) in rect-local full-res px under a full-res
// transform, so vPos is no longer NDC: the CPU uploads the reduced buffer's
// region map, 1 / region.size and -region.origin / region.size (see
// GetBufferRegion). For a buffer covering the whole viewport that is
// 1 / viewport and rectCentre / viewport, the mapping the fullscreen NDC quad
// gave. The ring reads the gather buffer through a map of the same form.
uniform vec2 uUVScale;
uniform vec2 uUVOffset;

// Geometry of the cut, all in FULL-RES px - this pass is never scaled. The
// centre is in gl_FragCoord space (y up), mirrored CPU-side out of Config's
// y-down convention, exactly as black-rect.frag takes it.
uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform vec2  uRectCenter;
uniform int   uGlowSide;
uniform float uGlowSideSoftness; // NOT pre-multiplied by the resolution scale.

// The inside / outside cutoffs, in FULL-RES px like everything above: the same
// four values neon.frag receives, without the scale. A disabled side arrives
// as the CPU's CUTOFF_DISABLED_SIZE sentinel, which no realistic geometry
// reaches - see CUTOFF_NEUTRALISED in neon-tuning.h, injected above.
uniform float uInsideCutoff;
uniform float uInsideCutoffSoftness;
uniform float uOutsideCutoff;
uniform float uOutsideCutoffSoftness;

// sdRoundBox and bandOuterDistance are neon.frag's own, from neon-sdf.glsl.

void main() {
    vec2 uv = vPos * uUVScale + uUVOffset; // rect-local full-res px -> buffer UV
    vec4 src = texture(uSource, uv); // premultiplied colour + coverage alpha

    // GlowSide::BOTH pays nothing, and the branch that arranges that is safe
    // BECAUSE uGlowSide IS A UNIFORM: every invocation in the draw takes the
    // same side, helper invocations included, which is the "uniform control
    // flow" a derivative is allowed to sit inside. The rule it must not break
    // is a derivative under control flow that varies BETWEEN fragments.
    //
    // Not the same shape as the early returns black-rect.frag's main() warns
    // about at length, and measured not to behave like them. Those sat ABOVE
    // the derivative and cost 35% on this driver whichever mode ran; this one
    // CONTAINS it, and skipping the block is a straight saving: scaled BOTH at
    // 1920x1320 runs 1.255 ms branched against 1.396 ms with the SDF hoisted
    // unconditionally, which is back to what the pass cost before it grew a cut
    // at all. One-sided is unmoved either way. Re-measure BOTH as well as a
    // one-sided scene if this block is ever restructured.
    //
    // The cutoffs join the same branch, on the same terms: whether a side is
    // live is a function of uniforms alone, so control flow stays uniform and
    // a config with neither a cut nor a cutoff - the default - still pays only
    // the texture read. A side is live when it is enabled AND glowSide does not
    // already cull it; neon.frag neutralises exactly that side (its band
    // distance block), because the cut subsumes it.
    bool cutIn  = uGlowSide != GLOW_SIDE_OUTSIDE && uInsideCutoff  < 0.5 * CUTOFF_NEUTRALISED;
    bool cutOut = uGlowSide != GLOW_SIDE_INSIDE  && uOutsideCutoff < 0.5 * CUTOFF_NEUTRALISED;
    float cut = 1.0;
    if (uGlowSide != GLOW_SIDE_BOTH || cutIn || cutOut)
    {
        vec2  p        = gl_FragCoord.xy - uRectCenter;
        vec2  halfSize = uRectSize * 0.5;
        float d        = sdRoundBox(p, halfSize, uCornerRadius);
        float aa       = max(sdRoundBoxFwidth(p, halfSize, uCornerRadius), 1e-6);
        float soft     = max(uGlowSideSoftness, aa);
        float back     = 0.5 * aa;

        // Same anchor, same floor, same curve as neon.frag's post-grade cut -
        // see the derivation there. Keep the two in step; they are one edge,
        // written twice because only one of them ever runs.
        if (uGlowSide == GLOW_SIDE_INSIDE)
        {
            cut = 1.0 - smoothstep(back - soft, back, d);
        }
        else if (uGlowSide == GLOW_SIDE_OUTSIDE)
        {
            cut = smoothstep(-back, soft - back, d);
        }

        // Same again for the cutoffs: neon.frag's masks after the grade, with
        // its direct-path floor - one destination pixel, which is what aa is
        // here. Midpoints softness/2 past each cutoff, ramps centred on them
        // and spanning the floored softness, so each starts at its cutoff and
        // ends softness past it. A dead side gets the neutral distance and its
        // smoothstep saturates to exactly 1.
        if (cutIn || cutOut)
        {
            float inHalf  = 0.5 * max(uInsideCutoffSoftness,  aa);
            float outHalf = 0.5 * max(uOutsideCutoffSoftness, aa);
            float inMid   = uInsideCutoff  + 0.5 * max(uInsideCutoffSoftness,  0.0);
            float outMid  = uOutsideCutoff + 0.5 * max(uOutsideCutoffSoftness, 0.0);
            float dIn     = cutIn  ? d + inMid : CUTOFF_NEUTRALISED;
            float dOut    = cutOut ? bandOuterDistance(p, d, halfSize, uCornerRadius, outMid)
                                   : -CUTOFF_NEUTRALISED;
            cut *= smoothstep(-inHalf, inHalf, dIn);
            cut *= 1.0 - smoothstep(-outHalf, outHalf, dOut);
        }
    }

    // Applied to the premultiplied sample, so colour and coverage scale
    // together and the layer thins out as a whole rather than dimming while it
    // keeps occluding. At BOTH with no live cutoff this is a multiply by an
    // exact 1.0, so that path stays bit-identical to the plain texture read
    // this shader used to be.
    fragColor = src * cut;

    // The output dither (neon-grade.glsl): this is the write to the caller's
    // framebuffer, so the place the reduced buffer's half-float precision
    // finally meets 8 bits.
    fragColor.rgb = neonDither(fragColor.rgb);
}

)shadersrc";

                static const char *const NEON_FIELD_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - the neon shaders (neon.frag, neon-emission.frag), where CMake
//     text-injects this file via @NEON_TUNING@ in shaders.h.in, and
//   - the C++ renderer (neon-renderer.cpp), which #includes it for the
//     glow-reach quad-sizing factors.
//
// The px constants below are written in FULL-RESOLUTION pixels. Where the
// renderer draws at a reduced NeonConfig::resolutionScale, the shader converts
// each one with uResolutionScale at the point of use; the notes on the
// individual constants say which need it and which do not.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a `const float X = 0.9f;` cannot
// be written once and stay valid in both languages. Plain #define is the only
// form that compiles identically as GLSL and as C++ from a single file.
// (For C++-only constants, prefer constexpr - macros are used here strictly
// because these values must also live inside the shaders.)
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
//
//     FILAMENT_MIN_HALF_WIDTH is a STATED-width floor: it is what makes
//     lineWidth 0 mean "no line" rather than a single bright dot, and it is
//     paired with lineGate, which fades the filament in over the same span. It
//     describes the width the caller asked for, so it is a FULL-RES px constant
//     and the shader converts it with uResolutionScale like every other one.
//
//     The SAMPLING floor is the pair below, and the distinction from the
//     stated floor is the whole point of having two of them. Below
//     resolutionScale 1.0 the gather rasterises into a reduced buffer and
//     neon-blit.frag bilinearly upsamples it, so a filament the buffer cannot
//     sample is not reconstructed - it is resampled, and what comes out
//     depends on where the rect edge happens to fall between buffer texel
//     centres. That is a property of the BUFFER, so these are in BUFFER px and
//     are NOT converted with uResolutionScale.
//
//     Getting that backwards is what made a 1 px line at scale 0.5 disagree
//     with the same line at 1.0. sigma was floored only by the converted
//     stated constant, which at scale 0.5 is 0.25 buffer px, so the line sat
//     under the buffer's Nyquist limit and its peak swung with the rect's
//     sub-pixel position - 217 on a texel centre, 180 half a texel off,
//     against a 1.0 reference that holds 230-241 at every position.
//
//     THE FLOOR IS NOT A FIXED HALF WIDTH. It was one - a flat 0.5 buffer px -
//     and a fixed half width asks the wrong question. What survives the blit
//     is not decided by the profile's width at half maximum; it is decided by
//     how much signal the NEIGHBOURING buffer texel still carries, because
//     that is what the bilinear filter rebuilds the peak from. So the floor
//     says exactly that: the profile must still be at
//     FILAMENT_NYQUIST_MIN_SHARE of its peak, FILAMENT_NYQUIST_SAMPLE_PX out.
//     Inverting the generalized Gaussian for sigma gives
//
//         sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
//
//     which neon.frag and NeonRenderer::setupGeometry both evaluate.
//
//     0.0625 and 1.0 are today's behaviour restated, not a retune: at the
//     DEFAULT falloff (N = 2) the expression is exactly 0.5 buffer px, the
//     constant it replaces, so nothing at or near the default moves.
//
//     The two questions diverge at a SOFT falloff, and that is what the change
//     is for. sigma does not only set the core - it multiplies reachSigmas, so
//     it sets the whole tail. At filamentFalloff 0.27 (N = 0.54) reachSigmas
//     clamps at FILAMENT_REACH_MAX_SIGMAS and the tail is 64 sigmas, so a flat
//     0.5 floor stretched a 31 px filament to 61 px at scale 0.5 and past
//     120 px at 0.25 - to buy 5 levels of peak accuracy (max per-phase error
//     13 -> 8) on a profile 31 px wide that the buffer was sampling perfectly
//     well. The share form asks for 0.082 buffer px there, below what the
//     caller already supplies, so it does not engage.
//
//     Where it DOES still engage it is doing the work it was added for: at the
//     default falloff, without any floor, the peak collapses to 153 against a
//     235 reference, and at filamentFalloff 1.5 to 24. The known cost of the
//     share form is the band around filamentFalloff 0.4 to 0.5, where it stops
//     engaging while the unfloored peak error is still 12 to 18 levels;
//     tightening MIN_SHARE to cover that would stop the default case being
//     bit-identical, so the crossover sits here deliberately. See
//     docs/corner-crease-and-filament-nyquist.md section 2.8.
//
//     The line does read WIDER at a reduced scale wherever the floor engages,
//     and that part is not a defect to tune away: a line the buffer cannot
//     sample cannot be reconstructed from it. The floor buys a stable,
//     correctly-bright line of the narrowest width the buffer can actually
//     carry. Deliberately NOT paired with an amplitude compensation to
//     conserve the line's integral - the grade at the end of neon.frag
//     tonemaps per-fragment INSIDE the buffer and FILAMENT_GAIN puts the core
//     deep in saturation, so scaling the linear amplitude barely moves the
//     8-bit value while it does measurably dim the peak (error 15-23/255
//     against 0-10 without it). See
//     docs/corner-crease-and-filament-nyquist.md section 2.
//
//     GATED to scales below 1.0 in both consumers. At scale 1.0 the reduced
//     buffer is full size and the blit copies it 1:1, so there is nothing to
//     survive, and the ring always shades at 1.0. The old flat constant was a
//     no-op at 1.0 by arithmetic
//     coincidence - it equalled the converted stated floor; this expression
//     would not be, above N = 2, so the gate is explicit now. ---
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_NYQUIST_SAMPLE_PX 1.0
#define FILAMENT_NYQUIST_MIN_SHARE 0.0625
#define FILAMENT_GAIN             12.0

// --- Continuous-arc filament gate feathers (neon.frag).
//     INWARD FEATHER: the smooth ramp sits INSIDE the arc's own perimeter
//     span, so nothing outside the arc gets lit -> no corner bleed regardless
//     of width, no perpendicular spike, and the profile is a plain smoothstep
//     that reads the same on straight edges and at corners. Trade-off: the
//     visible arc appears inset by these widths (arc lights up at
//     start + TAIL_FEATHER_PX and ends at start + length - HEAD_FEATHER_PX).
//     Values are pixel-space spans, divided by the current perimeter at the
//     call site, and capped per-arc by ARC_FEATHER_MAX_SHARE below. ---
#define HEAD_FEATHER_PX           14.0
#define TAIL_FEATHER_PX           14.0

// --- Cap on each feather, as a share of the arc's own length.
//
//     The feathers above are a fixed pixel span while an arc's length is a
//     FRACTION of the perimeter, so the same arc config is a different pixel
//     length on every rect. Below ~(HEAD + TAIL) px the two ramps overlapped
//     and ate into the arc's peak: an L = 0.02 arc peaked at 1.00 on an
//     800x600 rect but only 0.21 on a 200x150 one - the last place rect size
//     still reached brightness. Capping each feather at a share of the arc
//     length holds the peak at exactly 1.0 for any length at any size; a short
//     arc gets a proportionally shorter ramp instead of a truncated top.
//
//     0.4 leaves a 0.2 * length plateau at full brightness between the two
//     ramps. Up to 0.5 also holds the peak, but with no plateau the arc reads
//     as a spike rather than a flat-topped segment. Arcs longer than
//     (HEAD + TAIL) / 0.4 px never reach the cap, so the normal case is
//     bit-identical to before. ---
#define ARC_FEATHER_MAX_SHARE     0.4

// --- Halo (sharp coloured glow).
//
//     The halo and bloom are evaluated ANALYTICALLY from the rounded-box SDF
//     distance, not summed over the perimeter gather. For a locally straight
//     emitter of unit density the old sums converge exactly to:
//
//       sum g*sqrt(g) * spacing*kh^2*HALO_NORM  -> HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
//       sum 1/(dd+bw^2) * spacing*bw*BLOOM_NORM -> BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
//
//     so the NORM factors keep their meaning and their calibration: peak
//     values at ad = 0 are unchanged (0.86 and 1.005 respectively).
//
//     The closed form is what makes the effect geometry-independent. The
//     gather had to floor its kernel at a multiple of sampleSpacing to stop
//     128 discrete samples beading into dots, and sampleSpacing is
//     perimeter / NEON_MAX_LOOP_SAMPLES - so halo width, and via the gate its
//     brightness, both tracked the rect size, and glowRadius did nothing at
//     all until it exceeded the floor (~56 px on a 1920x1080 rect, i.e. most
//     of its usable range). An analytic profile cannot bead at any radius, so
//     no floor is needed and glowRadius sets the width directly.
//
//     Those two limits are of an INFINITE emitter, and evaluating one of them
//     at ad = the nearest-edge distance is what used to produce the interior
//     medial-axis creases - the dark wedges running in from each corner, plus
//     the central spine. A fragment on a corner diagonal has two edges equally
//     near and was lit by exactly one of them: measured against a fragment with
//     a single edge at the same distance, the ratio was 1.000 at every distance
//     tested, where physics says roughly 2.
//
//     neon.frag now sums the FINITE-SEGMENT form of the same two integrals over
//     the four straight edges instead (haloSegment / bloomSegment). Integrating
//     the same kernels from t1 to t2 along a segment at perpendicular distance
//     a gives elementary antiderivatives, and as t1 -> -inf, t2 -> +inf each one
//     reduces to exactly the limit above - so this is a strict generalisation
//     and the NORM factors keep the calibration they already had. The corner
//     sweep went from a V kinked at exactly 45 degrees (21 levels deep at
//     r = 160) to a smooth basin of 12, which is genuine falloff rather than a
//     crease. Cost is ~1.06-1.10x of the neon frame; the gather loop is still
//     the overwhelming majority of it.
//
//     The CREASE is what the four-segment form was adopted for. It is not the
//     largest thing it moves. The old expressions were functions of ad and of
//     nothing else, so the old profile was SYMMETRIC about the line - a
//     fragment 120 px inside the edge and one 120 px outside were lit
//     identically, by construction. Outside, the emitter recedes; inside, it
//     wraps around you. The segment sum knows the difference, so the exterior
//     now falls off slightly faster (mid-edge peak itself moves by 2/255) and
//     the INTERIOR stops falling off past roughly one rect-half and settles on
//     a floor. On the 1000x500 / glowRadius 60 probe the centre goes 162 ->
//     186 and the interior mean 174.5 -> 188.8; at glowRadius 30 on 800x400 it
//     is 81 -> 113. The glow reads as bigger, and the part that grew is the
//     interior - the exterior mean goes slightly DOWN.
//
//     Intended, and not tunable back out: a real rectangular tube does light
//     its own interior, and HALO_GAIN cannot be lowered to undo it without
//     dimming the line with it. The knobs for a perimeter-hugging glow are the
//     ones that always meant that - insideCutoff, and glowSide OUTSIDE.
//
//     THE CORNERS ARE A FIFTH THROUGH EIGHTH SEGMENT, not an extension of the
//     four. The straights are trimmed to their TANGENT POINTS, and each quarter
//     arc contributes one more segment: the arc DEVELOPED onto its own tangent
//     at whichever arc point is nearest the fragment, carrying the arc's full
//     length and split about that point, so it abuts the straights exactly in
//     arclength and the emitter is continuous. See neon.frag's
//     arcTangentSegment.
//
//     DEVELOPED AT RATE sqrt(rho * min(rho, r)), NOT AT RATE r, with the
//     measure put back by scaling the segment by r/rate - `rho` being the
//     fragment's distance from the arc centre. Laying an arc out one unit of
//     tangent per unit of arc is only right for a fragment ON it; the exact
//     distance to the point dphi away is a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,
//     whose slope at the foot is sqrt(rho*r). Rate r cost two things:
//
//       - at the CENTRE OF CURVATURE the whole arc ran off to one side of the
//         foot, and both kernels peak at t = 0, so it under-counted - to 54%
//         of the true value on a circle, where all four centres coincide at
//         the middle of the shape;
//       - and it CREASED, because the arc's endpoints slide at the tangent
//         rate on one side of the clamp and at rate 1 on the other, which
//         agree only where the rate is length(w). Every other point of the two
//         lines through an arc centre carried a C1 kink: an L-shaped seam per
//         corner, and on a circle an unmistakable dark cross. See
//         docs/review-findings.md V12.
//
//     rho == r gives rate r and weight 1, so a fragment on the tube is
//     bit-identical to the rate-r form and this calibration is untouched. The
//     whole emitter's worst error against a numerically integrated perimeter
//     falls on every geometry tested (16.5 -> 9.8% on a 600x400 r=40,
//     37.3 -> 26.3% on a circle).
//
//     The straights used to run to the SHARP corner instead, on the grounds
//     that over-extending them past the tangent point stood in for the arc that
//     has no elementary closed form. It does not stand in for it. The
//     over-extension is a phantom emitter a few px from a fragment that is tens
//     of px from the real tube: on an 800x400 rect at cornerRadius 40, a point
//     20 px outside the arc on the diagonal sits 2.4 px from EACH of the two
//     phantoms, and rendered 98 -> 146 across the commit that introduced them.
//     Against a numerically integrated rounded-rect perimeter it was 40 levels
//     too dark ON the tube and 50 too bright just outside it, which is what
//     made a rounded corner read square at a narrow glowRadius.
//
//     The developed arc costs one extra segment per corner - one atan and a
//     length on top - and takes the worst error over a quadrant from 76 to 6
//     levels at glowRadius 5, 112 to 6 at cornerRadius 120, 130 to 8 at 200.
//     A full circle (cornerRadius == halfMin) is still the residual case,
//     since its perimeter is four developed arcs and no straights at all.
//
//     Gated on uCornerRadius > 0, which is a UNIFORM - safe branching, and the
//     reason a sharp rect pays nothing and stays bit-identical (nine scenes
//     cmp-equal, not argued, and still cmp-equal after the rate fix above).
//     Rounded costs 1.19x of the neon pass against no arcs at all, and the rate
//     fix adds 1.03x on top - but the sharp path pays 1.026x of THAT for code
//     it never runs, because what decides this block is REGISTER PRESSURE, not
//     structure: with a per-arc bloom pedestal it was heavy enough to cost the
//     sharp path 1.14x. Re-time a cornerRadius 0 scene as well as a rounded one
//     after touching this. See docs/corner-crease-and-filament-nyquist.md
//     sections 1.7 to 1.9, and docs/review-findings.md V4, V10 and V12. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Width of the COLOUR gather kernel, as a FRACTION OF THE PERIMETER.
//
//     The perimeter colour blend is the one part of the shader that is still a
//     discrete sum over the loop samples, and it is the only place a length has
//     to be expressed this way rather than in pixels. The reason is that the
//     signal it filters - the gradient LUT - is itself parameterised by
//     perimeter fraction: sample i contributes LUT(i / NEON_MAX_LOOP_SAMPLES).
//     A kernel measured in pixels therefore covers a DIFFERENT span of the
//     gradient on every geometry, and the same colour stops render washed out
//     on a small rect and crisp on a large one. Measured on the stock 4-stop
//     ring, the colour sampled exactly on the red stop ran (0.95, 0.33, 0.06)
//     at 200x150 against (0.96, 0.04, 0.00) at 2800x2200 - roughly 8x the hue
//     bleed from geometry alone. Scaling the kernel with the perimeter is what
//     makes the gradient read identically at any size.
//
//     Two further properties fall out of the same choice:
//
//       - A constant anti-bead margin. The kernel has to stay >= about one
//         sample spacing, or it collapses between samples and the blend beads
//         into dots. Spacing is perimeter / NEON_MAX_LOOP_SAMPLES, so a
//         perimeter fraction pins the ratio at
//         COLOR_BLEND_PERIM_FRAC * NEON_MAX_LOOP_SAMPLES = 1.13 spacings on
//         every geometry, where the previous fixed 24 px span met the bound
//         only up to a ~3000 px perimeter and was down to 0.31 spacings by
//         9900 px. (No beading was actually measurable there - a DFT of the
//         hue around the perimeter put the 128-cycle component at the noise
//         floor - because the gradient varies slowly over one spacing on a
//         rect that large. The margin is a guarantee for dense colour stops,
//         not a fix for an observed artifact.)
//
//       - Sample-count independence. The kernel comes from the
//         NEON_MAX_LOOP_SAMPLES-based fraction rather than from the runtime
//         sample count, so the numSamples knob does not move the colour
//         blend. (An earlier sampleSpacing-derived floor divided by the live
//         count and landed 2x wider at reduced counts.)
//
//     Deliberately NOT coupled to glowRadius. The gather produces colour only -
//     the halo and bloom have been closed-form since they stopped riding on it -
//     so a wide glow has no reason to desaturate the gradient, and the old
//     max(glowRadius, floor) reintroduced exactly the pixel-space dependence
//     this constant exists to remove.
//
//     0.0088 puts the kernel at 24.0 px on the stock 800x600 / radius 40
//     geometry, matching the previous fixed span, so default-sized output is
//     unchanged.
//
//     NOTE: unit-free, unlike the px constants around it. The shader
//     multiplies it by a perimeter that is already in scaled px, so it needs
//     no uResolutionScale correction - applying one would double-apply. ---
#define COLOR_BLEND_PERIM_FRAC    0.0088

// --- Emission on/off ramp. glowRadius = 0 must read as "filament only", but
//     an analytic profile at radius 0 is a sub-pixel spike of full height
//     rather than nothing, so the halo and bloom fade in over
//     glowRadius = [0, this]. A FIXED pixel width: gating against the
//     sampleSpacing-derived floor instead would re-couple brightness to the
//     rect size, which is the whole thing this design removes.
//
//     NOTE: full-res pixel span, compared against a uGlowRadius that arrives
//     already scaled, so the shader multiplies this by uResolutionScale at
//     the point of use. Identity at scale 1.0. ---
#define GLOW_GATE_FADE_PX         2.0

// --- Lower bound on the analytic emission widths. Guards the divides only;
//     anything this small is already multiplied out by GLOW_GATE_FADE_PX. ---
#define EMISSION_MIN_WIDTH        1e-3

// --- Degenerate guard for the corner arc's tangent frame (neon.frag,
//     arcTangentSegment). The frame is built by normalising the fragment's
//     offset from the arc centre, clamped into the quarter the arc occupies;
//     that offset is the zero vector only for a fragment sitting exactly on an
//     arc centre, where the direction is undefined and atan(0, 0) is undefined
//     with it. Below this the frame falls back to the nearer of the two tangent
//     points, which is the correct clamp for the whole region the guard covers.
//
//     It floors `rho` in the same function, for the same fragment: the
//     development rate divides into the segment's weight, so an exactly-zero
//     rho would be a division by zero. The floored value is nowhere near a
//     pixel and the limit it lands on is the exact answer anyway, so this only
//     keeps a NaN out.
//
//     Unit-free: compared against a length in the shader's own px space, and
//     small enough that nothing but the exact degeneracy reaches it. ---
#define ARC_FRAME_EPSILON         1e-6

// --- Bloom (wide background spill). See the halo note above for the closed
//     form; BLOOM_SPACING_FLOOR is gone with the gather. ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_NORM_FACTOR         0.32

// --- Travelling-segment array size (shared by C++ vector cap + GLSL uniform array) ---
#define MAX_SEGMENT_BOOSTS        8

// --- Arc range array size (shared by C++ vector cap + GLSL uniform array).
//     Each arc gates a slice of the perimeter with its own colour + intensity.
//     Overlap resolves winner-take-all (max mask*intensity wins). ---
#define MAX_ARCS                  8

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO and
//     the shader's array. The gather iterates only uNumSamples of them
//     (NeonConfig::numSamples, which defaults to this), so it is a ceiling,
//     not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Glow coverage table (neon-glow-cover.frag, V20 and V21): for each piece
//     of the emitter and each fragment position round it, the arcs' and the
//     segments' coverage of that piece as the halo and the bloom see it. Laid
//     out in neon-pieces.glsl: four bands of ROWS rows, each holding one
//     straight at its left and one corner at its right. A band's columns are
//     shared between the two in proportion to their lengths (uGlowCoverSplit,
//     NeonRenderer::GetGlowCoverSplit), so a circle's corners get the columns
//     its straights do not need and a long rect's straights get its corners':
//     the straight has OVERHANG columns past each end, the corner
//     CORNER_OVERHANG on each side of its arc and a guard texel at each end of
//     its block, and the columns left over (the layout's `shared`) are split
//     between the two interiors, each keeping at least MIN_INTERIOR. ROWS 32 is half of them
//     within one halo width of a straight, and 16 inside and 16 outside a
//     corner's arc. 1024 x 128 RGBA16F is 1.0 MB - half V20's single table per
//     perimeter. Sizing it, against an exact per-piece reference (V21 in
//     docs/review-findings.md): 48 rows (1.5 MB) read at most 2 levels off where
//     32 reads 3 on a few pixels of a segment scene, for 1.3x the bake's cost
//     under animation; 768 columns read 6 levels off a 1 px halo on a 4K-sized
//     rect.
//
//     The WIDTH is not a constant: the table's layout is a parameter
//     (NeonRenderer's GlowCoverLayout, the shaders' uGlowCoverLayout), between
//     MIN_WIDTH and MAX_WIDTH in steps of WIDTH_STEP. ROWS and HEIGHT are. ---
#define GLOW_COVER_MAX_WIDTH      1024
#define GLOW_COVER_MIN_WIDTH      256
#define GLOW_COVER_WIDTH_STEP     64
#define GLOW_COVER_ROWS           32
#define GLOW_COVER_OVERHANG       64
#define GLOW_COVER_CORNER_OVERHANG 32
#define GLOW_COVER_MIN_INTERIOR   16
#define GLOW_COVER_HEIGHT         (4 * GLOW_COVER_ROWS)

// --- The glow coverage table's width (NeonRenderer's GetGlowCoverWidth):
//     enough interior columns that the longest band - its straight plus its
//     quarter arc, L full-res px - gets one every S px, S growing with the
//     halo width kh (max(glowRadius, EMISSION_MIN_WIDTH), px):
//
//         S    = clamp(PX_PER_COLUMN_PER_KH * kh, MIN_PX_PER_COLUMN, MAX_PX_PER_COLUMN)
//         need = 2 + 2 OVERHANG + 2 CORNER_OVERHANG + ceil(L / S)
//         W    = clamp(need rounded up to WIDTH_STEP, MIN_WIDTH, MAX_WIDTH)
//
//     In full-res px because the edge ring reads the table at full
//     resolution; lengths enter the layout only as ratios, so every scale
//     shares one table.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the 1024-wide
//     table (docs/neon-glow-cover-resolution-plan.md step 2): 250 configs -
//     rects 200 x 120 to 3600 x 2000, glowRadius 1 / 2 / 5 / 10 / 20,
//     cornerRadius 0 / 40 / a circle, both windings, three partial arcs with
//     and without colour stops and with two segments, at scales 1.0 / 0.5 /
//     0.25 - each at every width from 256 to 960, the frame after a light
//     change, against the same frame at 1024. The widest spacing that still
//     met the plan's criterion (max 2 levels, 99.9% of lit pixels within 1)
//     on every config was ~2.2 px per column at kh 1, 2.5 at 2, 4.6 at 5, 6.8
//     at 10 and 10 at 20. These constants sit under that envelope - 12% at
//     kh 5, 27% or more elsewhere - and at the widths they give, every config
//     read at most 2 levels with at least 99.988% of lit pixels within 1.
//     Spacing 20% wider failed at kh 5 (4 levels on a 200 px circle). A
//     narrow glow on a large rect still gets MAX_WIDTH: at kh 1-2 a
//     3600 x 2000 rect needed all 1024 columns. ---
#define GLOW_COVER_PX_PER_COLUMN_PER_KH 0.8
#define GLOW_COVER_MIN_PX_PER_COLUMN    1.6
#define GLOW_COVER_MAX_PX_PER_COLUMN    5.0

// --- The segment table (NeonRenderer's GetGlowCoverSegmentWidth,
//     neon-glow-cover-fill.frag): the segments' two channels of the glow
//     coverage table, baked at a width of their own and copied in. A bell's
//     standard deviation is 0.3536 x its length x the perimeter - ~100 px for
//     a length-0.05 segment on a 1840 x 1000 rect, against an arc's 14 px
//     feather - so it needs far fewer columns than the arcs:
//
//         sigma = 0.3536 x the shortest lit segment's length x perimeter   (full-res px)
//         need  = 2 + 2 SEG_OVERHANG + 2 SEG_CORNER_OVERHANG + ceil(L / (sigma / SEG_COLUMNS_PER_SIGMA))
//         W     = clamp(need rounded up to WIDTH_STEP, SEG_MIN_WIDTH, the main table's width)
//
//     with L the longest band, as for the main table. Where W would be at
//     least SEG_DIRECT_SHARE of the main table's width (very short segments)
//     the separate table saves nothing, and the segments are baked into the
//     main table directly, as before it existed.
//
//     Calibrated 2026-10-09 on an AMD Radeon Pro 5300M against the segments
//     baked into the main table directly (plan step 3): 135 configs - rects
//     200 x 120 to 3600 x 2000, glowRadius 2 / 5 / 10, cornerRadius 0 / 40 /
//     a circle, scales 1.0 / 0.5 / 0.25, segments of length 0.01-0.2 with
//     boosts 0.5-2, overlapping, abutting, over the corners, travelling, and
//     under a partial arc - the frame after a light change, the segment table
//     forced on. With overhangs 16 / 8, every COLUMNS_PER_SIGMA from 3 to 8
//     read at most 2 levels with 99.9992% of lit pixels within 1; 2 read 3
//     levels on a thin glow at 3840 x 2160. 4 is one step of margin. Overhangs
//     8 / 4 held at every value tried (99.988%), 32 / 16 read at most 1 level
//     from 4 up but do not fit SEG_MIN_WIDTH. ---
#define GLOW_COVER_SEG_COLUMNS_PER_SIGMA 4.0

// --- How far the glow coverage bake integrates a segment's bell either side
//     of its centre, in units of 1 / invSigma (the bell is exp(-(d invSigma)^2),
//     so 3 is 4.24 standard deviations, where it has fallen to 1.2e-4 of its
//     peak). The bake (neon-glow-cover.frag, segmentsOnPiece) cuts the bell
//     here and NeonRenderer's GetGlowCoverDirtySegmentPieces mirrors it to
//     find the pieces a moved segment reaches: change one and the other
//     follows, or a piece the bell reaches is not re-baked. It was 5 / sqrt(2)
//     (5 sigma); at 3 a moved segment dirties fewer pieces, which took the
//     segment table's bake on a band of 8 segments from 0.75 to 0.58 (timer
//     units, AMD Radeon Pro 5300M), at most 1 level, on up to 1.6% of a
//     frame's lit pixels (0.15% on average over the cover set; I61 in
//     docs/review-findings.md). An 8-node rule on top halved it again but
//     read 3 levels off a long, bright segment, and was not taken. ---
#define GLOW_COVER_BELL_REACH            3.0
#define GLOW_COVER_SEG_OVERHANG          16
#define GLOW_COVER_SEG_CORNER_OVERHANG   8
#define GLOW_COVER_SEG_MIN_WIDTH         128
#define GLOW_COVER_SEG_DIRECT_SHARE      0.75

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- The output dither (R7), peak to peak in 8-bit levels: neonDither
//     (neon-grade.glsl) adds noise in +/- half this to every colour the neon
//     writes to the caller's framebuffer - the blit, the edge ring and the
//     ring's field composite, never an offscreen buffer. The halo and bloom
//     are slow gradients that cross a level only every few px, up to tens of
//     px in the dark tail, so an undithered write draws concentric contour
//     rings around the rect. 1.0 is the least that removes them; 0 turns the
//     dither off. It assumes an 8-bit target. ---
#define OUTPUT_DITHER_LSB         1.0

// --- Epsilons ---
#define WSUM_EPSILON              1e-6

// --- One-sided cut guard band, in BUFFER pixels. Scaled path only.
//
//     One of the two constants in this file stated in buffer px rather than
//     full-res px (BLIT_CUTOFF_GUARD_PX below is the other): it describes the
//     BUFFER's own sampling, so it must NOT be converted with uResolutionScale.
//
//     For pass 1b the one-sided cut is not applied by neon.frag at all -
//     neon-blit.frag applies it at DESTINATION resolution, where a pixel is a
//     pixel and the edge can land exactly where the full-resolution shading
//     puts it. What neon.frag still does is cull the dark side, and the bound
//     it culls at cannot be the cut itself: the blit reconstructs each
//     destination pixel from the 2x2 buffer texels around it, so a texel
//     killed at the cut leaves the first LIT destination pixel rebuilt partly
//     from black. Measured at scale 0.5, glowSide OUTSIDE: the first lit pixel
//     came back at 178 against the 239 full resolution puts there, a dark seam
//     hugging the inside of the glow's own edge.
//
//     So the cull runs this far PAST the cut, and the guard band's emission is
//     what the blit's filter reconstructs the boundary from. One buffer texel
//     is the filter's reach; 2.0 covers that with room for the diagonal, where
//     fwidth of an SDF is sqrt(2) rather than 1, and for the half-texel of
//     phase between the cut and the nearest texel centre.
//
//     The band costs a two-texel strip along the perimeter - nothing beside
//     the half-plane the cull still removes - and none of it is visible: the
//     blit's mask is zero everywhere past the cut, including at the hard step
//     where this bound finally discards.
//
//     NOT used by the edge ring, which owns its own cut (uBlitOwnsCut 0). See
//     neon.frag's sideCull and the post-grade cut block. ---
#define BLIT_SIDE_GUARD_PX        2.0

// --- Cutoff guard band, in BUFFER pixels. Pass 1b only.
//
//     BLIT_SIDE_GUARD_PX's argument, applied to the inside/outside cutoffs.
//     For pass 1b neon-blit.frag applies the cutoff masks at
//     DESTINATION resolution, and neon.frag only culls - this far past the end
//     of each ramp, so the blit's bilinear filter rebuilds every boundary from
//     lit texels rather than from black. Same value as the side guard, for the
//     same reasons: one buffer texel of filter reach, plus room for the
//     diagonal and for the phase between the boundary and the nearest texel
//     centre.
//
//     It replaces CUTOFF_SOFT_FLOOR_PX, a feather in buffer px that let a mask
//     drawn INTO the buffer place its boundary sub-texel. That was the best the
//     buffer could do, and it still softened the boundary across a buffer
//     texel and snapped it toward the buffer grid: on the bounded_band scene of
//     docs/neon-resolution-scale-comparison.html it read p99 41/255 against the
//     1.0 render at scale 0.5, and 69 at 0.25, and the band spread past its
//     own cutoffs as the scale fell (50,703 lit px at 0.5 and 53,568 at 0.25,
//     against 47,012 at 1.0). Drawn by the blit instead it reads p99 4 at 0.5
//     and 12 at 0.25, with 47,012 lit px at every scale; what is left is the
//     reduced-resolution glow inside the band, not its edges. See
//     docs/neon-resolution-scale-plan.md, step 2.
//
//     NOT used by the edge ring, whose masks and culls are its own. See
//     neon.frag's cutGuard and NeonRenderer::setupGeometry's cutGuardPx. ---
#define BLIT_CUTOFF_GUARD_PX      2.0

// --- Edge ring width. CPU only.
//
//     At every scale, NeonRenderer redraws a ring around the rect
//     edge at FULL resolution (neon.frag's ring program),
//     reading only the gather's result from the gather buffer. The ring reaches R
//     full-res px either side of the edge (GetRingWidth in neon-renderer.cpp):
//
//         R = max(reach(1.0), reach(scale)) + RING_GUARD_TEXELS / scale
//
//     where reach(s) is the filament's reach as neon.frag computes it at scale
//     s, in full-res px. BOTH, because inside R the reduced pass's filament -
//     floored to what its buffer can sample, so wider than the real one
//     whenever the line is thin - is replaced by the real one, and R has to
//     clear whichever reaches further. Uncapped: the reach is where a filament
//     has fallen to FILAMENT_CUTOFF of its gain, about 2/255 after the grade,
//     which is exactly how far its error can show. RING_GUARD_TEXELS adds the
//     reduced buffer's bilinear footprint past that.
//
//     Calibrated over lineWidth {1, 2, 4, 8, 16} x filamentFalloff
//     {0.5, 1, 2, 4} x glowRadius {0, 2, 5, 20}, each config rendered at
//     scales 0.75, 0.5 and 0.25 and compared against its own 1.0 render
//     (docs/neon-resolution-scale-plan.md, step 5). Worst error over all 80:
//
//       RING_GUARD_TEXELS    scale 0.75    scale 0.5    scale 0.25
//              0.5               3            12            3
//              1.0               4             2            2
//              2.0               2             2            2
//
//     1.0 is the smallest that holds 2/255 at 0.5 and 0.25, the targets. 2.0
//     also brings 0.75 within 2 (its worst at 1.0 is a 1 px flat-top line
//     with no glow) for a ring one buffer texel wider each side.
//
//     The two things that made the first version of this ring fail that sweep,
//     both now handled: R used to cover only the full-res filament (fixed by
//     taking the reduced pass's too, above), and at a small glowRadius pass 1's
//     quad stopped short of R, so the ring read gather texels nothing wrote
//     (fixed by covering the ring with the quad; since the gather was split
//     out, the gather pass's own quad, built in setupRingGeometry, covers
//     it). ---
#define RING_GUARD_TEXELS         1.0

// --- Gather resolution. CPU only.
//
//     At every resolutionScale the gather - ~95% of neon.frag's cost when it
//     ran inline - runs in a pass of its own (neon-gather.frag) at its own
//     scale, and pass 1 and the
//     edge ring shade from its result. Its scale is (GetGatherScale in
//     neon-renderer.cpp)
//
//         clamp(GATHER_TEXELS_PER_KERNEL / kc, GATHER_MIN_SCALE,
//               min(resolutionScale, GATHER_MAX_SCALE))
//
//     with kc the colour kernel's width in full-res px, perimeter *
//     COLOR_BLEND_PERIM_FRAC: the gather's outputs are Lorentzian-weighted
//     means whose kernel is never narrower than kc, so this many texels per kc
//     carries them through a bilinear read.
//
//     Calibrated on Mesa llvmpipe with an RGBA16F gather buffer (worst error
//     against 1.0 over `probe scenes`, and the default scene's frame time):
//
//       GATHER_TEXELS_PER_KERNEL   worst error   default 0.5   default 0.25
//                1.0                   4              -              -
//                1.5                   2           10.5 ms        8.0 ms
//                2.0                  1-2          10.8 ms        7.3 ms
//                4.0                   -           14-17 ms      11-12 ms
//         (gather at resolutionScale)  1-2          37.5 ms       12.3 ms
//
//     2.0 is the knee: 1.5 saves nothing measurable (the gather is no longer
//     the pass that costs) and 4.0 costs half as much again for no visible
//     gain. With an RGBA8 gather buffer - the fallback - 2.0 reads 3/255.
//     GATHER_MIN_SCALE barely matters - full-viewport rects
//     at 720p and 1080p read 1-2 at a floor of 0 and 0.0625 alike - and is
//     kept as a cheap guard: the gather buffer is clipped to the viewport, so
//     at the floor it is at most 128 x 80 texels at 1080p. See
//     docs/neon-resolution-scale-plan.md section 13. ---
#define GATHER_TEXELS_PER_KERNEL  2.0
#define GATHER_MIN_SCALE          0.0625

// --- The gather's ceiling. CPU only.
//
//     GetGatherScale never gathers finer than this, at any resolutionScale.
//     It binds only on a rect whose perimeter is under ~450 px (kc under 4
//     px), which would otherwise gather near full resolution: its gather
//     buffer - RGBA16F over the glow quad's box, two attachments with
//     segments - ran to 16 / 33 MB at 1080p for a 40 x 24 rect whose glow
//     fills the frame. At 0.5 that is a quarter, the same as the scaled path
//     at 0.5 always gathered at, and the check suite's smallest rect (perimeter
//     478 px) is not touched.
//
//     It replaces the split gate at 1.0 (FULL_RES_SPLIT_GATHER_MAX_SCALE /
//     FULL_RES_SPLIT_MIN_AREA_PX, removed - docs/neon-shader-cleanup-plan.md
//     step 3), which ran the gather INLINE in neon.frag where splitting it
//     out did not pay: on an AMD Radeon Pro 5300M, where the first offscreen
//     pass of a frame costs ~0.15 ms, for a still cutoff band under ~140k px
//     of quad and for a gather near full resolution. Since unchanged frames
//     stopped re-running the gather (I40) the still case no longer leaves
//     the caller's framebuffer at all; an Apple M2 Pro measured the split
//     1.1-4.75x faster at every size and mode, and the AMD, re-measured with
//     every rect split against its own gate, 0.98-1.02x in every animated
//     mode with one loss - a cutoff band under an intensity pulse, 0.84x.
//     So every scale splits, and the inline loop went with the gate. ---
#define GATHER_MAX_SCALE          0.5

// --- Stand-in distance for a cutoff that glowSide has already subsumed.
//
//     neon.frag hands this to the band distance on the side the one-sided cut
//     culls, so the discard and the mask there no-op through the arithmetic
//     exactly as a DISABLED cutoff does - same mechanism, same sentinel shape,
//     no extra branch downstream.
//
//     Matches CUTOFF_DISABLED_SIZE in neon-renderer.cpp, which is what a
//     disabled cutoff actually arrives as. It is a distance in the shader's own
//     px space and only has to be far enough that no realistic geometry reaches
//     it; see the note on that constant for the same reasoning. ---
#define CUTOFF_NEUTRALISED        1.0e6

// --- Glow reach (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity).
//
//     Named for the reach, not for an early-out: the per-fragment
//     `ad > earlyOut -> discard` this constant was originally calibrated for
//     no longer exists. Geometry culls the far region instead, which is
//     tiler-friendly, so what the factor sets is how far the glow is allowed to
//     reach before the quad stops covering it. The locals it feeds already say
//     so - `glowReach` in both setupGeometry implementations, `reach` in both
//     shaders.
//
//     glowRadius only: the companion sampleSpacing * SPACING_FACTOR term is
//     gone. sampleSpacing is perimeter / NEON_MAX_LOOP_SAMPLES, so it won on
//     any reasonably large rect at default glowRadius and made both the
//     distance the bloom got truncated at and the brightness it still had
//     there track the rect size. The bloom's reach is a function of glowRadius
//     alone, so that is all that sizes the quad.
//
//     Used by the renderers' setupGeometry AND by the shaders, which recompute
//     the same expression to place the bloom pedestal that lets this margin
//     stay tight without the truncation showing. Keep the two in step. ---
#define GLOW_REACH_RADIUS_FACTOR  48.0

// --- Where the shaders' quad-edge fade begins, as a FRACTION of the quad
//     margin. The emission ramps to zero over [FRAC * margin, margin], so the
//     bloom's 1/D tail - still ~10% of peak out at the quad edge - never clips
//     as a hard rectangle.
//
//     A fraction, not a pixel span, because the margin is proportional to
//     glowRadius (see GLOW_REACH_RADIUS_FACTOR) and so is the bloom profile it
//     hides. A fraction keeps the ramp at a constant proportion of the bloom's
//     reach, so the fade reads the same at every glow radius; a fixed px ramp
//     would vanish on a wide glow and dominate a narrow one. At the stock
//     glowRadius 5 / bloom 0.3 / intensity 1 the margin is 312 px, giving a
//     62 px ramp.
//
//     Nothing derives 0.8 - anything leaving a ramp wide enough to hide the
//     clip behaves the same. What it DOES assume is that the margin was set by
//     the glow, where 20% of it is a long distance. When outsideCutoff clamps
//     the margin instead (to where its fade ends, size + softness, plus 1),
//     20% of ~13 px is 2.6 px and
//     the ramp lands INSIDE the cutoff band, dimming the band's outer edge
//     ahead of the cutoff mask - and only on the exterior, since the fade keys
//     on positive d. Both shaders therefore floor the ramp's start at the
//     cutoff boundary whenever that boundary falls inside the quad; see the
//     fadeStart block in neon.frag.
//
//     Since V23 the bloom no longer relies on this fade to end smoothly - each
//     piece fades by its own distance from BLOOM_FADE_START_FRAC, below, which
//     has it under a level well before this ramp starts. ---
#define QUAD_FADE_START_FRAC      0.8

// --- Where each piece's bloom starts fading out, as a FRACTION of `reach`
//     (neon.frag's bloom end fade, V23).
//
//     The pedestal takes the bloom's 1/a tail to exactly 0 at `reach`, but
//     with its slope still on, so the glow stopped on a crease: inside a large
//     rect, a hard-edged dark rectangle `reach` in. The quad-edge fade above
//     hid it outside, but over [0.8, 1.0] of the margin, where the tail is
//     still ~5 levels up, and a fade that late STEEPENS the tail before it
//     flattens it - a shoulder, then flat, which reads as an edge too. So
//     each piece's bloom is multiplied by 1 - smoothstep(FRAC * reach, reach,
//     a) of its own distance, on both sides.
//
//     0.5 is about the latest start that does not steepen it: the steepest
//     slope inside the fade over the slope where it starts, on the
//     infinite-line bloom at glowRadius 5, bloomStrength 0.52, intensity 0.8,
//     is 1.40 at 0.8, 1.14 at 0.6, 1.035 at 0.5 and 1.00 from 0.4 down. (It
//     is also where c * (reach - a)^2 meets the pedestalled 1/a tail in value
//     and slope.) An earlier start costs more of the tail; at 0.5 the glow
//     reads up to 6 levels dimmer in the outer half of its reach. ---
#define BLOOM_FADE_START_FRAC     0.5

// --- Filament reach for the same quad sizing, expressed in sigmas.
//
//     glowRadius = 0 means "filament only" - the halo and bloom are gated off
//     by GLOW_GATE_FADE_PX - but the filament itself is still there, sized by
//     lineWidth, so the quad has to cover it or the exterior gets clipped.
//
//     The reach is NOT a constant, because the profile is
//     exp2(-(ad/sigma)^N) with N = 2 * filamentFalloff and the slider runs
//     down to falloff 0. Solving core * FILAMENT_GAIN < FILAMENT_CUTOFF gives
//
//         reach = sigma * log2(FILAMENT_GAIN / FILAMENT_CUTOFF) ^ (1/N)
//
//     which is 1.9 sigmas at falloff 2.0, 3.5 at 1.0, 12.6 at 0.5, 67.8 at 0.3
//     and 558 at 0.2 - it diverges as falloff -> 0. A fixed 12 (calibrated for
//     falloff 0.5) left the low end badly under-sized: the interior is
//     unbounded because the quad always covers it, so a soft filament flooded
//     inward while the exterior was chopped at 12 sigmas. That asymmetry is
//     the "no outer glow at glowRadius 0" report.
//
//     MAX_SIGMAS bounds the fill cost where the formula diverges; the shaders
//     pedestal-subtract the core at exactly this reach, so hitting the cap
//     shortens the glow symmetrically on BOTH sides instead of showing a cliff
//     on the outside. MIN_SIGMAS keeps a little room at very high falloff.
//     Keep the expression in step across neon-tuning.h, both setupGeometry
//     implementations, and both fragment shaders. ---
#define FILAMENT_CUTOFF           0.002
#define FILAMENT_REACH_MIN_SIGMAS 2.0
#define FILAMENT_REACH_MAX_SIGMAS 64.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// The neon's grade: the tone map from linear light to the output, shared by
// neon.frag and neon-field.frag, and the output dither, shared by those two and
// neon-blit.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it, after the tuning
// header (TONE_MAP_SHOULDER, GAMMA_EXPONENT, OUTPUT_DITHER_LSB), into the glow,
// the hue-invariant field's composite, which tone-maps col * Fa where neon.frag
// tone-maps its own result - the two have to agree to the bit for the field
// to stand in for pass 1 (I46) - and the blit, which only dithers.
// ---------------------------------------------------------------------------

precision highp float;

// Hue-preserving Reinhard: tonemap the peak channel and scale the others by
// the same ratio. Per-channel tonemap desaturates warm mixes (orange -> peach)
// because R saturates while G/B are still linear; scaling by the peak's
// compression preserves the original R:G:B ratio.
vec3 neonToneMap(vec3 result) {
    float peak = max(max(result.r, result.g), result.b);
    float mapped = peak / (peak + TONE_MAP_SHOULDER);
    result = result * (mapped / max(peak, 1e-6));
    return pow(result, vec3(GAMMA_EXPONENT));
}

// The output dither (R7): interleaved gradient noise in +/- OUTPUT_DITHER_LSB / 2
// levels added to a colour about to be written to the caller's 8-bit target,
// so the rounding there breaks the glow's slow gradients into noise instead of
// contour rings. A pure function of the pixel - no time term - so a frame is
// reproducible, and the blit and the edge ring, which tile one frame, draw one
// noise field across their seam. Colour only: the blend is premultiplied, and
// the same noise on alpha would cancel it over a bright background. Only for
// a write to the caller's framebuffer; an offscreen buffer is read back
// filtered, which averages the noise into blotches.
vec3 neonDither(vec3 c) {
    // A constant test, so OUTPUT_DITHER_LSB 0 compiles the noise out rather
    // than computing it and multiplying it by 0.
    if (OUTPUT_DITHER_LSB <= 0.0) {
        return c;
    }
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    return c + (n - 0.5) * (OUTPUT_DITHER_LSB / 255.0);
}

// ---------------------------------------------------------------------------
// The rounded box's signed distance, its antialiasing width and the cutoff
// band's two boundaries, shared by neon.frag, neon-blit.frag, neon-field.frag
// and black-rect.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it into the four
// programs that have to agree on the shape to the pixel - the glow, the
// scaled path's blit (which re-applies the glow's cut and cutoffs at full
// resolution), the ring field's composite (which re-applies the ring's) and
// the opaque fill (which has to register with the glow's edge) - so the four
// stay one copy. It reads no uniform and no constant.
// ---------------------------------------------------------------------------

precision highp float;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// One pixel's width in sdRoundBox's units, for the box filter across the
// outline: what fwidth(d) returns along a straight stretch of it, taken from
// the distance's analytic gradient instead of from d's own differences.
//
// fwidth(d) differences d across a 2x2 quad, and at a sharp corner that is not
// a derivative. On the vertex pixel the quad can hold BOTH outside neighbours:
// d then steps a whole pixel along each axis, fwidth reads 2, and every ramp
// sized from it doubles there - the corner pixel of a one-sided cut came out
// at 0.84 of the other three corners', the fill's at 0.75. The quad's parity
// decides which corner, so one rect showed it at its top-left and the next at
// no corner at all (V26). Along the inside diagonal it read 0 or 2 the same
// way. The gradient is one-sided by construction - a pixel takes the edge it
// is nearest - so it is 1 on every corner pixel, as along the edges, and
// fwidth's value wherever d is smooth.
//
// |gradient| per axis times that axis's pixel, fwidth(p): exact because every
// caller's p is the rect's own frame through an unrotated orthographic
// transform - vPos, or gl_FragCoord less the centre - so each screen axis
// moves one axis of p. q and length(max(q, 0)) are sdRoundBox's own, so a
// caller taking both pays for the length once. A derivative: take it in
// uniform control flow.
float sdRoundBoxFwidth(vec2 p, vec2 b, float r) {
    vec2  q  = abs(p) - b + r;
    vec2  qp = max(q, 0.0);
    float l  = length(qp);
    vec2  g  = (q.x > 0.0 && q.y > 0.0) ? qp / l
             : ((q.x > q.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    return dot(g, fwidth(p));
}

// --- Band boundary distances -------------------------------------------
// The band's two boundaries, expressed as signed distances: dIn >= 0 means
// "past the inside cutoff", dOut <= 0 means "within the outside cutoff".
//
// INNER: plain Euclidean (d + cut). Inside the shape the rounded-box SDF is
// already per-axis, so the inner boundary is square at cornerRadius 0 and a
// correct parallel curve (radius r - cut) above it. Nothing to fix.
//
// OUTER: Euclidean too whenever cornerRadius > 0, where it is exactly d - cut.
// That is the parallel curve, so the band keeps a uniform width and the opaque
// fill covers precisely as far as the light reaches - no black bulging past
// the glow at the corners.
//
// The one exception is cornerRadius == 0: the parallel curve of a SHARP corner
// is an arc of radius `cut`, so a rect the designer asked to be square comes
// out with rounded outer corners. There, and only there, offset the box
// per-axis instead to keep the corner square. The band is then ~1.41x wider
// measured diagonally across that corner, which is unavoidable - a uniform
// width and a square outer corner cannot both hold at a sharp corner.
//
// Disabled cutoffs arrive as a huge sentinel and still no-op: dIn goes hugely
// positive, dOut hugely negative, so both masks evaluate to 1.
float bandOuterDistance(vec2 p, float d, vec2 halfSize, float r, float cut) {
    if (r > 1e-4) { return d - cut; }
    vec2 b = halfSize + vec2(cut);
    return sdRoundBox(p, b, 0.0);
}
float bandInnerDistance(float d, float cut) {
    return d + cut;
}

precision highp float;

// ---------------------------------------------------------------------------
// The hue-invariant field's composite (pass 1c).
//
// Under a rotating hue - the library's default - only two things in neon.frag
// move from frame to frame: the gathered hue, and the colour-stop alpha read at
// the fragment's perimeter position. Everything else - the filament, the halo,
// the bloom, each piece's coverage, the fade - is a function of the config and
// the geometry, and enters the colour linearly:
//
//     neon.frag   = tonemap( col * Fa + segColHue * Fs )   (nothing after the grade)
//
// where col and segColHue are the gathered arc and segment hues and Fa and Fs
// are time-invariant. So NeonRenderer bakes both once - neon.frag compiled
// with NEON_FIELD_BAKE, which puts the arc hue on red and the segment hue on
// green and writes the pre-tone-map result - and a frame becomes the gather
// plus this: read the field, read the gathered hues, tone-map. The tone map
// is neon.frag's own function, neonToneMap from neon-grade.glsl. Without
// segments Fs is 0 and the field is one channel (R16F); with them it is two
// (RG16F), and uFieldSegments says which (D3).
//
// "Nothing after the grade": pass 1b leaves the one-sided cut and the cutoffs
// to the blit (uBlitOwnsCut), so the shading never applies them after its tone
// map and the field is one channel.
//
// This draws pass 1b's glow quad into the reduced buffer - full size at 1.0 -
// unblended, in pass 1b's place, under a rotating hue or a moving intensity
// (IsFieldEligible): the only frames that redraw pass 1b with nothing but the
// time, or the gain, moved.
//
// The texel a fragment wants is floor((vPos - uFieldOrigin) * uFieldTexelScale),
// rect-local, and so independent of where the host's viewport starts. The
// field IS the reduced buffer's region and grid, vPos is in scaled px, and the
// scale is that buffer's texels per scaled px - 1 but where the region is the
// whole viewport, whose texel count is truncated. Not gl_FragCoord: a select
// between the two cost the old full-resolution composite ~30% on an AMD Radeon
// Pro 5300M. A texel neon.frag discarded holds 0, which composites to nothing,
// as the discard did.
//
// NEON_FIELD_RING builds the edge ring's composite instead (at every scale, onto
// the caller's framebuffer, in pass 2c's place). Its field is full resolution
// but holds only the ring, PACKED: the ring is an axis-aligned annulus - a box
// minus a hole (setupRingGeometry) - so its pixels are four strips, stored
// one above the other on the viewport's pixel grid: the bottom and the top as
// they are, the left and the right transposed, so a side strip lies along the
// atlas's width rather than wasting a column of it. A fragment finds its box
// pixel q from vPos, its strip from q against the hole, and its texel from
// the strip's row. The hole is CONSERVATIVE - only box pixels the ring can
// never draw - so every ring fragment lands in a strip. The ring shades with
// scale-1.0 uniforms, so unlike pass 1b it multiplies the one-sided cut and the
// cutoffs in after its tone map; this composite does the same, from the
// pixel's own distance to the edge, so a ring with either still takes its
// field.
// ---------------------------------------------------------------------------

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uField;       ///< Fa (.r) and, with segments, Fs (.g): R16F or RG16F; NEAREST.
#ifdef NEON_FIELD_RING
uniform vec2      uRingLo;      ///< vPos (full-res px) of the ring box's lower-left pixel corner.
uniform vec4      uRingHole;    ///< Box pixels the ring never draws: [x0, x1) x [y0, y1), as x0, y0, x1, y1.
uniform vec2      uRingRows;    ///< First atlas row of the left strip and of the right strip.

// The one-sided cut and the cutoffs, as the ring's own shading takes them
// (uploadNeonUniforms at scale 1.0): full-res px, a disabled cutoff at
// CUTOFF_NEUTRALISED.
#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2
uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;
uniform float uInsideCutoff;
uniform float uInsideCutoffSoftness;
uniform float uOutsideCutoff;
uniform float uOutsideCutoffSoftness;
#else
uniform vec2      uFieldOrigin; ///< vPos of the field's texel (0, 0) corner.
uniform vec2      uFieldTexelScale; ///< Field texels per vPos unit: the reduced buffer's texels per scaled px.
#endif
uniform sampler2D uGather;      ///< The gather buffer: the hue in .rgb.
uniform sampler2D uGatherSeg;   ///< Its second attachment: the segment hue in .rgb. Read only when uFieldSegments is set.
uniform int       uFieldSegments; ///< 1 when the field carries Fs in .g (the config has segments), else 0.
uniform float     uFieldGain;   ///< I / I0: the neon's intensity over the one the field was baked at; exactly 1 but while it moves (I56). Scales Fa only - the segments do not take the intensity.
uniform vec2      uGatherUVScale;
uniform vec2      uGatherUVOffset;

void main() {
#ifdef NEON_FIELD_RING
    // Bottom strip, then the top stacked on it, then the left and the right
    // transposed - the layout computeRingFieldLayout builds and
    // renderRingFieldPass bakes. Keep the three together.
    ivec2 q    = ivec2(floor(vPos - uRingLo));
    ivec4 hole = ivec4(uRingHole);
    ivec2 texel;
    if (q.y < hole.y)
    {
        texel = q;
    }
    else if (q.y >= hole.w)
    {
        texel = ivec2(q.x, q.y - hole.w + hole.y);
    }
    else if (q.x < hole.x)
    {
        texel = ivec2(q.y - hole.y, int(uRingRows.x) + q.x);
    }
    else
    {
        texel = ivec2(q.y - hole.y, int(uRingRows.y) + q.x - hole.z);
    }
    vec2 field = texelFetch(uField, texel, 0).rg;
#else
    vec2 field = texelFetch(uField, ivec2(floor((vPos - uFieldOrigin) * uFieldTexelScale)), 0).rg;
#endif
    vec2 gatherUV = vPos * uGatherUVScale + uGatherUVOffset;
    vec3 col = textureLod(uGather, gatherUV, 0.0).rgb;

    // neon.frag's tone map - the same function, from neon-grade.glsl. The gain
    // scales a field baked at another intensity to this one; at exactly 1 the
    // product is fa's own bits, so a held config composites what it always did.
    // The segments' term takes no gain: neon.frag's segment emission does not
    // read the intensity (only the bloom's reach does, which the arc term's
    // gain ignores too). Branching on a uniform, so a config with no segments
    // composites exactly what it did before the field had a second channel.
    vec3 linear = col * (field.r * uFieldGain);
    if (uFieldSegments != 0) {
        vec3 segColHue = textureLod(uGatherSeg, gatherUV, 0.0).rgb;
        linear += segColHue * field.g;
    }
    vec3 result = neonToneMap(linear);

#ifdef NEON_FIELD_RING
    // The one-sided cut and the cutoffs, which the ring's own shading applies
    // after its tone map as coverage (neon.frag at scale 1.0, where
    // blitOwnsCut is false): the same expressions, term for term - keep them
    // in step with neon.frag's, and with neon-blit.frag's below 1.0. The field
    // holds Fa from before them (NEON_FIELD_BAKE returns at the grade), and
    // what neon.frag discarded ahead of them is a 0 texel, which composites to
    // nothing. Skipped on a uniform when neither is set: every factor is then
    // exactly 1 (a disabled side's sentinel saturates its smoothstep). The
    // derivative is in uniform control flow, so defined.
    if (uGlowSide != GLOW_SIDE_BOTH || uInsideCutoff < 0.5 * CUTOFF_NEUTRALISED ||
        uOutsideCutoff < 0.5 * CUTOFF_NEUTRALISED)
    {
        vec2  halfSize = uRectSize * 0.5;
        float d        = sdRoundBox(vPos, halfSize, uCornerRadius);
        float sideAA   = max(sdRoundBoxFwidth(vPos, halfSize, uCornerRadius), 1e-6);
        float sideSoft = max(uGlowSideSoftness, sideAA);
        float sideBack = 0.5 * sideAA;
        float inHalf   = 0.5 * max(uInsideCutoffSoftness,  sideAA);
        float outHalf  = 0.5 * max(uOutsideCutoffSoftness, sideAA);
        float inMid    = uInsideCutoff  + 0.5 * max(uInsideCutoffSoftness,  0.0);
        float outMid   = uOutsideCutoff + 0.5 * max(uOutsideCutoffSoftness, 0.0);
        float dOut;
        float dIn;
        if (uGlowSide == GLOW_SIDE_INSIDE)
        {
            dOut = -CUTOFF_NEUTRALISED;
        }
        else
        {
            dOut = bandOuterDistance(vPos, d, halfSize, uCornerRadius, outMid);
        }
        if (uGlowSide == GLOW_SIDE_OUTSIDE)
        {
            dIn = CUTOFF_NEUTRALISED;
        }
        else
        {
            dIn = bandInnerDistance(d, inMid);
        }
        if (uGlowSide == GLOW_SIDE_INSIDE)
        {
            result *= 1.0 - smoothstep(sideBack - sideSoft, sideBack, d);
        }
        else if (uGlowSide == GLOW_SIDE_OUTSIDE)
        {
            result *= smoothstep(-sideBack, sideSoft - sideBack, d);
        }
        result *= smoothstep(-inHalf, inHalf, dIn);
        result *= 1.0 - smoothstep(-outHalf, outHalf, dOut);
    }
#endif

    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
#ifdef NEON_FIELD_RING
    // The output dither, as the ring's own shading writes it (neon.frag): this
    // is a write to the caller's framebuffer. Pass 1c's to the reduced buffer
    // is not dithered; the blit is.
    result = neonDither(result);
#endif
    fragColor = vec4(result, alpha);
}

)shadersrc";

                static const char *const BLIT_FRAG_SRC = R"shadersrc(#version 330 core
precision highp float;

// Plain composite of a reduced-resolution buffer back over the caller's
// framebuffer: LensFlareRenderer's scaled path. Drawn with neon.vert over an
// NDC quad at an identity uMVP, so vPos is NDC. (SpotlightRenderer used this
// too until its scaled path needed the dither its own blit adds - see
// spotlight-blit.frag.)
//
// Bilinear upsampling of premultiplied colour + coverage alpha is fringe-free,
// and the caller's premultiplied-over blend does the rest.
//
// Both layers used to compile neon-blit.frag for this and upload
// GlowSide::BOTH to switch off the neon's one-sided cut, which neither has a
// rect to apply. That made every neon-only addition to that shader something
// two unrelated renderers had to remember to disable. This is exactly what
// neon-blit.frag computes with its cut off - the same uv, and src * 1.0 is src
// - so moving them here changed no pixel. Keep it this small: a layer that
// needs more than a composite wants its own blit, as the neon and the
// spotlight have.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    fragColor = texture(uSource, uv);
}

)shadersrc";

                static const char *const BLACK_RECT_FRAG_SRC = R"shadersrc(#version 330 core
// ---------------------------------------------------------------------------
// The rounded box's signed distance, its antialiasing width and the cutoff
// band's two boundaries, shared by neon.frag, neon-blit.frag, neon-field.frag
// and black-rect.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it into the four
// programs that have to agree on the shape to the pixel - the glow, the
// scaled path's blit (which re-applies the glow's cut and cutoffs at full
// resolution), the ring field's composite (which re-applies the ring's) and
// the opaque fill (which has to register with the glow's edge) - so the four
// stay one copy. It reads no uniform and no constant.
// ---------------------------------------------------------------------------

precision highp float;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// One pixel's width in sdRoundBox's units, for the box filter across the
// outline: what fwidth(d) returns along a straight stretch of it, taken from
// the distance's analytic gradient instead of from d's own differences.
//
// fwidth(d) differences d across a 2x2 quad, and at a sharp corner that is not
// a derivative. On the vertex pixel the quad can hold BOTH outside neighbours:
// d then steps a whole pixel along each axis, fwidth reads 2, and every ramp
// sized from it doubles there - the corner pixel of a one-sided cut came out
// at 0.84 of the other three corners', the fill's at 0.75. The quad's parity
// decides which corner, so one rect showed it at its top-left and the next at
// no corner at all (V26). Along the inside diagonal it read 0 or 2 the same
// way. The gradient is one-sided by construction - a pixel takes the edge it
// is nearest - so it is 1 on every corner pixel, as along the edges, and
// fwidth's value wherever d is smooth.
//
// |gradient| per axis times that axis's pixel, fwidth(p): exact because every
// caller's p is the rect's own frame through an unrotated orthographic
// transform - vPos, or gl_FragCoord less the centre - so each screen axis
// moves one axis of p. q and length(max(q, 0)) are sdRoundBox's own, so a
// caller taking both pays for the length once. A derivative: take it in
// uniform control flow.
float sdRoundBoxFwidth(vec2 p, vec2 b, float r) {
    vec2  q  = abs(p) - b + r;
    vec2  qp = max(q, 0.0);
    float l  = length(qp);
    vec2  g  = (q.x > 0.0 && q.y > 0.0) ? qp / l
             : ((q.x > q.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    return dot(g, fwidth(p));
}

// --- Band boundary distances -------------------------------------------
// The band's two boundaries, expressed as signed distances: dIn >= 0 means
// "past the inside cutoff", dOut <= 0 means "within the outside cutoff".
//
// INNER: plain Euclidean (d + cut). Inside the shape the rounded-box SDF is
// already per-axis, so the inner boundary is square at cornerRadius 0 and a
// correct parallel curve (radius r - cut) above it. Nothing to fix.
//
// OUTER: Euclidean too whenever cornerRadius > 0, where it is exactly d - cut.
// That is the parallel curve, so the band keeps a uniform width and the opaque
// fill covers precisely as far as the light reaches - no black bulging past
// the glow at the corners.
//
// The one exception is cornerRadius == 0: the parallel curve of a SHARP corner
// is an arc of radius `cut`, so a rect the designer asked to be square comes
// out with rounded outer corners. There, and only there, offset the box
// per-axis instead to keep the corner square. The band is then ~1.41x wider
// measured diagonally across that corner, which is unavoidable - a uniform
// width and a square outer corner cannot both hold at a sharp corner.
//
// Disabled cutoffs arrive as a huge sentinel and still no-op: dIn goes hugely
// positive, dOut hugely negative, so both masks evaluate to 1.
float bandOuterDistance(vec2 p, float d, vec2 halfSize, float r, float cut) {
    if (r > 1e-4) { return d - cut; }
    vec2 b = halfSize + vec2(cut);
    return sdRoundBox(p, b, 0.0);
}
float bandInnerDistance(float d, float cut) {
    return d + cut;
}

precision highp float;

// Opaque background pass drawn behind the neon when
// config.neon.opaqueMode != NONE. Fills a rounded-rect band with
// `uOpaqueColor` (default black). The silhouette comes from an analytic
// rounded-box SDF read off gl_FragCoord (NOT an interpolated varying), so it
// is independent of the geometry it is drawn on - which is what lets the CPU
// bound this pass with a ring sized to the band (see
// NeonRenderer::setupFillGeometry) instead of shading the whole viewport to
// paint a 20 px band.
//
// highp + gl_FragCoord keeps the SDF exact on mobile GLES (Mali/Tizen), where
// a mediump varying carrying pixel coordinates would lose precision and a
// vertex(highp)->fragment(mediump) varying mismatch can even fail to link.
//
// Cutoffs are positive pixel distances measured from the rect edge along
// their respective sides. They are the FILL's own pair (NeonConfig::
// opaqueInsideCutoff / opaqueOutsideCutoff), not the glow's - the two layers
// are bounded independently.
//
// uInsideCutoff / uOutsideCutoff are each Cutoff::size, where the fill's
// fade STARTS: solid up to size, gone by size + softness. The ranges in the
// table below are where the fill is solid; main() places the fade beyond
// them (inMid / outMid).
//
//   NONE    - never dispatched; the CPU skips the pass entirely.
//   OUTSIDE - fill 0 <= d <= outsideCutoff.
//   INSIDE  - fill -insideCutoff <= d <= 0.
//   BOTH    - fill -insideCutoff <= d <= outsideCutoff (the whole band).
//   ALL     - rarely dispatched: coverage is 1 at every pixel, so the CPU
//             normally expresses it as a scissored glClear and no shader runs.
//
// BOTH reaches that same clear whenever NEITHER cutoff is enabled, which is
// the default state of both. Disabled cutoffs arrive as a huge sentinel, so
// dIn saturates positive and dOut negative and the arm below returns coverage
// 1 at every fragment - identical output for the price of shading the whole
// viewport. NeonRenderer::FillsWholeViewport is what catches it.
//
// "Rarely", not "never": a clear ignores the depth and stencil tests, so when
// the host has either enabled the CPU keeps the draw (NeonRenderer::
// ClearClipsLikeDraw) and dispatches this shader over a fullscreen quad. Both
// coverage-1 paths below are therefore live code on that route, not dead
// arms - keep them correct.
//
// The d == 0 rect edge gets exact 1 px box-filter coverage from its pixel
// width (sdRoundBoxFwidth - fwidth(d) without its doubling on a sharp corner);
// each cutoff boundary gets its own side's feather (uInsideCutoffSoftness /
// uOutsideCutoffSoftness). See main().

#define OPAQUE_MODE_NONE    0
#define OPAQUE_MODE_OUTSIDE 1
#define OPAQUE_MODE_INSIDE  2
#define OPAQUE_MODE_BOTH    3
#define OPAQUE_MODE_ALL     4

out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform vec2  uRectCenter;     // rect centre in window pixels (gl_FragCoord space, y-up)
uniform int   uOpaqueMode;
uniform float uInsideCutoff;          // Cutoff::size: positive distance INSIDE the edge where the inside fade starts. Disabled sides collapse to a huge sentinel CPU-side.
uniform float uInsideCutoffSoftness;  // feather width in px, running on from uInsideCutoff toward the centre (NeonConfig::opaqueInsideCutoff.softness).
uniform float uOutsideCutoff;         // Cutoff::size: positive distance OUTSIDE the edge where the outside fade starts. Disabled sides collapse to a huge sentinel CPU-side.
uniform float uOutsideCutoffSoftness; // feather width in px, running on from uOutsideCutoff away from the rect (NeonConfig::opaqueOutsideCutoff.softness).
uniform vec4  uOpaqueColor;           // fill colour; only .rgb used today, .a reserved for a later partial-fill pass

// sdRoundBox and the band boundaries (bandOuterDistance, bandInnerDistance)
// are the glow's own, from neon-sdf.glsl - the one copy that keeps this edge
// registered with neon.frag's.

void main() {
    // ONE straight-line path to a single write at the bottom: no early return
    // above the derivative, and no discard anywhere. Both of those are load
    // bearing, and each cost a separate measurement to learn.
    //
    // NO EARLY RETURN. The NONE and ALL guards used to sit here as
    // `fragColor = ...; return;`, ahead of the derivative below. They read as
    // free - they branch on a uniform, so every lane in the draw takes the
    // same side - but the compiler does not get to assume that, and a
    // derivative downstream of a return it cannot prove uniform lands the
    // whole shader on a slower path. Measured at 3840x2160 on a fill covering
    // the surface: 0.29 ms before the returns were added, 0.41 ms with them,
    // 0.26 ms once they moved back into the chain below. That is a ~35%
    // penalty paid by EVERY fragment in EVERY mode, to skip work in two modes
    // the CPU never dispatches (NONE is guarded caller-side, ALL is a
    // scissored glClear). Keep the guards where they are - folded into the
    // coverage chain, below the derivative - and if a new one is ever needed,
    // it goes there too, not up here.
    //
    // NONE is not tested at all: it matches no arm of that chain, so it falls
    // through at coverage 0 and writes transparent black, which is the correct
    // no-op for this pass's blend.

    vec2  localPos = gl_FragCoord.xy - uRectCenter;
    vec2  halfSize = uRectSize * 0.5;
    float d        = sdRoundBox(localPos, halfSize, uCornerRadius);

    // Feather widths. Every width here is a TOTAL width in px, floored at one
    // pixel so nothing can stair-step; the two cutoff ramps are then stored as
    // HALF of that, because smoothstep takes the ramp's extent either side of
    // its CENTRE - which is the fade's midpoint (inMid / outMid, below),
    // softness/2 past the cutoff, not the cutoff itself.
    //   aa    = pixel-crisp AA at boundaries where no soft join is required
    //           (the geometric d=0 rect edge under OUTSIDE/INSIDE modes: the
    //           neon emission is at its brightest there and fully occludes the
    //           fill, so a wider feather would only bleed the fill sideways for
    //           no visual gain). A total width.
    //   inHalf / outHalf
    //         = HALF of each side's cutoff feather: the TOTAL width is the
    //           side's uInsideCutoffSoftness / uOutsideCutoffSoftness floored
    //           at aa, and the ramp runs over [-half, +half] around that
    //           side's fade midpoint - so, at or above the floor, from the
    //           cutoff to cutoff + softness. The fill's own Cutoff pair,
    //           independent of the glow's so fill and emission can taper at
    //           different rates - and independent of each other, so each side
    //           feathers on its own. Applied beyond the -insideCutoff and
    //           +outsideCutoff boundaries respectively so the fill fades off
    //           gently instead of stamping a hard rectangle.
    //
    // Both ramps used to be written as smoothstep(-w, w, x), which spans 2w -
    // TWICE the width being asked for. Two consequences, and they are the same
    // root cause as the d == 0 bug documented below:
    //
    //   - At softness 0 the fallback to aa gave a 2 px ramp centred on
    //     each cutoff, so the outermost and innermost pixel of the band were
    //     both partially covered and the fill read ~1 px narrower per side than
    //     the cutoff asked for.
    //   - At any non-zero softness the fade ran 2x wider than the documented
    //     "feather width in pixels".
    //
    // Halving fixed both: each ramp spans exactly its width, so a pixel wholly
    // inside the band is fully covered and a softness of S px feathers over
    // S px.
    float aa      = max(sdRoundBoxFwidth(localPos, halfSize, uCornerRadius), 1e-6);
    float inHalf  = 0.5 * max(uInsideCutoffSoftness, aa);
    float outHalf = 0.5 * max(uOutsideCutoffSoftness, aa);

    // WHERE each fade sits: it starts at the cutoff and runs the requested
    // softness beyond it, so its MIDPOINT is softness/2 past the cutoff. The
    // band distances in the arms below are measured from that midpoint and
    // the ramps built symmetrically about it over the floored width - exactly
    // cutoff -> cutoff + softness at or above the aa floor. Below the floor
    // the one-pixel ramp stays centred on the requested fade rather than
    // starting at the cutoff, so a softness-0 edge's 50% point is the cutoff
    // itself - the same rule neon.frag follows, where anchoring the floored
    // ramp at the cutoff would move the edge with resolutionScale. Plain ALU,
    // so it may sit up here with the other widths.
    float inMid  = uInsideCutoff  + 0.5 * max(uInsideCutoffSoftness,  0.0);
    float outMid = uOutsideCutoff + 0.5 * max(uOutsideCutoffSoftness, 0.0);

    // The d == 0 edge is covered by an EXACT box filter, not a smoothstep.
    //
    // For a straight edge with |grad d| = 1, a pixel whose centre sits at
    // signed distance d is covered by exactly clamp(0.5 - d/aa, 0, 1) - a ramp
    // ONE pixel wide, spanning d in [-aa/2, +aa/2]. smoothstep(-aa, aa, d)
    // spans TWO, so it shaded pixels that are wholly inside the shape.
    //
    // That showed up as "the outermost 1 px is not black" whenever the
    // geometry ends on a pixel boundary - a 1920x1080 rect filling a 1920x1080
    // surface, i.e. the on-device fullscreen case. The outer pixel's centre is
    // at gl_FragCoord 0.5, so d = -0.5: half a pixel inside, hence fully
    // covered, but the doubled ramp returned 1 - smoothstep(-1, 1, -0.5)
    // = 0.844 and let 15.6% of the background through all the way round.
    // OUTSIDE had the mirror bug, tinting that same ring at 0.156 when it
    // should not touch the interior at all.
    //
    // Normally invisible because the neon filament peaks at d = 0 and covers
    // that pixel - DebugConfig::opaqueOnly is what exposes it.
    //
    // Linear, not smoothstep, on purpose: this IS the analytic coverage, and
    // it matches the "pixel-crisp" intent stated above. The cutoff boundaries
    // keep their smoothstep even though they are now the same width, because
    // there the ramp is an artistic feather that can run tens of px wide
    // (the cutoffs' softness), and a linear alpha ramp that wide shows Mach banding
    // at its ends. At the aa floor the two shapes differ by at most ~0.09
    // coverage on the single boundary pixel, which is not resolvable.
    float edgeIn  = clamp(0.5 - d / aa, 0.0, 1.0); // 1 inside the rect, 0 outside
    float edgeOut = 1.0 - edgeIn;                  // mirror, for OUTSIDE

    // The band boundaries are computed INSIDE the arms that read them, not
    // once above the chain. Each arm uses at most one of them - OUTSIDE never
    // looks at the inner boundary, INSIDE never at the outer - so hoisting
    // them made every OUTSIDE and INSIDE fragment pay for a distance it then
    // discarded. bandOuterDistance is the one that matters: at cornerRadius 0
    // it is a whole second sdRoundBox, length() and all, and cornerRadius 0 is
    // not an exotic setting.
    //
    // MEASURED NOTHING, and that is worth writing down so nobody re-measures
    // it hoping otherwise. INSIDE at cornerRadius 0, cutoffs off, fill
    // isolated with debug.opaqueOnly, 3600x2126, min of 200 frames around
    // glFinish, three runs a side: 0.79 / 0.84 / 0.82 ms hoisted against
    // 0.81 / 0.74 / 0.87 ms sunk. That is one distribution, not two - the
    // Apple GLSL compiler was already sinking the pure computation into the
    // arm that consumes it, exactly as a compiler is free to do.
    //
    // Kept anyway, on two grounds and neither of them speed on this driver:
    // the source now says what it means, and this project's real target is a
    // Mali / Tizen compiler nobody here has measured. Also because this
    // shader has already been bitten once by trusting a compiler to do the
    // obvious thing with its control flow - see the 35% note at the top of
    // main(), which is why the guards below are arms rather than early
    // returns. Being explicit is free; assuming was not.
    //
    // Note what did NOT move: `aa`, `inHalf`, `outHalf`, `edgeIn` and
    // `edgeOut` all stay above the chain, because `aa` comes from a derivative.
    // A derivative must not end up downstream of control flow the compiler
    // cannot prove uniform - that IS the 35% mistake documented at the top.
    // Only branch-free ALU moves down here; the derivative stays put.
    float coverage = 0.0;
    if (uOpaqueMode == OPAQUE_MODE_ALL) {
        // Coverage 1 everywhere - none of the shaping above changes it.
        // Reached only when the CPU could not substitute a clear, i.e. with
        // the depth or stencil test enabled (see the mode table at the top of
        // this file), so it is rare rather than dead. See the note at the top
        // of main() for why it is an arm of this chain rather than an early
        // return.
        coverage = 1.0;
    } else if (uOpaqueMode == OPAQUE_MODE_OUTSIDE) {
        // 0 <= d <= outsideCutoff. No inner boundary here.
        float dOut = bandOuterDistance(localPos, d, halfSize, uCornerRadius, outMid);
        float rise = edgeOut;
        float fall = 1.0 - smoothstep(-outHalf, outHalf, dOut);
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_INSIDE) {
        // -insideCutoff <= d <= 0. No outer boundary here - this is the arm
        // that was paying for a second sdRoundBox it never read.
        float dIn  = bandInnerDistance(d, inMid);
        float rise = smoothstep(-inHalf, inHalf, dIn);
        float fall = edgeIn;
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_BOTH) {
        // -insideCutoff <= d <= +outsideCutoff (the full glow band). The one
        // arm that genuinely needs both boundaries.
        float dIn  = bandInnerDistance(d, inMid);
        float dOut = bandOuterDistance(localPos, d, halfSize, uCornerRadius, outMid);
        float rise = smoothstep(-inHalf, inHalf, dIn);
        float fall = 1.0 - smoothstep(-outHalf, outHalf, dOut);
        coverage   = rise * fall;
    }

    // No `if (coverage <= 0.0) discard;` here, and it is safe to leave out:
    // under this pass's glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) a coverage
    // of 0 gives dst = 0 + dst * (1 - 0) = dst, so writing transparent black
    // is bit-exact with discarding - 333 offscreen scenes come back
    // byte-identical either way.
    //
    // It buys nothing measurable HERE: restoring the discard and re-running
    // the 3840x2160 cases came back inside noise, because the ring in
    // NeonRenderer::setupFillGeometry already keeps most zero-coverage
    // fragments from being rasterised at all, and the cases it cannot bound
    // are the ones where coverage is 1 and the discard never fires anyway.
    // It is left out because a shader with no discard is the friendlier shape
    // for the Mali / Tizen targets, where discard defeats forward pixel kill -
    // UNVERIFIED on device, but it costs nothing to be on the right side of.

    // Coverage-weighted premultiplied output for
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA): opaque uOpaqueColor.rgb
    // where coverage == 1, smooth AA at boundaries, off-side untouched.
    // uOpaqueColor.a is intentionally not applied yet - reserved for a
    // later premultiplied partial-fill pass.
    fragColor = vec4(uOpaqueColor.rgb * coverage, coverage);
}

)shadersrc";

                static const char *const NEON_LUT_DEBUG_FRAG_SRC = R"shadersrc(#version 330 core
precision highp float;

// Debug: sample the baked gradient LUT along the strip's x axis so the
// colour ring baked by GradientRingLUT can be verified visually.
// Drawn on a small quad centred on the rect origin.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uGradientLUT;
uniform vec2  uStripHalfSize; ///< half-width, half-height of the strip in local px.
uniform float uTime;
uniform float uHueRotationRate;

void main() {
    // Map local x in [-halfW, +halfW] to u in [0, 1], then apply the same
    // time-based hue scroll as the neon shader so this preview matches what
    // the perimeter is actually reading each frame (REPEAT-wrapped LUT).
    float u = vPos.x / (2.0 * uStripHalfSize.x) + 0.5;
    u -= uTime * uHueRotationRate;
    vec4 lut = texture(uGradientLUT, vec2(u, 0.5));
    // Scale by the baked alpha so the strip previews the EMISSION, matching
    // how neon.frag applies stop alpha to the magnitude. A stop faded to
    // alpha 0 reads black here, exactly as it renders on the perimeter.
    // The strip itself stays fully opaque - it is drawn with blending off to
    // overwrite the glow underneath.
    fragColor = vec4(lut.rgb * lut.a, 1.0);
}

)shadersrc";

                static const char *const NEON_STOP_MARKER_FRAG_SRC = R"shadersrc(#version 330 core
precision highp float;

// Debug: draws a filled disc in uMarkerColor with a thin white ring so the
// marker reads on any background - used to spot each colour-stop position
// on the perimeter (see NeonRenderer::Render, showColorStops).
//
// The vertex data is a unit quad in [-1, +1]; the C++ side scales the model
// matrix by the desired marker radius so vPos here is always in disc space.

in vec2 vPos;
out vec4 fragColor;

uniform vec4 uMarkerColor;

void main() {
    float d = length(vPos);
    if (d > 1.0) discard;

    // 0.0-0.85 -> solid stop colour
    // 0.85-0.95 -> white ring
    // 0.95-1.0 -> alpha fade to zero (anti-alias)
    float ring  = smoothstep(0.85, 0.9, d);
    float alpha = 1.0 - smoothstep(0.95, 1.0, d);
    vec3  rgb   = mix(uMarkerColor.rgb, vec3(1.0), ring);
    fragColor = vec4(rgb, alpha);
}

)shadersrc";

                static const char *const DROPLETS_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_DROPLETS_TUNING_H_
#define _EDGE_LIGHTING_DROPLETS_TUNING_H_

// ---------------------------------------------------------------------------
// Shared droplet band constants - single source of truth.
//
// Consumed by BOTH:
//   - the droplet shader (droplets.frag), where CMake text-injects this file
//     via @DROPLETS_TUNING@ in shaders.h.in, and
//   - the C++ renderer (droplets-renderer.cpp), which #includes it to size the
//     draw quad.
//
// Only constants that BOTH sides must agree on live here. The droplet field's
// own look constants (CELL_UV, TRAIL_FLAT_SPAN, TRAIL_FLAT_DROPS) stay in
// droplets.frag: the renderer never reasons about them, and a constant with
// one consumer does not need two homes.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a single definition that compiles
// as both GLSL and C++ has to be a plain #define. Same reasoning as
// neon-tuning.h.
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off

/// How far past each band boundary the shader still shades, in band widths.
///
/// The band coordinate runs 0 at the inner boundary to 1 at the outer, and
/// droplets.frag's early bail keeps [-GUARD, 1 + GUARD] rather than [0, 1]:
/// BandFade needs a drop's neighbourhood, not just the drop, to fade a whole
/// drop by where its centre sits.
///
/// This is the OUTER bound on anything the pass can write, which is exactly
/// what the renderer's draw quad has to clear - so the two read it from here.
/// Widen the bail without widening this and the quad clips the band's outer
/// edge into a straight line along all four sides.
#define DROPLET_BAND_GUARD 0.25

// clang-format on

#endif // _EDGE_LIGHTING_DROPLETS_TUNING_H_

precision highp float;

// ---------------------------------------------------------------------------
// Rain-on-glass droplets, confined to a band along the rounded-rect perimeter.
//
// Rain falls DOWN. The droplet field is hashed in screen space with a single
// global gravity direction, exactly as real rain behaves - it does not flow
// around the perimeter loop, which would read as circulating water rather than
// rain. What the perimeter geometry controls is *where* the rain is allowed to
// show and *how big* the drops are:
//
//   * A rounded-box SDF masks everything outside a band of @c uBandWidth
//     pixels, on the side selected by @c uGlowSide.
//   * Droplet cell size is derived from @c uBandWidth rather than from the
//     viewport, so drops fit the band however thin it is. (Sizing off the
//     viewport was the original bug: at a 20px band you saw slivers of drops
//     tens of pixels across.)
//
// Because gravity is global and the grid is screen-space, the field never
// shears or tears - but that means the band's orientation matters, and the
// band is only @c uBandWidth pixels wide across. Two mechanisms keep the field
// from being guillotined by that, neither of which moves a drop:
//
//   * Whole-drop fade (@ref BandFade). A drop is faded by where its CENTRE
//     sits across the band, not by where the current fragment sits. Drops
//     therefore fade in and out as a whole while crossing, instead of being
//     sliced along a straight line with a flat, rim-less cut face. The fade
//     window scales with each drop's own radius, so it holds at any band width
//     or lane count.
//   * Trail LENGTH is orientation-gated. Down a vertical run a trail can
//     stretch the whole cell, because it runs along the band. Across a
//     horizontal run the only room available is the band's thickness, so the
//     tail is cut to a fraction of that: a short teardrop that fits, rather
//     than a full-length streak sheared flat into a rectangle. Trails also
//     inherit their own head's band fade, so a tail never outlives the drop
//     that drew it.
//
// Layer *amplitudes* are likewise modulated by how vertical the local edge is:
// trickles on the sides, condensation beads along the top and bottom. All of
// this is amplitude only - never position - precisely so the grid stays
// unsheared.
//
// Both mechanisms have to stay continuous through the corners, which rules out
// reading the SDF's GRADIENT. A box SDF creases along the medial diagonal
// inside every corner: the gradient flips 90 degrees over about a pixel there,
// so anything derived from it inherits a hard diagonal seam. @ref BandAcross
// samples the field itself at the point of interest and the orientation mix
// below reads per-axis face distances; both are continuous everywhere.
//
// @c uGlowSide selects which side of the edge the band occupies:
//   OUTSIDE -> band grows outward from the edge.
//   INSIDE  -> band grows inward.
//   BOTH    -> band straddles the edge, centred on it.
//
// Drops are self-lit: transparent body plus a crescent rim and a specular dot.
// There is no framebuffer capture and no refraction pass - refraction was
// invisible over the smooth neon gradient (the only backdrop this band ever
// sees), so the whole capture/lens/wet-glass path was removed and only this
// highlight-only shading remains.
//
// Droplet field adapted from the well-known Shadertoy rain technique
// (grid-hashed trickling drops with trails; see "Heartfelt" by Martijn
// Steinrucken / The Art of Code and its many forks, e.g. tdG3Rw).
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

/// One droplet cell spans this many uv units. The droplet grid inside
/// DropLayer is (12, 2) cells per uv unit with a 6:1 tall aspect, so dividing
/// screen pixels by (CELL_UV * cellPx) makes one cell exactly cellPx wide -
/// and 6 * cellPx tall, which is the room the trail needs.
#define CELL_UV 12.0

/// Trail length on a horizontal run, as a fraction of the band's thickness.
/// The band is all the room a tail has there, so this has to stay well under
/// 1.0.
#define TRAIL_FLAT_SPAN 0.7

/// ...and no longer than this many drop diameters. The band bound alone is
/// the real constraint, but on its own it makes the tail's character depend on
/// @c uLanes: at one lane a tail is shorter than a drop is wide, at four it
/// would be three times longer, which reads as a streak rather than a bead
/// being dragged. Taking the smaller of the two keeps it a bead at any count.
#define TRAIL_FLAT_DROPS 2.0

in vec2 vPos; ///< Rect-local px from the band quad; we drive UVs off gl_FragCoord.
out vec4 fragColor;

uniform vec2  uRectSize;          ///< Rect size (px).
uniform vec2  uRectCenter;        ///< Rect centre (px) in framebuffer space.
uniform float uCornerRadius;
uniform float uTime;
uniform float uAmount;
uniform float uSpeed;
uniform int   uLanes;             ///< Droplet lanes across the band (>= 1).
uniform vec4  uTint;
uniform int   uGlowSide;          ///< GLOW_SIDE_BOTH / INSIDE / OUTSIDE.
uniform float uGlowSideSoftness;  ///< Band-boundary feather width in pixels.
uniform float uBandWidth;         ///< Band thickness in pixels; also sets droplet size.
uniform float uBandOffset;        ///< Gap in pixels between the rect edge and the band's inner boundary.

// Rect frame for the current fragment. Set once at the top of main() and read
// by the droplet field, which would otherwise need these threaded through
// three functions that are each evaluated three times.
float gBandWidth;   ///< Band thickness in px (>= 1).
vec2  gHalfSize;    ///< Rect half-extents in px.
vec2  gRectLocal;   ///< This fragment's position in rect-local px.
float gRuns;        ///< 1 where the band runs vertically, 0 where horizontal.

#define S(a, b, t) smoothstep(a, b, t)

// ---------------------------------------------------------------------------
// Hash helpers
// ---------------------------------------------------------------------------

vec3 N13(float p) {
    vec3 p3 = fract(vec3(p) * vec3(0.1031, 0.11369, 0.13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
}

float N(float t) {
    return fract(sin(t * 12345.564) * 7658.76);
}

float Saw(float b, float t) {
    return S(0.0, b, t) * S(1.0, b, t);
}

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// ---------------------------------------------------------------------------
// Band
// ---------------------------------------------------------------------------

/// Band coordinate of an arbitrary rect-local point: 0 at the band's inner
/// boundary, 1 at its outer. The band mask and the per-drop fade both go
/// through here, so the two can never disagree about where the band is.
float BandAcross(vec2 localPx) {
    float sdl = sdRoundBox(localPx, gHalfSize, uCornerRadius);
    float depth; ///< Into the band, measured from its inner boundary outward.
    if (uGlowSide == GLOW_SIDE_INSIDE)
    {
        depth = -sdl - uBandOffset;
    }
    else if (uGlowSide == GLOW_SIDE_OUTSIDE)
    {
        depth = sdl - uBandOffset;
    }
    else
    {
        // Straddle the edge: the band is centred on sd = 0.
        depth = sdl + gBandWidth * 0.5 - uBandOffset;
    }
    return depth / gBandWidth;
}

/// Fade a whole drop by where its CENTRE sits across the band, rather than
/// letting the band mask slice its body.
///
/// @param offsetPx fragment-minus-centre, in pixels.
/// @param radiusPx the drop's radius, in pixels.
///
/// The centre's band coordinate is sampled from the field at the centre
/// itself, not linearised from this fragment along the SDF normal. Linearising
/// is cheaper but wrong at the corners: the box SDF creases along the medial
/// diagonal there, so the normal flips 90 degrees over about a pixel and the
/// two halves of one drop resolve to completely different centres - which put
/// a hard straight cut through every drop straddling that diagonal.
///
/// A drop is at full brightness once it clears both boundaries by its own
/// radius and gone once its centre is a radius outside, so the window scales
/// with drop size - correct for both trickle layers, the static beads, and any
/// @c uLanes setting. The radius ratio is clamped below 0.5 so the two ramps
/// cannot overlap; without it a drop wider than the band could never reach
/// full brightness.
float BandFade(vec2 offsetPx, float radiusPx) {
    float aC = BandAcross(gRectLocal - offsetPx);
    float r = clamp(radiusPx / gBandWidth, 0.02, 0.45);
    return S(-r, r, aC) * S(1.0 + r, 1.0 - r, aC);
}

// ---------------------------------------------------------------------------
// Droplet field (screen space, gravity-aligned)
// ---------------------------------------------------------------------------
//
// Cells are 6:1 tall so each drop has room for the trail it leaves behind.
// uv.y increases upward (gl_FragCoord convention), so advancing the pattern
// along +uv.y makes drops travel downward.
//
// @c uvToPx is how many pixels one uv unit spans for this call. It is what
// ties the field back to the band, and it differs per layer - the fine layer
// is sampled at uv * 1.85, so its cells are 1.85x smaller on screen.

vec2 DropLayer(vec2 uv, float t, float uvToPx) {
    vec2 baseUV = uv;
    uv.y += t * 0.75;
    vec2 a = vec2(6.0, 1.0);
    vec2 grid = a * 2.0;
    vec2 id = floor(uv * grid);

    float colShift = N(id.x); ///< Per-column phase so columns do not fall in lockstep.
    uv.y += colShift;
    id = floor(uv * grid);

    vec3 n = N13(id.x * 35.2 + id.y * 2376.1);
    vec2 st = fract(uv * grid) - vec2(0.5, 0.0);

    float x = n.x - 0.5;
    float y = baseUV.y * 20.0;
    float wiggle = sin(y + sin(y));
    x += wiggle * (0.5 - abs(x)) * (n.z - 0.5);
    x *= 0.7;

    float ti = fract(t + n.z);
    y = (Saw(0.85, ti) - 0.5) * 0.9 + 0.5;
    vec2 p = vec2(x, y);
    float d = length((st - p) * a.yx);
    // A cell is cellWidthPx wide and 6x that tall, so (st - p) * a.yx is the
    // offset from the centre in units of cell WIDTHS - isotropic, which is what
    // makes `d` a true radius and what lets one scalar convert it to pixels.
    // The uv.y translations above are pure translations, so they leave this
    // centre-to-fragment offset untouched.
    float cellWidthPx = uvToPx / grid.x;
    // BandFade resolves the drop's centre from this offset, so evaluating it
    // anywhere in the cell - including far up the trail - yields the HEAD's
    // fade. The trail reuses it, which is what stops a tail outliving its drop
    // when the head fades out at a boundary.
    float headFade = BandFade((st - p) * a.yx * cellWidthPx, 0.4 * cellWidthPx);
    float mainDrop = S(0.4, 0.0, d) * headFade;

    // Trail length. Along a vertical run the tail can run to the top of the
    // cell. Across a horizontal run it only has the band's thickness to live
    // in, so it is cut to TRAIL_FLAT_SPAN of that - in pixels, which makes it
    // identical for both trickle layers despite their different cell sizes.
    // Shortening rather than deleting is the point: `r` also drives the tail's
    // width, so a short tail is a narrow one and tapers to a teardrop instead
    // of ending in the flat-topped rectangle a sheared full-length trail left.
    float flatPx = min(TRAIL_FLAT_SPAN * gBandWidth, TRAIL_FLAT_DROPS * 0.8 * cellWidthPx);
    float flatLen = min(flatPx / (6.0 * cellWidthPx), 1.0 - y);
    float trailLen = max(mix(flatLen, 1.0 - y, gRuns), 0.02);
    float r = sqrt(S(y + trailLen, y, st.y));
    float cd = abs(st.x - x);
    float trail = S(0.23 * r, 0.15 * r * r, cd);
    float trailFront = S(-0.02, 0.02, st.y - y);
    trail *= trailFront * r * r * headFade;

    // Beads shed along the trail.
    y = baseUV.y;
    float trail2 = S(0.2 * r, 0.0, cd);
    y = fract(y * 10.0) + (st.y - 0.5);
    float dd = length(st - vec2(x, y));
    float droplets = S(0.3, 0.0, dd) * trail2;

    float m = mainDrop + droplets * r * trailFront * headFade;
    return vec2(m, trail);
}

/// Static condensation beads - they fade in and out in place rather than
/// running, which is what rain actually does on a near-horizontal surface.
///
/// These beads are the ONLY thing that shows on horizontal runs of the band
/// (rain cannot streak down a horizontal edge), so the cell size is tuned to
/// give a comfortable per-band count rather than the sparse "condensation on
/// a windowpane" look the original tuning aimed for.
float StaticDrops(vec2 uv, float t, float uvToPx) {
    // Cells are square here, so one scalar converts cell units to pixels.
    float cellSizePx = uvToPx / (CELL_UV * 0.85);
    uv *= CELL_UV * 0.85;
    vec2 id = floor(uv);
    uv = fract(uv) - 0.5;
    vec3 n = N13(id.x * 107.45 + id.y * 3543.654);
    vec2 p = (n.xy - 0.5) * 0.7;
    float d = length(uv - p);
    float fade = Saw(0.025, fract(t + n.z));
    return S(0.3, 0.0, d) * fract(n.z * 10.0) * fade *
           BandFade((uv - p) * cellSizePx, 0.3 * cellSizePx);
}

vec2 Drops(vec2 uv, float t, float l0, float l1, float l2, float uvToPx) {
    float s = StaticDrops(uv, t, uvToPx) * l0;
    vec2 m1 = DropLayer(uv, t, uvToPx) * l1;
    vec2 m2 = DropLayer(uv * 1.85, t, uvToPx / 1.85) * l2;

    float c = s + m1.x + m2.x;
    c = S(0.3, 1.0, c);
    // m1/m2 are already scaled by l1/l2. The trail channel used to be weighted
    // a second time by l0/l1, which meant trails picked up the static-bead
    // amplitude - and that is boosted 1.6x on horizontal runs, exactly where
    // the band shears trails into flat rectangles.
    return vec2(c, max(m1.y, m2.y));
}

// ---------------------------------------------------------------------------

void main() {
    vec2 p = gl_FragCoord.xy - uRectCenter; ///< Rect-local pixels.
    vec2 halfSize = uRectSize * 0.5;
    float bandWidth = max(uBandWidth, 1.0);

    // Rect frame for the droplet field, which has to evaluate the band at
    // arbitrary points (drop centres), not just at this fragment.
    gBandWidth = bandWidth;
    gHalfSize = halfSize;
    gRectLocal = p;

    float across = BandAcross(p); ///< 0 at the inner boundary, 1 at the outer.

    // Early bail. A thin band is a small slice of the viewport, so rejecting
    // before any droplet work is where most of this pass's cost goes away.
    // The window is widened by DROPLET_BAND_GUARD on both sides because
    // BandFade reads a drop's neighbourhood, not just the drop.
    //
    // This is the OUTER bound on anything this shader can write, so the
    // renderer sizes its draw quad from the same constant - see
    // droplets-tuning.h. Fragments outside it never reach here at all now;
    // this stays as the exact bound, since the quad is a rectangle and the
    // band is a rounded ring inside it.
    if (across < -DROPLET_BAND_GUARD || across > 1.0 + DROPLET_BAND_GUARD)
    {
        discard;
    }

    // Band boundary feather, expressed in the same normalised units. Floored at
    // a quarter of the band rather than half a pixel: whatever still overhangs
    // after the per-drop fade has to vignette out, not be cut off.
    float soft = clamp(max(uGlowSideSoftness, bandWidth * 0.25) / bandWidth, 0.02, 0.5);
    float bandMask = S(0.0, soft, across) * S(1.0, 1.0 - soft, across);
    if (bandMask <= 0.0)
    {
        discard;
    }

    // --- Droplet grid ----------------------------------------------------
    // Cell size comes from the band, not the viewport, so drops fit the band
    // at any thickness. `lanes` is how many drops sit side by side across it.
    float lanes = float(max(uLanes, 1));
    float cellPx = bandWidth / lanes;
    float uvToPx = CELL_UV * cellPx;
    vec2 uv = gl_FragCoord.xy / uvToPx;

    float t = uTime * 0.2 * uSpeed;

    // --- Orientation-aware layer mix -------------------------------------
    // How vertical the local run of band is. `q` is the per-axis distance to
    // the rounded rect's faces, so q.x - q.y says which face is nearer:
    // strongly positive down the left/right runs, where rain can streak;
    // strongly negative along the top/bottom, where it can only bead; crossing
    // zero at the corners.
    //
    // This deliberately does not use the SDF gradient. The gradient is exactly
    // what creases along a corner's medial diagonal, and a trail term built on
    // it snapped between 0 and 1 across that line. Face distances are
    // continuous everywhere, and the transition scales with the band, so trail
    // length eases down over roughly one band width approaching a corner
    // instead of ending at a seam.
    vec2 q = abs(p) - halfSize + uCornerRadius;
    float runs = S(-bandWidth, bandWidth, q.x - q.y); ///< 1 where the band is vertical.
    gRuns = runs;

    float rain = clamp(uAmount, 0.0, 1.0);
    // Gravity is global: the trickling layers are active everywhere, not just
    // on vertical runs. On horizontal runs the drops just cross the band
    // vertically instead of streaking along its length, which is what falling
    // rain looks like passing through a narrow slit - BandFade is what makes
    // that crossing read as a drop fading through rather than a sliced one,
    // and the shortened tail keeps it a drop rather than a streak.
    // Static condensation stays as a mild base density and is still weighted a
    // little higher on the horizontal runs, where drops pass through quickly
    // and beads sit longer.
    float staticDrops = S(-0.5, 1.0, rain) * mix(1.6, 0.5, runs);
    float layer1 = S(0.25, 0.75, rain);
    float layer2 = S(0.0, 0.5, rain);

    vec2 c = Drops(uv, t, staticDrops, layer1, layer2, uvToPx);

    // Height-field gradient in cell space - independent of viewport size.
    //
    // Gated on c.x, which makes this the single most expensive thing the
    // shader can skip: the two taps are two more FULL evaluations of the
    // droplet field, so an ungated fragment pays for the field three times
    // over. Most in-band fragments are the gaps BETWEEN drops, where c.x is
    // exactly 0 (`Drops` closes with S(0.3, 1.0, ...), which returns a hard
    // zero below the threshold) and the two taps are pure waste.
    //
    // Skipping there is EXACT, not an approximation. nrm reaches the output
    // through exactly two terms, and c.x = 0 annihilates both:
    //
    //   rim  = pow(clamp(c.x * (1 - c.x) * 4, 0, 1), 1.5)  -> 0, and the
    //          `facing` weight built from nrm only scales that zero.
    //   spec = pow(max(0, dot(nrm, lightDir)), 16) * c.x    -> 0.
    //
    // So with c.x = 0 the fragment's colour and alpha do not depend on nrm at
    // all, and vec2(0) is as good as the real gradient. It has to stay finite
    // rather than undefined for that: dot(vec2(0), lightDir) is 0, so `facing`
    // lands at 0.5 and nothing downstream sees a NaN to propagate.
    //
    // Note this gates on c.x alone, NOT on the trail channel c.y. A fragment
    // with c.x = 0 and c.y > 0 still emits light through `bright`, but that
    // path never reads nrm either, so it is correctly served by the skip.
    vec2 nrm = vec2(0.0);
    if (c.x > 0.0)
    {
        vec2 e = vec2(0.001, 0.0);
        float cx = Drops(uv + e.xy, t, staticDrops, layer1, layer2, uvToPx).x;
        float cy = Drops(uv + e.yx, t, staticDrops, layer1, layer2, uvToPx).x;
        vec2 normal = vec2(cx - c.x, cy - c.x);
        nrm = normal / max(length(normal), 1e-5);
    }

    // --- Water shading ---------------------------------------------------
    // Water has no pigment. A drop is legible only through what it does to the
    // light behind it, so its body stays mostly transparent and the two things
    // that actually read as water are drawn on top:
    //
    //   rim  - a crescent at the drop's edge, derived from the drop MASK
    //          (c.x * (1 - c.x) * 4), not from the height-field gradient. The
    //          gradient saturates to 1 across the whole drop at any usable
    //          gain, which is what made older passes fill drops solid white.
    //          The mask-based form stays a thin outline at any drop size.
    //          Because BandFade attenuates the MASK rather than the finished
    //          shading, the rim re-forms around a fading drop's shrinking
    //          silhouette instead of leaving a flat, rim-less cut face.
    //   spec - one tight hotspot per drop, exponent 16 so it's a dot, not a
    //          broad sheen.
    float rim = pow(clamp(c.x * (1.0 - c.x) * 4.0, 0.0, 1.0), 1.5);
    vec2 lightDir = normalize(vec2(-0.4, 0.8));
    // Weight the rim toward the lit side. An evenly bright ring reads as a
    // soap bubble; a real drop catches the light as a crescent.
    float facing = dot(nrm, lightDir) * 0.5 + 0.5;
    rim *= 0.35 + 0.65 * facing;
    float spec = pow(max(0.0, dot(nrm, lightDir)), 16.0) * c.x;

    float bright = clamp(rim * 0.5 + spec * 0.6 + c.y * 0.25, 0.0, 1.0);

    // Composite premultiplied: a faint tinted body suggests refraction the
    // drop doesn't actually do, then self-lit highlights add on top.
    vec3 bodyColor = uTint.rgb;
    float bodyAlpha = c.x * 0.10;

    vec3 premul = bodyColor * bodyAlpha + vec3(1.0) * bright;
    float dropAlpha = clamp(bodyAlpha + bright, 0.0, 1.0);

    // uTint.a scales the whole drop's visibility. Because we output
    // premultiplied, both the RGB and the alpha have to be scaled by it
    // uniformly - scaling alpha alone would darken drops without fading them.
    // bandMask stays as a guaranteed-zero guard at the boundary; the drops
    // themselves have already faded by the time it bites.
    float alpha = bandMask * dropAlpha * uTint.a;
    if (alpha <= 0.0)
    {
        discard;
    }

    fragColor = vec4(premul * bandMask * uTint.a, alpha);
}

)shadersrc";

                static const char *const LENS_FLARE_VERT_SRC = R"shadersrc(#version 330 core
precision highp float;

// Trivial passthrough for a fullscreen NDC quad. The fragment shader derives
// pixel coordinates from gl_FragCoord (y-up) - not from a varying - so no
// interpolation precision is required here on mobile GLES.
in vec2 aPos;

void main()
{
    gl_Position = vec4(aPos, 0.0, 1.0);
}

)shadersrc";

                static const char *const LENS_FLARE_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_LENS_FLARE_TUNING_H_
#define _EDGE_LIGHTING_LENS_FLARE_TUNING_H_

// ---------------------------------------------------------------------------
// Shared ghost-term constants - single source of truth.
//
// Consumed by BOTH:
//   - the flare shader (lens-flare.frag), where CMake text-injects this file
//     via @LENS_FLARE_TUNING@ in shaders.h.in, and
//   - the C++ renderer (lens-flare-renderer.cpp), which #includes it to derive
//     the two support bounds it uploads as uniforms.
//
// Only the constants both sides must agree on live here. Everything else that
// shapes a ghost - the 50.0 / 3.0 / 0.05 term magnitudes, the 5.0 hex scale,
// the procedural palette - stays in lens-flare.frag: the renderer never
// reasons about those, and a constant with one consumer does not need two
// homes. Same split as droplets-tuning.h.
//
// WHY THE RENDERER NEEDS THEM AT ALL
//   circle() evaluates three terms per ghost per fragment, and two of them are
//   compactly supported - provably zero outside a radius. The shader skips
//   each where it cannot contribute, but the bounds are pure functions of
//   uGhostSize, so they are solved once on the CPU instead of in every one of
//   the viewport's fragments:
//
//   BLOOM   c > 0  <=>  pow(lq, size * FLARE_BLOOM_EXP) < FLARE_BLOOM_CUT
//                  <=>  lq < FLARE_BLOOM_CUT^(1 / (size * FLARE_BLOOM_EXP))
//           which is uBloomRadius. Exact, not conservative: pow is strictly
//           increasing in lq for a positive exponent, so the implication runs
//           both ways.
//
//   RING    c1 > 0 <=>  sin(l * 30) > pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP)
//                                     - FLARE_RING_BIAS
//           l = length(...) + size * FLARE_RING_L_BIAS, so l is never below
//           size * FLARE_RING_L_BIAS, and pow is increasing, so the whole
//           right-hand side is never below
//             pow(size * FLARE_RING_L_BIAS - FLARE_RING_SHIFT, FLARE_RING_EXP)
//             - FLARE_RING_BIAS
//           which is uRingFloor. Testing the (much cheaper) sin against that
//           first errs only in the safe direction: it can run the term where
//           the term turns out to be zero, never skip one that is not.
//
//   Change a constant here without rebuilding the matching derivation in
//   lens-flare-renderer.cpp and the gate starts clipping ghost pixels that
//   should be lit - the flare quietly loses ring arcs or bloom edges, with
//   nothing in the log to say so.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a single definition that compiles
// as both GLSL and C++ has to be a plain #define. Same reasoning as
// neon-tuning.h.
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off

/// Bloom term: the constant it is subtracted from, and the per-ghost falloff
/// exponent's scale. `max(FLARE_BLOOM_CUT - pow(lq, size * FLARE_BLOOM_EXP), 0)`.
#define FLARE_BLOOM_CUT     0.01
#define FLARE_BLOOM_EXP     1.4

/// Ring term: the bias it is added to, the shift inside its pow, and that
/// pow's exponent.
/// `max(FLARE_RING_BIAS - pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP) + sin(l * 30), 0)`.
#define FLARE_RING_BIAS     0.001
#define FLARE_RING_SHIFT    0.3
#define FLARE_RING_EXP      (1.0 / 40.0)

/// The share of ghostSize that biases the ring term's radius:
/// `l = length(...) + size * FLARE_RING_L_BIAS`. This is what puts a floor
/// under l, and so under the pow above, which is the whole basis of the ring
/// gate - the renderer cannot derive uRingFloor without it.
#define FLARE_RING_L_BIAS   0.5

/// Number of ghost groups scattered along the sun axis. Sizes the shader's
/// GhostBlock array AND the renderer's GhostBlockData that fills it, so the
/// two must agree or the upload disagrees with the std140 layout the shader
/// was compiled against.
#define FLARE_GHOST_COUNT   10

// clang-format on

#endif // _EDGE_LIGHTING_LENS_FLARE_TUNING_H_

precision highp float;

// Sun + hexagonal-aperture lens flare. Ported from the Shadertoy
// https://www.shadertoy.com/view/Xlc3D2 - see the LICENSE NOTE below.
//
// LICENSE NOTE
//   Shadertoy shaders default to CC BY-NC-SA 3.0 when the author does not
//   grant a more permissive licence in the source. The referenced shader
//   contains no explicit licence, so this port inherits that restriction.
//   If this library needs to ship in a commercial product, treat this file as
//   a reference implementation and rewrite the elements you keep, OR obtain a
//   permissive re-license from the original author. The prior version in git
//   history (based on Shadertoy 4sX3Rs, explicit Unlicense) is safe to ship.
//
// Structure (matches the reference so the visual reads the same):
//   10x ghosts    - `circle()` called in a loop. Each iteration is a group of
//                   {big pow-based ghost, ring, hex sprite} at a pseudo-random
//                   distance / size along the sun axis. The distance and the
//                   group's colour are read from uGhosts, which the renderer
//                   bakes; the reference derived them here, per fragment.
//                   Hex shape comes from regShape(N=6).
//   sun rays+core - three layered terms on top of the ghosts, tinted by
//                   uSunColor so the sun's temperature is controllable.
//   post          - global exp() vignette away from the sun.
//
// Cleanups vs the reference: dead sun() function removed, dead per-iteration
// `circColor` params dropped (the function overrides them with a procedural
// palette anyway), noise-texture reads replaced with hash so no atlas needed,
// sky-gradient background removed (this renderer's single responsibility is
// the flare itself - use a dedicated background renderer for atmosphere).
//
// Three departures from the reference are performance, not looks. The
// reference's hex distance went through atan(); it is a max of three dot
// products instead. Its two compactly-supported ghost terms were evaluated at
// every fragment; they are now gated on the exact radius outside which each is
// zero. Both are value-preserving - see hexCoverage and circle below. And its
// per-ghost distance and palette, pure functions of the ghost index and the
// config, were re-derived in every fragment; they now arrive baked in uGhosts.
// That last one shifts ghost placement in the third decimal, for the reason
// the renderer's BakeGhostTable spells out. The ghost loop was 88% of this
// shader's cost, and hexCoverage alone 43%.

out vec4 fragColor;

uniform vec2  uResolution;   // Viewport in pixels; used for aspect + normalisation.
uniform vec2  uSunPos;       // Sun centre in gl_FragCoord pixels (y-up).
uniform vec4  uSunColor;     // Sun-core / rays tint. Ghosts stay procedural.
uniform float uIntensity;    // Master brightness multiplier.
uniform float uSpread;       // Ghost strength (0 = suppress ghosts, 1 = reference look).
uniform float uSize;         // Sun-core / rays size scale (1 = reference); ghosts unaffected.
uniform float uRotation;     // Ray-angle offset in radians; drives sun/ray spin.
uniform float uRayDensity;   // Angular density of the ray pattern (see the doc block on the ray recipe).
uniform float uGhostSpacing;  // Ghost placement stretch along the sun axis (1 = reference). Scales spread only, not colour/size.
uniform float uGhostSize;     // Uniform per-ghost size/falloff exponent, shared by every ghost so they all read the same size.
uniform vec2  uFlareCenter;   // Ghost convergence point in normalised screen coords (0..1, y-down). (0.5, 0.5) = screen centre.
uniform float uBloomRadius;   // Radius outside which a ghost's bloom term is exactly zero. Derived from uGhostSize on the CPU; see lens-flare-tuning.h.
uniform float uRingFloor;     // Lower bound sin(l * 30) must clear before a ghost's ring term can be non-zero. Same derivation.

// Per-ghost table as a std140 uniform block, baked by the renderer: xyz is a
// ghost's final colour, w its distance along the sun axis.
//
// Both were derived here, per fragment, from the ghost index and three
// uniforms - LensFlareConfig's ghostOffset, ghostColor and ghostTint, which is
// why those no longer appear above. They were the same ten values in every
// fragment of the viewport, so they are the renderer's work, not the shader's.
// Same split the neon emission pre-pass is built on, one tier cheaper: one
// 160-byte block, no texture and no pass.
//
// A BLOCK, not a bare `uniform vec4 uGhosts[N]`: every per-index array in this
// tree is packed into a std140 block, because the bare array form is not
// available on the restricted GL targets this library ships against. Same
// pattern as neon.frag's LoopSamplesBlock / SegmentBlock / ArcBlock.
layout(std140) uniform GhostBlock
{
    vec4 uGhosts[FLARE_GHOST_COUNT];
};

// Hex aperture sprite. Returns anti-aliased *inside* coverage: 1 within the
// hexagon, 0 outside, with the edge feathered over the pixel footprint of the
// polygon distance.
//
// The reference sliced the sprite out with `max(0.01 - regShape, 0)`, where
// regShape was a 0.5..0.51 smoothstep. That threshold lands at the flat bottom
// of the smoothstep, so the *visible* fill boundary was razor-thin (sub-pixel)
// no matter how wide the smoothstep band was - it aliased into stair-steps,
// and worse in the half-res optimized pass where a pixel spans ~2x the width.
// Building coverage straight from the polygon distance with an fwidth-sized
// edge keeps the boundary ~1px wide at whatever resolution the pass runs at,
// so the hex ghosts resolve cleanly at full and half res alike.
//
// WHY THERE IS NO atan HERE
//   The distance being measured is the hexagon's support function: p projected
//   onto whichever of the six edge normals it lies nearest. The reference found
//   that normal by angle - atan(p.x, p.y), snapped to the nearest multiple of
//   2 PI / N, then cos(snapped - a) * length(p). But for a convex regular
//   polygon the nearest normal is also the one with the LARGEST projection:
//   every other normal sits further round, so its cosine is smaller. That makes
//   the whole construction a plain max over the six dot products, with no
//   transcendental in it. Opposite normals differ only in sign, so three
//   abs()-ed dots cover all six.
//
//   Same value to within float noise (max deviation 5e-6 over the plane, which
//   is the reference's own 6.28319 truncation), at roughly a third of the cost.
//   atan alone was ~43% of this shader.
//
// HEX_AXIS_* are dir(k * 2 PI / 6 - 0.2) for k = 0, 1, 2, in the reference's
// (sin, cos) convention and carrying its +0.2 radian aperture roll.
const vec2 HEX_AXIS_0 = vec2(-0.198669331,  0.980066578);
const vec2 HEX_AXIS_1 = vec2( 0.749428406,  0.662085390);
const vec2 HEX_AXIS_2 = vec2( 0.948096722, -0.317982085);

float hexCoverage(vec2 p)
{
    float d = max(max(abs(dot(p, HEX_AXIS_0)), abs(dot(p, HEX_AXIS_1))),
                  abs(dot(p, HEX_AXIS_2)));
    float w = max(fwidth(d), 1e-4);
    return 1.0 - smoothstep(0.5 - w, 0.5 + w, d);
}

// One ghost group at parametric distance `dist` along the sun axis.
//
// The bloom and ring terms are compactly supported - each is exactly zero
// outside a radius that depends only on `size` - so each is gated on the bound
// the renderer solved for it. A ghost covers a few percent of the viewport but
// used to be evaluated in full at every fragment of it. lens-flare-tuning.h
// carries the derivation of both bounds and the constants they share with the
// terms below.
//
// The hex sprite is deliberately NOT gated the same way: hexCoverage calls
// fwidth, and a derivative taken in non-uniform control flow is undefined in
// GLSL. It stays at the top level of the function, where the whole quad
// evaluates it or none of it does.
//
// `color` arrives from uGhosts rather than being derived here; `dist` still
// shapes all three terms, so it is still a parameter.
vec3 circle(vec2 p, float size, float dist, vec2 sunUV, vec3 color)
{
    // Bloom. `pow` is strictly increasing, so lq < uBloomRadius is exactly the
    // condition for a non-zero term, not an approximation of it.
    float lq = length(p + sunUV * dist);
    float c  = 0.0;
    if (lq < uBloomRadius)
    {
        c = max(FLARE_BLOOM_CUT - pow(lq, size * FLARE_BLOOM_EXP), 0.0) * 50.0;
    }

    // Ring. The sin is the cheap half of the comparison and uRingFloor bounds
    // the expensive half from below, so test the sin first and only pay for the
    // pow on the thin annuli where the term can actually be non-zero.
    float l  = length(p + sunUV * (dist * 4.0)) + size * FLARE_RING_L_BIAS;
    float sl = sin(l * 30.0);
    float c1 = 0.0;
    if (sl > uRingFloor)
    {
        c1 = max(FLARE_RING_BIAS - pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP) + sl, 0.0) * 3.0;
    }

    // Flat-filled hex sprite (magnitude 0.05, matching the reference's
    // 0.01 * 5) with an anti-aliased edge from hexCoverage.
    float s  = hexCoverage(p * 5.0 + sunUV * dist * 5.0 + 0.9) * 0.05;

    // Brightness comes from the (c + c1 + s) shape terms and hue from the
    // baked table, so ghosts keep their falloff whatever the tint.
    return (c + c1 + s) * color - 0.01;
}

void main()
{
    vec2  uv     = gl_FragCoord.xy / uResolution - 0.5;
    vec2  sunUV  = uSunPos         / uResolution - 0.5;
    float aspect = uResolution.x / uResolution.y;
    uv.x    *= aspect;
    sunUV.x *= aspect;

    vec3 color = vec3(0.0);

    // Ghost convergence point ("optical centre"). Converts the normalised
    // screen coord (0..1, y-down) into this shader's aspect-corrected, y-up,
    // centre-origin space. (0.5, 0.5) maps to (0, 0) = the screen centre, which
    // is the reference the ghosts historically pivoted about.
    vec2 flareCentre = vec2((uFlareCenter.x - 0.5) * aspect,
                            0.5 - uFlareCenter.y);

    // --- Ghost groups scattered along the sun -> flare-centre axis.
    // Working relative to flareCentre re-anchors the whole cluster: ghostP puts
    // dist 0 at flareCentre, and ghostAxis is the flareCentre -> sun direction,
    // so the ghost line runs through flareCentre and the sun. uGhostSpacing
    // stretches the placement axis without touching per-ghost colour/size.
    //
    // uGhostSize replaces the reference's per-ghost random size so every ghost
    // reads the same size; only distance (placement) still varies, and that
    // distance now arrives baked in uGhosts[i].w. dist 0 is the flare centre
    // and dist ~ -1 is the sun, which is the axis LensFlareConfig::ghostOffset
    // slides the cluster along before the bake.
    //
    // Every term the loop produces is multiplied by uSpread, so at zero the
    // whole block contributes exactly nothing and is skipped outright - and
    // zero is a documented setting (LensFlareConfig::spread, "suppress
    // ghosts"), not a degenerate one. uSpread is a uniform, so this is uniform
    // control flow: every invocation in the draw takes the same side, which is
    // what keeps circle()'s fwidth call legal inside it, and what makes the
    // branch itself free.
    if (uSpread != 0.0)
    {
        vec2 ghostP    = uv    - flareCentre;
        vec2 ghostAxis = (sunUV - flareCentre) * uGhostSpacing;
        for (int i = 0; i < FLARE_GHOST_COUNT; i++)
        {
            color += circle(ghostP, uGhostSize, uGhosts[i].w, ghostAxis, uGhosts[i].xyz) * uSpread;
        }
    }

    // --- Sun rays + core; tint governed by uSunColor. Size scale divides the
    // distance so uSize > 1 grows the disc and uSize < 1 shrinks it. Ray extent
    // is NOT scaled by uSize: the `ray / 8.0` term below has no distance
    // falloff, so how far the rays reach is set by the global exp(1 - sunDist)
    // envelope, which is in unscaled units.
    //
    // Ray recipe for a "sunburst" look:
    //   primary  = pow(|sin(a * N/2)|, 8)  -> N very thin countable spikes.
    //   sub      = pow(|cos(a * N/2)|, 20) -> even thinner "sparkle" needles
    //              at the half-angles between primaries. Fainter (0.15 weight).
    //   lenMod   = per-ray hash-based length scale in [0.15, 1.0]. Slots are
    //              indexed off @c mod(a, 2 PI) so the hash is stable across
    //              rotation, and slot boundaries land at primary valleys
    //              (primary = 0) so hash discontinuities are invisible.
    //              Applied to both primary and sub, so short rays are short
    //              in ALL their layers - avoids "flower" uniformity.
    const float TWO_PI = 6.28318530717958647692;
    float a       = atan(uv.y - sunUV.y, uv.x - sunUV.x) + uRotation;
    float sunDist = length(uv - sunUV);
    float sDist   = sunDist / max(uSize, 1e-3);
    vec3  sunTint = uSunColor.rgb;

    float N        = float(uRayDensity);
    float aWrapped = mod(a, TWO_PI);
    float slot     = floor(aWrapped * N / TWO_PI);
    float rand     = fract(sin(slot * 43.7583) * 12345.67);
    float lenMod   = 0.15 + 0.85 * rand;
    float primary  = pow(abs(sin(a * N * 0.5)), 8.0)  * lenMod;
    float sub      = pow(abs(cos(a * N * 0.5)), 20.0) * lenMod * 0.15;
    float ray      = primary + sub;

    // Softening floors give each falloff an intrinsic scale, so its peak is
    // finite and size-invariant. Without them the terms are pure power laws -
    // scale free, so dividing the radius by uSize is algebraically identical to
    // multiplying the term by uSize^k, and `size` acted as a second, non-linear
    // brightness knob (uSize^5 on the core term). The floor only bites near
    // sDist = 0, so far-field values - and the reference look - are unchanged.
    // It also removes the divide-by-zero inf the reference had at the exact sun
    // centre, which is why the max(..., 0.0) guards are gone.
    const float CORE_SOFT = 0.35; // core term, k = 5
    const float HAZE_SOFT = 0.02; // haze term, k = 1/20
    const float GLOW_SOFT = 0.10; // glow term, k = 1/2

    float core = 0.1 / (pow(sDist * 5.0,  5.0)        + CORE_SOFT);
    float haze = 0.1 / (pow(sDist * 10.0, 1.0 / 20.0) + HAZE_SOFT);
    float glow = 0.1 / (pow(sDist * 4.0,  0.5)        + GLOW_SOFT);

    color += core * ray / 20.0 * sunTint;
    color += (haze + ray / 8.0) * sunTint;
    color += glow * 4.0 * vec3(0.2, 0.21, 0.3) * 4.0 * sunTint;

    // --- Global falloff around the sun (kept from reference).
    color *= exp(1.0 - sunDist) / 5.0;

    color = max(color, 0.0) * uIntensity;

    // Premultiplied "over": alpha driven by the brightest channel so bright
    // cores occlude and thin ghost wings blend additively.
    float alpha = clamp(max(max(color.r, color.g), color.b), 0.0, 1.0);
    fragColor = vec4(color, alpha);
}

)shadersrc";

                static const char *const SPOTLIGHT_VERT_SRC = R"shadersrc(#version 330 core
precision highp float;

// Spotlight strip vertex stage.
//
// Every lamp is drawn as a strip of quads hugging its own support, and its
// scalars ride ALONG WITH that geometry as vertex attributes rather than in a
// uniform block. Three consequences worth knowing:
//
//   - There is no per-index array anywhere in this shader, so the project's
//     no-bare-uniform-arrays rule is satisfied by construction. Nothing to
//     bind, no std140 layout to keep in step with a C++ struct.
//   - aP0 / aP1 / aColor / aClipWeight carry the SAME value on all six of
//     every quad in a lamp's strip, so they would interpolate to themselves
//     even without `flat`. The qualifier is here because skipping the
//     interpolation is free, not because correctness needs it.
//   - aLocal is the one attribute that genuinely varies: the renderer
//     pre-rotates each corner into the lamp's own frame, so the fragment stage
//     never touches a sin or a cos, and never reads gl_FragCoord.
//
// aPos is in APP coordinates - origin top-left, +y DOWN, the same space as
// Config::geometry.position. uMVP carries a y-flipped ortho that maps it
// straight to clip space (see SpotlightRenderer::Render).
//
// vApp forwards that app-space position to the fragment stage, where the clip
// area and the shadow caster are evaluated. It is a SECOND USE of aPos, not a second attribute, and
// it is what keeps the clip out of gl_FragCoord: the projection is an ortho
// (w is 1 everywhere), so this varying interpolates exactly and reads the same
// app pixel whatever buffer the strips are being rasterised into.
//
// That makes the clip's GEOMETRY scale-free; it does not make its EDGE so. The
// mask is still evaluated once per fragment, so a reduced-resolution buffer
// resolves the boundary at its own texel pitch and the blit smears it back.
// SpotlightRenderer pins resolutionScale to 1.0 whenever a clipped or
// shadow-casting lamp is enabled for that reason - see GetClampedSpotScale for the measurements, and
// the note about uniforms in SpotlightRenderer::Render.

layout(location = 0) in vec2 aPos;   ///< App px, top-left origin, +y down.
layout(location = 1) in vec2 aLocal; ///< (along, across) px in the lamp's frame.
layout(location = 2) in vec4 aP0;    ///< tanHalfBeam, throwLength, softK, intensity.
layout(location = 3) in vec4 aP1;    ///< apertureWidth, bloom, bloomRadius, bloomWindow.
layout(location = 4) in vec3 aColor; ///< Linear RGB, baked from colorTemp on the CPU.
// A WEIGHT, not a flag, and named for it: the fragment stage mixes with it
// rather than branching on it. "Weight" also keeps it clear of the OTHER clip
// in a vertex shader - gl_Position's clip space, which this has nothing to do
// with.
layout(location = 5) in float aClipWeight; ///< 1 where this lamp honours the clip area, else 0.
/// Exponent on the cone's spread term - SpotLight::spreadFalloff, clamped.
/// Its own attribute because aP0 and aP1 are full; see StripVertex.
layout(location = 6) in float aSpreadFalloff;

// THE SHADOW: one lamp's share of SpotlightConfig::shadowCaster, baked by
// DeriveLampShadow in spotlight-renderer.cpp and read by the THE SHADOW
// section of spotlight.frag. Constant across a strip, like everything above
// but aPos and aLocal.
/// The lamp's app position (xy), the caster-to-lamp wall-distance ratio t
/// (z), and the penumbra's half width on the caster's plane (w).
layout(location = 7) in vec4 aShadow;
/// A WEIGHT, like aClipWeight: ShadowCaster::opacity for a lamp that casts a
/// shadow, 0 for one that does not.
layout(location = 8) in float aShadowWeight;
/// The frame's rails over the recessed picture: t_in and the penumbra half
/// width at the rails' front.
layout(location = 9) in vec2 aRecess;

out vec2 vLocal;
out vec2 vApp;
flat out vec4 vP0;
flat out vec4 vP1;
flat out vec3 vColor;
flat out float vClipWeight;
flat out float vSpreadFalloff;
// THE SHADOW.
flat out vec4 vShadow;
flat out float vShadowWeight;
flat out vec2 vRecess;

uniform mat4 uMVP;

void main() {
    vLocal = aLocal;
    vApp = aPos;
    vP0 = aP0;
    vP1 = aP1;
    vColor = aColor;
    vClipWeight = aClipWeight;
    vSpreadFalloff = aSpreadFalloff;
    // THE SHADOW.
    vShadow = aShadow;
    vShadowWeight = aShadowWeight;
    vRecess = aRecess;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}

)shadersrc";

                static const char *const SPOTLIGHT_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_SPOTLIGHT_TUNING_H_
#define _EDGE_LIGHTING_SPOTLIGHT_TUNING_H_

// ---------------------------------------------------------------------------
// Shared spotlight constants - single source of truth.
//
// Consumed by BOTH:
//   - the spotlight shaders (spotlight.frag, and spotlight-blit.frag for
//     SPOT_DITHER_STEPS), where CMake text-injects this file via
//     @SPOTLIGHT_TUNING@ in shaders.h.in, and
//   - the C++ renderer (spotlight-renderer.cpp), which #includes it to solve
//     each lamp's support and size the strip it draws.
//
// WHICH SIDE READS WHAT
//   Unlike droplets-tuning.h and lens-flare-tuning.h, the two sides here do
//   not read the same set. The split is deliberate and worth knowing:
//
//     SPOT_NEAR_FADE, SPOT_BLOOM_WINDOW_INNER   shader only - they shape terms
//                                               the CPU only has to bound, and
//                                               it bounds them conservatively.
//     SPOT_SOLID_SEARCH_STEPS                   shader only - a solid caster
//                                               only removes light, like the
//                                               panel.
//     SPOT_SHADOW_FULL_REACH                    shader only - the darkening
//                                               only writes where its lamp's
//                                               light does, inside the strip.
//     SPOT_FILL_FULL_RATIO                      shader only - the fill only
//                                               ever restores light the lamp
//                                               had before its shadow, so it
//                                               moves nothing the CPU bounds.
//     SPOT_DITHER_STEPS                         shader only - it perturbs the
//                                               output by less than one
//                                               destination step, which is
//                                               below everything the CPU
//                                               bounds.
//     SPOT_BLOOM_WINDOW_OUTER                   CPU only - it fixes where the
//                                               bloom window ends, and that
//                                               number reaches the shader as a
//                                               per-vertex attribute rather
//                                               than as this macro.
//     SPOT_MIN_APERTURE / SPOT_MIN_THROW        BOTH - the only constants here
//                                               that each side applies itself,
//                                               which is exactly why they have
//                                               to be shared.
//     SPOT_SOFT_MIN / SPOT_SOFT_MAX             CPU only - the renderer maps
//                                               softness to the gaussian
//                                               exponent and ships the result
//                                               as an attribute.
//     SPOT_MAX_LAMPS, SPOT_STRIP_SEGMENTS,
//     SPOT_VISIBILITY_FLOOR                     CPU only - geometry sizing.
//     SPOT_LAMP_WALL_DISTANCE                   CPU only - folded with the
//                                               caster's distance into a
//                                               per-lamp ratio before the
//                                               shader sees it.
//
//   They all live here anyway because they describe ONE falloff, and a
//   maintainer changing the shape needs to see the whole shape in one place.
//
// THE INVARIANT THIS HEADER CANNOT ENFORCE
//   The renderer solves, in closed form, where the shader's own falloff drops
//   below one 8-bit step, and draws a strip that stops there. That solve
//   inverts the WHOLE expression spotlight.frag evaluates - not just these
//   constants. Change the falloff's SHAPE in the shader (swap the gaussian,
//   change the exponent on the 1/halfW spread term other than through
//   SpotLight::spreadFalloff, add a factor) without redoing the solve in
//   SolveConeAcross, and the strip starts clipping lit pixels: the cone gets a
//   straight edge where the geometry ends, with nothing in the log to say so.
//
//   The guard for that is not this file - it is the offscreen diff described
//   in docs/spotlight-renderer-plan.md, which renders the same rig through the
//   solved strip and through an oversized quad and requires the two to be
//   byte-identical.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a single definition that compiles
// as both GLSL and C++ has to be a plain #define. Same reasoning as
// neon-tuning.h.
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off

/// Aperture widths over which the beam fades in at the lamp itself.
/// `smoothstep(-apertureWidth, SPOT_NEAR_FADE * apertureWidth, along)` - what
/// stops the cone from painting backwards out of the lamp.
///
/// The CPU does not read this, but it depends on it TWICE and only one of the
/// two is obvious:
///
///   - It starts the strip at -max(2 * apertureWidth, bloomReach), which
///     covers this fade for any value at or below 2.0. Raise it past 2.0 and
///     that bound stops being conservative.
///   - The strip's FIRST CHORD cuts inside the solved support, by up to ~1.7
///     px at a long throw or a boosted tint, because SolveConeAcross reads
///     max(a, 0) and so puts a corner in the support at a = 0. Nothing is
///     clipped only because this fade leaves the solve 3x to 13x conservative
///     over exactly that span. Lowering this value spends that slack.
///
/// Both are verified by grid evaluation rather than by argument - see the long
/// note on the widening pass in buildStrips, and re-run it if this moves.
#define SPOT_NEAR_FADE            1.6

/// The aperture bloom's window, as the two edges of one smoothstep:
/// `1 - smoothstep(bloomWindow * SPOT_BLOOM_WINDOW_INNER, bloomWindow, d)`,
/// where `bloomWindow = bloomRadius * SPOT_BLOOM_WINDOW_OUTER` is the per-lamp
/// value the renderer computes and ships as a vertex attribute.
///
/// The bloom is inverse-square and so has NO natural end - without this window
/// its support is the whole framebuffer and there is no finite strip to draw.
/// Same problem, same fix, as GetGhostBloomRadius in the lens flare.
///
/// _INNER is shader only. _OUTER is renderer only: it multiplies this out and
/// ships the product, so the shader never sees the factor.
///
/// THREE WORDS, ONE EACH, because this cluster used to share them. The value
/// below was SPOT_BLOOM_SUPPORT while its product was `bloomWindow` in C++,
/// `bloomSupport` in three doc comments and `sup` in the shader - four names
/// for one float, with "support" naming both the factor and the product. Now:
///
///   window - the artificial cutoff, i.e. these two constants and the
///            per-lamp `bloomWindow` px value they produce.
///   reach  - where the bloom actually stops mattering, i.e. LampSolve's
///            `bloomReach` = min(bloomWindow, the inverse-square core's own
///            visibility limit). Pairs with SolveConeReach.
///   support - reserved for the STRIP's support (SupportAt), which is a
///            different thing: the union of cone and bloom that the geometry
///            has to cover.
#define SPOT_BLOOM_WINDOW_INNER   0.55
#define SPOT_BLOOM_WINDOW_OUTER   8.0

/// Floors under SpotLight::apertureWidth and SpotLight::throwLength.
///
/// Shared, because BOTH sides apply them and they have to agree about what a
/// degenerate lamp means: spotlight.frag clamps the two attributes it reads,
/// and DeriveLamp clamps the same two config fields before the solve inverts
/// that shader. They were a matched pair of bare `1.0` literals in the shader
/// against MIN_APERTURE / MIN_THROW in the .cpp - the one pair of shared
/// constants in this layer that had no shared name.
///
/// A throwLength of 0 would divide by zero in the throw term; an apertureWidth
/// of 0 would collapse halfW at the lamp and take `apertureWidth / halfW` to
/// 0/0.
#define SPOT_MIN_APERTURE         1.0
#define SPOT_MIN_THROW            1.0

/// Gaussian exponent across the beam at softness 0 and softness 1:
/// `exp(-(across / halfWidth)^2 * softK)`, softK lerped between these.
/// Renderer only - it lerps and ships softK per vertex.
///
/// Note there is no setting that produces a hard beam edge, by design: the
/// look this renderer targets has no visible cone boundary anywhere, so the
/// control is how broad the gaussian is, never whether it is one.
#define SPOT_SOFT_MAX             3.40
#define SPOT_SOFT_MIN             0.85

/// Ceiling on lamps drawn in one pass. Sizes the VBO allocation ONLY - it is
/// not an array bound in any shader, because per-lamp data rides on vertex
/// attributes rather than in a uniform block. Entries past it are ignored.
///
/// Raising it costs one recompile and a larger (still small) buffer:
/// SPOT_MAX_LAMPS * SPOT_STRIP_SEGMENTS * 6 * 96 bytes.
#define SPOT_MAX_LAMPS            8

/// Quads per lamp along the beam. The strip approximates a curved support with
/// this many straight chords; each sample is widened to cover its neighbours'
/// midpoints so a chord bulges outward rather than cutting inside.
///
/// More segments means a tighter fit and more vertices, and nothing else - the
/// drawn image is identical at any value, because everything the strip adds or
/// removes is a region where the shader writes zero.
///
/// LOWERING it is the direction that needs care. The widening does not by
/// itself bound the chord at the corner the support carries at a = 0, and what
/// covers the resulting cut is SPOT_NEAR_FADE's slack over a span that grows
/// as segments get longer: at 12 the deepest cut measured is ~1.7 px against
/// 3x to 13x of headroom. Fewer, longer chords eat into that margin from the
/// other side. See the widening note in buildStrips for the grid evaluation,
/// and re-run it before shipping a lower value.
#define SPOT_STRIP_SEGMENTS       12

/// Width of the ordered dither added before the framebuffer quantises the
/// layer's output, peak to peak, in destination steps - by spotlight.frag at
/// resolutionScale 1.0, and by spotlight-blit.frag below it (see
/// spotlight-dither.glsl for why never into the reduced buffer). 1.0 is therefore
/// a rectangular +/- HALF a step - the classic dither for an 8-bit target: it
/// decorrelates the rounding error from the signal without adding visible
/// noise, and its peak is exactly the rounding threshold, so it cannot round
/// a black fragment up to 1.
///
/// WHY THIS LAYER NEEDS IT AND THE OTHERS MOSTLY DO NOT. A spotlight's outer
/// falloff is the flattest gradient in this library: at default settings the
/// cone crosses one 8-bit step every 13 to 50 px, so RGBA8 turns it into a
/// handful of wide bands with long, almost perfectly straight edges - the
/// cone's iso-contours are near-straight rays, so the quantisation contours
/// are too. Straight edges are exactly what the eye finds, which is why the
/// banding reads as a hard-edged "strip" laid over whatever is behind it
/// rather than as a smooth pool of light. Neon and the flare quantise the
/// same way but their gradients are steep enough that a band is a pixel or
/// two wide.
///
/// Shader only: the strip solve does not read it. See SPOT_VISIBILITY_FLOOR
/// for what the dither costs that solve.
///
/// 0.0 disables it and restores the exact pre-dither output.
#define SPOT_DITHER_STEPS         1.0

/// Where the highlight shoulder in spotlight.frag starts bending, as a
/// fraction of full scale. Below this the shading is passed through untouched;
/// above it the whole RGB vector is scaled by a Reinhard shoulder on its
/// BRIGHTEST channel, so it approaches 1.0 without ever reaching it.
///
/// WHY A SHOULDER AND NOT A CLAMP. At the lamp itself the cone is ~1 and the
/// bloom is ~bloomStrength, so the core peaks near intensity * (1 + bloom) -
/// past full scale for any lamp much brighter than 0.7 at the default bloom.
/// An RGBA8 target clips each channel on its own, and independent clipping is a
/// HUE change, not a brightness one: a 2700 K lamp at intensity 3 reaches
/// (3.9, 2.4, 1.1) and is written as (255, 255, 245), so an amber lamp renders
/// a white core that grows with intensity.
///
/// Scaling the vector by its own peak fixes the hue but not the look: a hard
/// knee at 1.0 leaves a C1 discontinuity, and the core becomes a flat lozenge
/// with a visible edge - the same "straight contour in a shallow gradient"
/// artefact SPOT_DITHER_STEPS exists to remove, arriving by another route.
/// This shoulder is C1 continuous at the knee (its derivative there is exactly
/// 1) and asymptotic above it, so the core saturates in brightness, holds its
/// colour, and has no edge anywhere.
///
/// Shader only, and safe to be: the shoulder only ever scales a fragment DOWN,
/// and only above this fraction of full scale, which is three orders of
/// magnitude above the SPOT_VISIBILITY_FLOOR the strip bound is solved
/// against. SolveConeAcross therefore stays conservative without knowing this
/// exists.
///
/// WHY 0.30 AND NOT 0.75, which is where this started. The shoulder's first job
/// is the hue fix above. Its second - the reason the value is this low - is that
/// it decides how far SpotLight::intensity can be driven before the EMITTER
/// stops looking like an emitter.
///
/// Raising intensity multiplies the whole field, and the cone's near field is
/// already ~1 at the lamp, so what grows is not the peak (the shoulder holds
/// that) but the AREA sitting at the top of the curve: the lamp turns into a
/// long white streak down the beam. Measured at 1920x1080, throwLength 3000,
/// intensity 8, counting pixels at or above 240:
///
///   knee 0.75 -> 8577 px, streaking 221 px down the axis
///   knee 0.50 -> 1076 px,             61 px
///   knee 0.30 ->   29 px,             18 px
///   knee 0.15 ->    0 px,              0 px
///
/// while the far field does not move at all - 1200 px out reads 61 at every one
/// of those, because those values are below the knee and pass through
/// unchanged. Lowering it therefore buys headroom for intensity almost for
/// free.
///
/// What it costs is the pinpoint hotspot at LOW intensity: a default lamp's
/// core reads 211 at 0.75 and 156 at 0.30. That is the whole cost - 100 px out
/// and beyond is byte-identical between the two. The hotspot comes back by
/// raising intensity, which is now the point.
///
/// 0.15 flattens the emitter completely but starts eating the near beam (100 px
/// out moves for the first time), which is why the value stops here.
///
/// PER LAMP. The shoulder sees one lamp's light and cannot see the others, so
/// it bounds each lamp, not their sum: two crossing beams each held near 0.6
/// would still add past full scale. What stops that is the renderer's blend,
/// which SCREENS the lamps rather than adding them - see
/// SpotlightRenderer::Render, and V12a in docs/review-findings.md for why the
/// shoulder was not moved onto the sum instead.
///
/// 1.0 disables the shoulder and restores per-channel clipping of a lone lamp.
#define SPOT_HIGHLIGHT_KNEE       0.30

// ---------------------------------------------------------------------------
// The shadow caster's constants: the lamps' distance from the wall (CPU
// only), then three the shader alone reads. The rest of the
// shadow is in two places: the "THE SHADOW" section of spotlight.frag, and
// the "THE SHADOW CASTER" blocks of spotlight-renderer.{h,cpp}.
// ---------------------------------------------------------------------------

/// How far every shadow-casting lamp stands off the wall, in px - the length
/// ShadowCaster::wallDistance is measured against, `t = caster / this`.
///
/// One constant rather than a field per lamp, because only that ratio (and
/// frameDepth / this) reaches the picture: the shadow's scale, its penumbra,
/// the solid caster's grazing case and the rails' shadow are all functions
/// of the distances divided by one another, so scaling every one of them by
/// a factor changes no pixel. The caster's own distance is therefore the
/// whole control. What a fixed value gives up is lamps at DIFFERENT
/// distances in one rig - each still throws its shadow away from its own
/// position, but at the same scale.
///
/// 150 is the default the per-lamp field (SpotLight::wallDistance, removed
/// 2026-10-16) had, so a rig that never set it renders as before.
#define SPOT_LAMP_WALL_DISTANCE   150.0

/// The lamp light - peak channel, after the highlight shoulder, in [0, 1] of
/// full scale - at which ShadowCaster::darkness reaches its full value. The
/// darkening is `darkness * (1 - visibility) * smoothstep(0, this, peak)`:
/// no light, no shadow, and the shadow fades out with the beam rather than
/// ending where the lamp's strip does.
///
/// That fade is also what keeps the strip bound free: outside the strip the
/// lamp is under half an 8-bit step (0.002), where the smoothstep is
/// 3 * (0.002 / this)^2 - under 2e-4 at this value, so the darkening there
/// stays far under half a step of any backdrop. Do not lower it below ~0.02.
#define SPOT_SHADOW_FULL_REACH    0.25

/// Where ShadowCaster::fill finishes washing a shadow out, as a ratio: the
/// light the OTHER lamps put on a point in lamp A's shadow, over the light A
/// itself would have put there. The fill's coverage lift is
/// `fill * smoothstep(0, SPOT_FILL_FULL_RATIO, ratio)`, so at fill 1 the shadow
/// is completely gone once the others reach this fraction of A.
///
/// 0.5, not 1.0, which is where it started. At 1.0 the shadow only vanished
/// where the other lamps matched A, and that is not what "no shadow where the
/// second lamp shines" means to a viewer: on the two-lamp scene this was
/// tuned against, a point where lamp 2 gave ~45% of lamp 0's light kept more
/// than half its shadow at fill 1 (34 -> 48 of an unshadowed 77). At 0.5 that
/// point is filled (see docs/spotlight-renderer.md section 4.7). Lower still
/// and a soft beam's gaussian tail - which reaches a long way - washes out
/// shadows far outside where the beam reads as lit.
///
/// Shader only: the fill changes nothing the strip bound depends on, since it
/// never raises a lamp above its own unshadowed light.
#define SPOT_FILL_FULL_RATIO      0.5

/// Golden-section iterations of the SOLID caster's search, each one a single
/// rounded-box SDF evaluation (plus three outside the loop).
///
/// A solid caster shadows a wall point when the line to the lamp passes
/// through the block anywhere along its depth, so the shader needs the
/// minimum, along that line, of the caster's SDF over a penumbra window that
/// grows with the distance travelled. That ratio is QUASICONVEX in the line
/// parameter - the SDF of a convex shape is convex along any line, divided by
/// a positive linear function - so it has one minimum and a golden-section
/// search converges on it without false minima, in a fixed number of steps.
///
/// Each step shrinks the bracket to 0.618 of itself: 12 leave 0.3% of the
/// segment, a few px on a long shadow, and the minimum is flat there, so the
/// value read is far closer than that. Raise it if a long, soft solid shadow
/// shows steps along its edge; every step is one more SDF for every fragment
/// of every casting lamp.
///
/// Shader only.
#define SPOT_SOLID_SEARCH_STEPS   12

/// HALF an 8-bit step - the level below which a value quantises to zero,
/// and therefore the budget the strip bound is solved against.
///
/// Half, not whole, because GL rounds float-to-unorm to NEAREST: a
/// contribution of 0.9/255 still lands on 1, so cutting at a full step clips
/// pixels that were about to be lit. That is not a theoretical worry - cutting
/// at 1/255 differed from an unbounded reference by exactly 1 LSB across tens
/// of thousands of pixels in every scene of the offscreen diff.
///
/// The renderer divides this by the number of ENABLED lamps before solving, so
/// the whole rig's clipped remainder stays inside one half-step however many
/// lamps overlap: N lamps each under floor/N sum to under floor. A single-lamp
/// rig therefore pays nothing for the sharing, and an eight-lamp rig draws the
/// larger strips it actually needs.
///
/// SPOT_DITHER_STEPS softens what this bound means, and improves what it is
/// worth. A dithered fragment carries up to half a step of noise, so a pixel
/// just inside the strip whose signal this solve cut can round to 1 where an
/// undithered one could not. That does not make the boundary MORE visible: the
/// pixels that light up are a sparse scatter rather than a line, which is the
/// same reason the dither is here. The contour the boundary used to hide
/// behind - the last 1/255 band edge - was the visible artefact, and it is the
/// one the dither removes.
#define SPOT_VISIBILITY_FLOOR     (0.5 / 255.0)

// clang-format on

#endif // _EDGE_LIGHTING_SPOTLIGHT_TUNING_H_

precision highp float;

// The spotlight's output dither, shared by the two programs that write the
// layer to the caller's framebuffer: spotlight.frag at resolutionScale 1.0, and
// spotlight-blit.frag below it. Injected after @SPOTLIGHT_TUNING@ (it reads
// SPOT_DITHER_STEPS), so it declares its own precision - a #version 300 es
// fragment shader has no default float precision, and this lands ahead of the
// file's own precision line. Not a shader: no main().
//
// ONLY for a write to the caller's 8-bit target. Never to the reduced buffer:
// the blit reads that back bilinearly, which averages the noise into blotches
// one buffer texel wide, and an RGBA8 buffer has already rounded the dark
// tail into one-level plateaus before the blit sees it - the contours this
// dither exists to remove come back as blocky diamonds. The neon learned the
// same thing (R7, neon-grade.glsl); see SpotlightRenderer::Render.

/// Interleaved gradient noise, in [-0.5, 0.5). One fract, one dot: the whole
/// dither costs about as much as the bloom's sqrt.
///
/// Deliberately NOT the fract(sin(dot(...))) hash. That one feeds sin an
/// argument in the tens of thousands, and how much of it survives is a
/// precision question every GPU answers differently - on a part that rounds
/// it coarsely the "noise" degenerates into a second set of bands, which is
/// the artefact this function exists to remove.
///
/// @p p is in PIXELS of the target being written: one step of @p p must be one
/// destination pixel, or the noise is sampled at the wrong pitch and stops
/// being noise. spotlight.frag hands it vLocal (a rotation plus a translation
/// of the framebuffer, so the pattern travels with the beam and two
/// overlapping lamps dither independently); spotlight-blit.frag hands it
/// gl_FragCoord, since by then the lamps are one summed image.
float spotDither(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5;
}

/// The dither offset to add to a colour and its coverage alike, in output
/// units: +/- SPOT_DITHER_STEPS / 2 levels of an 8-bit target.
///
/// Strictly under half a step at SPOT_DITHER_STEPS 1.0 (`fract` returns
/// strictly under 1), so GL's round-to-nearest cannot take a fragment the
/// falloff left at ZERO up to 1 - the strip's dark corners and, on the scaled
/// path, the whole unlit viewport the blit covers stay black rather than
/// acquiring a speckle.
float spotDitherOffset(vec2 p) {
    return spotDither(p) * (SPOT_DITHER_STEPS / 255.0);
}

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
// lamp's frame - or, for the clip area and the shadow caster, in app space
// through vApp. That is why the y-flip in Render is free, why SpotlightConfig::resolutionScale needs no
// uniform to shape the light (uDither below only says whether THIS pass
// dithers), and why the sub-viewport caveat in BaseRenderer's doc comment does
// not apply here.
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
// THE SHADOW - a picture standing off the wall that blocks the lamps opted
// into it - is a coverage multiply in the same place as the clip. All of it -
// the model, its varyings and uniforms, its functions and the SPOT_FILL
// variant - lives in the THE SHADOW section below, between its two banners,
// and main() reaches it through one call, spotShadowCover.
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
in vec2 vApp;        ///< App px, top-left origin, +y down. Clip area and shadow.
flat in vec4 vP0;    ///< tanHalfBeam, throwLength, softK, intensity.
flat in vec4 vP1;    ///< apertureWidth, bloom, bloomRadius, bloomWindow.
flat in vec3 vColor; ///< Linear RGB.
flat in float vClipWeight; ///< 1 where this lamp honours the clip area, else 0.
flat in float vSpreadFalloff; ///< Exponent on the spread term, already clamped.

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

// ===========================================================================
// THE SHADOW - everything about SpotlightConfig::shadowCaster, from here to
// the closing banner: the model, the per-lamp varyings, the caster's
// uniforms, the functions and the SPOT_FILL and SPOT_SHADOW_DARKEN variants.
// main() calls into it once, beside the clip. The CPU half is the THE SHADOW
// CASTER blocks of spotlight-renderer.{h,cpp}; its two constants are in
// spotlight-tuning.h. docs/spotlight-shadow-plan.md and
// docs/spotlight-solid-frame-plan.md are the measured record.
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
// coverage. `cover` is the light pass's own (fill lift included under
// SPOT_FILL), and `lit` the lamp's unshadowed light - no light, no shadow,
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

/// What the shadow caster leaves of this lamp's light here, in [0, 1] - the
/// ONE entry point main() calls, and only for a lamp whose vShadowWeight is
/// above 0 (a branch on a flat value, so coherent per lamp: with the solid
/// search in it the mask is a dozen SDFs, and a lamp that casts nothing pays
/// none of them and stays byte-identical to the renderer before the caster).
/// The weight is ShadowCaster::opacity, so a translucent caster lets that much
/// through.
///
/// @p lit is the lamp's light BEFORE the shadow - after the shoulder and the
/// clip - which the fill variant needs as the light this lamp would have put
/// here unshadowed.
///
/// Under SPOT_FILL the coverage is then lifted by the other lamps' light - see
/// THE FILL VARIANT above. A lamp that casts nothing skips this whole function
/// there too, which is exact: with a coverage of 1 the lift is (1 - 1) times a
/// finite value, so the light would be multiplied by exactly 1.0.
float spotShadowCover(vec3 lit, float apertureWidth) {
    float cover = mix(1.0, spotShadowMask(vApp, vShadow, vRecess, apertureWidth), vShadowWeight);
#ifdef SPOT_FILL
    // THE FILL - see THE FILL VARIANT above.
    vec3 own = lit * cover;
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
    cover += (1.0 - cover) * uFillAmount * smoothstep(0.0, SPOT_FILL_FULL_RATIO, ratio);
#endif
    return cover;
}

#ifdef SPOT_SHADOW_DARKEN
/// ShadowCaster::darkness, clamped to [0, 1].
uniform float uShadowDarkness;
/// ShadowCaster::color, linear RGB clamped to [0, 1].
uniform vec3 uShadowColor;

/// The darkening variant's output for this fragment - see THE DARKENING
/// VARIANT above. @p lit is the lamp's light after the shoulder and the clip,
/// before the shadow; @p dither the output dither main() computes.
///
/// An exact 0 wherever the lamp casts nothing, the fragment is outside its
/// shadow or the lamp puts no light there, so the dither cannot speckle the
/// rest of the strip: under GL_ONE / GL_ONE_MINUS_SRC_ALPHA a 0 leaves the
/// destination exactly as it was.
vec4 spotShadowDarken(vec3 lit, float apertureWidth, float dither) {
    if (vShadowWeight <= 0.0) {
        return vec4(0.0);
    }
    float cover = spotShadowCover(lit, apertureWidth);
    float reach = smoothstep(0.0, SPOT_SHADOW_FULL_REACH, max(max(lit.r, lit.g), lit.b));
    float d = uShadowDarkness * (1.0 - cover) * reach;
    if (d <= 0.0) {
        return vec4(0.0);
    }
    // The same half-step dither as the light, on the coverage: the colour is
    // premultiplied by it, so the two move together.
    d = clamp(d + dither, 0.0, 1.0);
    return vec4(uShadowColor * d, d);
}
#endif

// ===========================================================================
// End of THE SHADOW.
// ===========================================================================

/// 1 where this pass writes the caller's framebuffer (resolutionScale 1.0) or
/// an RGBA8 reduced buffer, 0 where it writes the half-float reduced buffer -
/// whose dither spotlight-blit.frag adds instead, after its bilinear read. The
/// ONE uniform the two paths set differently; see SpotlightRenderer::Render
/// and spotlight-dither.glsl.
uniform float uDither;

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

    // THE SHADOW, in the same place - after the shoulder, before the coverage
    // alpha and the dither, for the clip's reasons. Behind a branch on the
    // flat weight rather than a lerp: see spotShadowCover. The darkening
    // variant writes the shadow instead of the light, and stops here.
#ifdef SPOT_SHADOW_DARKEN
    fragColor = spotShadowDarken(lit, apertureWidth, spotDitherOffset(vLocal) * uDither);
    return;
#else
    if (vShadowWeight > 0.0) {
        lit *= spotShadowCover(lit, apertureWidth);
    }
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
    //
    // vLocal, not vApp or gl_FragCoord: the lamp frame is a rotation plus a
    // translation of the framebuffer, so one fragment is one unit of it
    // whatever the lamp's angle, and the pattern is anchored to the lamp - it
    // travels WITH the beam under an animation instead of crawling across it,
    // and two overlapping beams dither independently rather than in lockstep.
    //
    // Scaled by uDither, which is 0 on the scaled path's half-float buffer:
    // one fragment there is 1 / scale destination pixels, and the blit's
    // bilinear read would average this noise into blotches that wide. The blit
    // dithers instead (spotlight-blit.frag).
    float dither = spotDitherOffset(vLocal) * uDither;

    fragColor = vec4(max(lit + dither, 0.0), max(cov + dither, 0.0));
}

)shadersrc";

                static const char *const SPOTLIGHT_BLIT_FRAG_SRC = R"shadersrc(#version 330 core
#ifndef _EDGE_LIGHTING_SPOTLIGHT_TUNING_H_
#define _EDGE_LIGHTING_SPOTLIGHT_TUNING_H_

// ---------------------------------------------------------------------------
// Shared spotlight constants - single source of truth.
//
// Consumed by BOTH:
//   - the spotlight shaders (spotlight.frag, and spotlight-blit.frag for
//     SPOT_DITHER_STEPS), where CMake text-injects this file via
//     @SPOTLIGHT_TUNING@ in shaders.h.in, and
//   - the C++ renderer (spotlight-renderer.cpp), which #includes it to solve
//     each lamp's support and size the strip it draws.
//
// WHICH SIDE READS WHAT
//   Unlike droplets-tuning.h and lens-flare-tuning.h, the two sides here do
//   not read the same set. The split is deliberate and worth knowing:
//
//     SPOT_NEAR_FADE, SPOT_BLOOM_WINDOW_INNER   shader only - they shape terms
//                                               the CPU only has to bound, and
//                                               it bounds them conservatively.
//     SPOT_SOLID_SEARCH_STEPS                   shader only - a solid caster
//                                               only removes light, like the
//                                               panel.
//     SPOT_SHADOW_FULL_REACH                    shader only - the darkening
//                                               only writes where its lamp's
//                                               light does, inside the strip.
//     SPOT_FILL_FULL_RATIO                      shader only - the fill only
//                                               ever restores light the lamp
//                                               had before its shadow, so it
//                                               moves nothing the CPU bounds.
//     SPOT_DITHER_STEPS                         shader only - it perturbs the
//                                               output by less than one
//                                               destination step, which is
//                                               below everything the CPU
//                                               bounds.
//     SPOT_BLOOM_WINDOW_OUTER                   CPU only - it fixes where the
//                                               bloom window ends, and that
//                                               number reaches the shader as a
//                                               per-vertex attribute rather
//                                               than as this macro.
//     SPOT_MIN_APERTURE / SPOT_MIN_THROW        BOTH - the only constants here
//                                               that each side applies itself,
//                                               which is exactly why they have
//                                               to be shared.
//     SPOT_SOFT_MIN / SPOT_SOFT_MAX             CPU only - the renderer maps
//                                               softness to the gaussian
//                                               exponent and ships the result
//                                               as an attribute.
//     SPOT_MAX_LAMPS, SPOT_STRIP_SEGMENTS,
//     SPOT_VISIBILITY_FLOOR                     CPU only - geometry sizing.
//     SPOT_LAMP_WALL_DISTANCE                   CPU only - folded with the
//                                               caster's distance into a
//                                               per-lamp ratio before the
//                                               shader sees it.
//
//   They all live here anyway because they describe ONE falloff, and a
//   maintainer changing the shape needs to see the whole shape in one place.
//
// THE INVARIANT THIS HEADER CANNOT ENFORCE
//   The renderer solves, in closed form, where the shader's own falloff drops
//   below one 8-bit step, and draws a strip that stops there. That solve
//   inverts the WHOLE expression spotlight.frag evaluates - not just these
//   constants. Change the falloff's SHAPE in the shader (swap the gaussian,
//   change the exponent on the 1/halfW spread term other than through
//   SpotLight::spreadFalloff, add a factor) without redoing the solve in
//   SolveConeAcross, and the strip starts clipping lit pixels: the cone gets a
//   straight edge where the geometry ends, with nothing in the log to say so.
//
//   The guard for that is not this file - it is the offscreen diff described
//   in docs/spotlight-renderer-plan.md, which renders the same rig through the
//   solved strip and through an oversized quad and requires the two to be
//   byte-identical.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a single definition that compiles
// as both GLSL and C++ has to be a plain #define. Same reasoning as
// neon-tuning.h.
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off

/// Aperture widths over which the beam fades in at the lamp itself.
/// `smoothstep(-apertureWidth, SPOT_NEAR_FADE * apertureWidth, along)` - what
/// stops the cone from painting backwards out of the lamp.
///
/// The CPU does not read this, but it depends on it TWICE and only one of the
/// two is obvious:
///
///   - It starts the strip at -max(2 * apertureWidth, bloomReach), which
///     covers this fade for any value at or below 2.0. Raise it past 2.0 and
///     that bound stops being conservative.
///   - The strip's FIRST CHORD cuts inside the solved support, by up to ~1.7
///     px at a long throw or a boosted tint, because SolveConeAcross reads
///     max(a, 0) and so puts a corner in the support at a = 0. Nothing is
///     clipped only because this fade leaves the solve 3x to 13x conservative
///     over exactly that span. Lowering this value spends that slack.
///
/// Both are verified by grid evaluation rather than by argument - see the long
/// note on the widening pass in buildStrips, and re-run it if this moves.
#define SPOT_NEAR_FADE            1.6

/// The aperture bloom's window, as the two edges of one smoothstep:
/// `1 - smoothstep(bloomWindow * SPOT_BLOOM_WINDOW_INNER, bloomWindow, d)`,
/// where `bloomWindow = bloomRadius * SPOT_BLOOM_WINDOW_OUTER` is the per-lamp
/// value the renderer computes and ships as a vertex attribute.
///
/// The bloom is inverse-square and so has NO natural end - without this window
/// its support is the whole framebuffer and there is no finite strip to draw.
/// Same problem, same fix, as GetGhostBloomRadius in the lens flare.
///
/// _INNER is shader only. _OUTER is renderer only: it multiplies this out and
/// ships the product, so the shader never sees the factor.
///
/// THREE WORDS, ONE EACH, because this cluster used to share them. The value
/// below was SPOT_BLOOM_SUPPORT while its product was `bloomWindow` in C++,
/// `bloomSupport` in three doc comments and `sup` in the shader - four names
/// for one float, with "support" naming both the factor and the product. Now:
///
///   window - the artificial cutoff, i.e. these two constants and the
///            per-lamp `bloomWindow` px value they produce.
///   reach  - where the bloom actually stops mattering, i.e. LampSolve's
///            `bloomReach` = min(bloomWindow, the inverse-square core's own
///            visibility limit). Pairs with SolveConeReach.
///   support - reserved for the STRIP's support (SupportAt), which is a
///            different thing: the union of cone and bloom that the geometry
///            has to cover.
#define SPOT_BLOOM_WINDOW_INNER   0.55
#define SPOT_BLOOM_WINDOW_OUTER   8.0

/// Floors under SpotLight::apertureWidth and SpotLight::throwLength.
///
/// Shared, because BOTH sides apply them and they have to agree about what a
/// degenerate lamp means: spotlight.frag clamps the two attributes it reads,
/// and DeriveLamp clamps the same two config fields before the solve inverts
/// that shader. They were a matched pair of bare `1.0` literals in the shader
/// against MIN_APERTURE / MIN_THROW in the .cpp - the one pair of shared
/// constants in this layer that had no shared name.
///
/// A throwLength of 0 would divide by zero in the throw term; an apertureWidth
/// of 0 would collapse halfW at the lamp and take `apertureWidth / halfW` to
/// 0/0.
#define SPOT_MIN_APERTURE         1.0
#define SPOT_MIN_THROW            1.0

/// Gaussian exponent across the beam at softness 0 and softness 1:
/// `exp(-(across / halfWidth)^2 * softK)`, softK lerped between these.
/// Renderer only - it lerps and ships softK per vertex.
///
/// Note there is no setting that produces a hard beam edge, by design: the
/// look this renderer targets has no visible cone boundary anywhere, so the
/// control is how broad the gaussian is, never whether it is one.
#define SPOT_SOFT_MAX             3.40
#define SPOT_SOFT_MIN             0.85

/// Ceiling on lamps drawn in one pass. Sizes the VBO allocation ONLY - it is
/// not an array bound in any shader, because per-lamp data rides on vertex
/// attributes rather than in a uniform block. Entries past it are ignored.
///
/// Raising it costs one recompile and a larger (still small) buffer:
/// SPOT_MAX_LAMPS * SPOT_STRIP_SEGMENTS * 6 * 96 bytes.
#define SPOT_MAX_LAMPS            8

/// Quads per lamp along the beam. The strip approximates a curved support with
/// this many straight chords; each sample is widened to cover its neighbours'
/// midpoints so a chord bulges outward rather than cutting inside.
///
/// More segments means a tighter fit and more vertices, and nothing else - the
/// drawn image is identical at any value, because everything the strip adds or
/// removes is a region where the shader writes zero.
///
/// LOWERING it is the direction that needs care. The widening does not by
/// itself bound the chord at the corner the support carries at a = 0, and what
/// covers the resulting cut is SPOT_NEAR_FADE's slack over a span that grows
/// as segments get longer: at 12 the deepest cut measured is ~1.7 px against
/// 3x to 13x of headroom. Fewer, longer chords eat into that margin from the
/// other side. See the widening note in buildStrips for the grid evaluation,
/// and re-run it before shipping a lower value.
#define SPOT_STRIP_SEGMENTS       12

/// Width of the ordered dither added before the framebuffer quantises the
/// layer's output, peak to peak, in destination steps - by spotlight.frag at
/// resolutionScale 1.0, and by spotlight-blit.frag below it (see
/// spotlight-dither.glsl for why never into the reduced buffer). 1.0 is therefore
/// a rectangular +/- HALF a step - the classic dither for an 8-bit target: it
/// decorrelates the rounding error from the signal without adding visible
/// noise, and its peak is exactly the rounding threshold, so it cannot round
/// a black fragment up to 1.
///
/// WHY THIS LAYER NEEDS IT AND THE OTHERS MOSTLY DO NOT. A spotlight's outer
/// falloff is the flattest gradient in this library: at default settings the
/// cone crosses one 8-bit step every 13 to 50 px, so RGBA8 turns it into a
/// handful of wide bands with long, almost perfectly straight edges - the
/// cone's iso-contours are near-straight rays, so the quantisation contours
/// are too. Straight edges are exactly what the eye finds, which is why the
/// banding reads as a hard-edged "strip" laid over whatever is behind it
/// rather than as a smooth pool of light. Neon and the flare quantise the
/// same way but their gradients are steep enough that a band is a pixel or
/// two wide.
///
/// Shader only: the strip solve does not read it. See SPOT_VISIBILITY_FLOOR
/// for what the dither costs that solve.
///
/// 0.0 disables it and restores the exact pre-dither output.
#define SPOT_DITHER_STEPS         1.0

/// Where the highlight shoulder in spotlight.frag starts bending, as a
/// fraction of full scale. Below this the shading is passed through untouched;
/// above it the whole RGB vector is scaled by a Reinhard shoulder on its
/// BRIGHTEST channel, so it approaches 1.0 without ever reaching it.
///
/// WHY A SHOULDER AND NOT A CLAMP. At the lamp itself the cone is ~1 and the
/// bloom is ~bloomStrength, so the core peaks near intensity * (1 + bloom) -
/// past full scale for any lamp much brighter than 0.7 at the default bloom.
/// An RGBA8 target clips each channel on its own, and independent clipping is a
/// HUE change, not a brightness one: a 2700 K lamp at intensity 3 reaches
/// (3.9, 2.4, 1.1) and is written as (255, 255, 245), so an amber lamp renders
/// a white core that grows with intensity.
///
/// Scaling the vector by its own peak fixes the hue but not the look: a hard
/// knee at 1.0 leaves a C1 discontinuity, and the core becomes a flat lozenge
/// with a visible edge - the same "straight contour in a shallow gradient"
/// artefact SPOT_DITHER_STEPS exists to remove, arriving by another route.
/// This shoulder is C1 continuous at the knee (its derivative there is exactly
/// 1) and asymptotic above it, so the core saturates in brightness, holds its
/// colour, and has no edge anywhere.
///
/// Shader only, and safe to be: the shoulder only ever scales a fragment DOWN,
/// and only above this fraction of full scale, which is three orders of
/// magnitude above the SPOT_VISIBILITY_FLOOR the strip bound is solved
/// against. SolveConeAcross therefore stays conservative without knowing this
/// exists.
///
/// WHY 0.30 AND NOT 0.75, which is where this started. The shoulder's first job
/// is the hue fix above. Its second - the reason the value is this low - is that
/// it decides how far SpotLight::intensity can be driven before the EMITTER
/// stops looking like an emitter.
///
/// Raising intensity multiplies the whole field, and the cone's near field is
/// already ~1 at the lamp, so what grows is not the peak (the shoulder holds
/// that) but the AREA sitting at the top of the curve: the lamp turns into a
/// long white streak down the beam. Measured at 1920x1080, throwLength 3000,
/// intensity 8, counting pixels at or above 240:
///
///   knee 0.75 -> 8577 px, streaking 221 px down the axis
///   knee 0.50 -> 1076 px,             61 px
///   knee 0.30 ->   29 px,             18 px
///   knee 0.15 ->    0 px,              0 px
///
/// while the far field does not move at all - 1200 px out reads 61 at every one
/// of those, because those values are below the knee and pass through
/// unchanged. Lowering it therefore buys headroom for intensity almost for
/// free.
///
/// What it costs is the pinpoint hotspot at LOW intensity: a default lamp's
/// core reads 211 at 0.75 and 156 at 0.30. That is the whole cost - 100 px out
/// and beyond is byte-identical between the two. The hotspot comes back by
/// raising intensity, which is now the point.
///
/// 0.15 flattens the emitter completely but starts eating the near beam (100 px
/// out moves for the first time), which is why the value stops here.
///
/// PER LAMP. The shoulder sees one lamp's light and cannot see the others, so
/// it bounds each lamp, not their sum: two crossing beams each held near 0.6
/// would still add past full scale. What stops that is the renderer's blend,
/// which SCREENS the lamps rather than adding them - see
/// SpotlightRenderer::Render, and V12a in docs/review-findings.md for why the
/// shoulder was not moved onto the sum instead.
///
/// 1.0 disables the shoulder and restores per-channel clipping of a lone lamp.
#define SPOT_HIGHLIGHT_KNEE       0.30

// ---------------------------------------------------------------------------
// The shadow caster's constants: the lamps' distance from the wall (CPU
// only), then three the shader alone reads. The rest of the
// shadow is in two places: the "THE SHADOW" section of spotlight.frag, and
// the "THE SHADOW CASTER" blocks of spotlight-renderer.{h,cpp}.
// ---------------------------------------------------------------------------

/// How far every shadow-casting lamp stands off the wall, in px - the length
/// ShadowCaster::wallDistance is measured against, `t = caster / this`.
///
/// One constant rather than a field per lamp, because only that ratio (and
/// frameDepth / this) reaches the picture: the shadow's scale, its penumbra,
/// the solid caster's grazing case and the rails' shadow are all functions
/// of the distances divided by one another, so scaling every one of them by
/// a factor changes no pixel. The caster's own distance is therefore the
/// whole control. What a fixed value gives up is lamps at DIFFERENT
/// distances in one rig - each still throws its shadow away from its own
/// position, but at the same scale.
///
/// 150 is the default the per-lamp field (SpotLight::wallDistance, removed
/// 2026-10-16) had, so a rig that never set it renders as before.
#define SPOT_LAMP_WALL_DISTANCE   150.0

/// The lamp light - peak channel, after the highlight shoulder, in [0, 1] of
/// full scale - at which ShadowCaster::darkness reaches its full value. The
/// darkening is `darkness * (1 - visibility) * smoothstep(0, this, peak)`:
/// no light, no shadow, and the shadow fades out with the beam rather than
/// ending where the lamp's strip does.
///
/// That fade is also what keeps the strip bound free: outside the strip the
/// lamp is under half an 8-bit step (0.002), where the smoothstep is
/// 3 * (0.002 / this)^2 - under 2e-4 at this value, so the darkening there
/// stays far under half a step of any backdrop. Do not lower it below ~0.02.
#define SPOT_SHADOW_FULL_REACH    0.25

/// Where ShadowCaster::fill finishes washing a shadow out, as a ratio: the
/// light the OTHER lamps put on a point in lamp A's shadow, over the light A
/// itself would have put there. The fill's coverage lift is
/// `fill * smoothstep(0, SPOT_FILL_FULL_RATIO, ratio)`, so at fill 1 the shadow
/// is completely gone once the others reach this fraction of A.
///
/// 0.5, not 1.0, which is where it started. At 1.0 the shadow only vanished
/// where the other lamps matched A, and that is not what "no shadow where the
/// second lamp shines" means to a viewer: on the two-lamp scene this was
/// tuned against, a point where lamp 2 gave ~45% of lamp 0's light kept more
/// than half its shadow at fill 1 (34 -> 48 of an unshadowed 77). At 0.5 that
/// point is filled (see docs/spotlight-renderer.md section 4.7). Lower still
/// and a soft beam's gaussian tail - which reaches a long way - washes out
/// shadows far outside where the beam reads as lit.
///
/// Shader only: the fill changes nothing the strip bound depends on, since it
/// never raises a lamp above its own unshadowed light.
#define SPOT_FILL_FULL_RATIO      0.5

/// Golden-section iterations of the SOLID caster's search, each one a single
/// rounded-box SDF evaluation (plus three outside the loop).
///
/// A solid caster shadows a wall point when the line to the lamp passes
/// through the block anywhere along its depth, so the shader needs the
/// minimum, along that line, of the caster's SDF over a penumbra window that
/// grows with the distance travelled. That ratio is QUASICONVEX in the line
/// parameter - the SDF of a convex shape is convex along any line, divided by
/// a positive linear function - so it has one minimum and a golden-section
/// search converges on it without false minima, in a fixed number of steps.
///
/// Each step shrinks the bracket to 0.618 of itself: 12 leave 0.3% of the
/// segment, a few px on a long shadow, and the minimum is flat there, so the
/// value read is far closer than that. Raise it if a long, soft solid shadow
/// shows steps along its edge; every step is one more SDF for every fragment
/// of every casting lamp.
///
/// Shader only.
#define SPOT_SOLID_SEARCH_STEPS   12

/// HALF an 8-bit step - the level below which a value quantises to zero,
/// and therefore the budget the strip bound is solved against.
///
/// Half, not whole, because GL rounds float-to-unorm to NEAREST: a
/// contribution of 0.9/255 still lands on 1, so cutting at a full step clips
/// pixels that were about to be lit. That is not a theoretical worry - cutting
/// at 1/255 differed from an unbounded reference by exactly 1 LSB across tens
/// of thousands of pixels in every scene of the offscreen diff.
///
/// The renderer divides this by the number of ENABLED lamps before solving, so
/// the whole rig's clipped remainder stays inside one half-step however many
/// lamps overlap: N lamps each under floor/N sum to under floor. A single-lamp
/// rig therefore pays nothing for the sharing, and an eight-lamp rig draws the
/// larger strips it actually needs.
///
/// SPOT_DITHER_STEPS softens what this bound means, and improves what it is
/// worth. A dithered fragment carries up to half a step of noise, so a pixel
/// just inside the strip whose signal this solve cut can round to 1 where an
/// undithered one could not. That does not make the boundary MORE visible: the
/// pixels that light up are a sparse scatter rather than a line, which is the
/// same reason the dither is here. The contour the boundary used to hide
/// behind - the last 1/255 band edge - was the visible artefact, and it is the
/// one the dither removes.
#define SPOT_VISIBILITY_FLOOR     (0.5 / 255.0)

// clang-format on

#endif // _EDGE_LIGHTING_SPOTLIGHT_TUNING_H_

precision highp float;

// The spotlight's output dither, shared by the two programs that write the
// layer to the caller's framebuffer: spotlight.frag at resolutionScale 1.0, and
// spotlight-blit.frag below it. Injected after @SPOTLIGHT_TUNING@ (it reads
// SPOT_DITHER_STEPS), so it declares its own precision - a #version 300 es
// fragment shader has no default float precision, and this lands ahead of the
// file's own precision line. Not a shader: no main().
//
// ONLY for a write to the caller's 8-bit target. Never to the reduced buffer:
// the blit reads that back bilinearly, which averages the noise into blotches
// one buffer texel wide, and an RGBA8 buffer has already rounded the dark
// tail into one-level plateaus before the blit sees it - the contours this
// dither exists to remove come back as blocky diamonds. The neon learned the
// same thing (R7, neon-grade.glsl); see SpotlightRenderer::Render.

/// Interleaved gradient noise, in [-0.5, 0.5). One fract, one dot: the whole
/// dither costs about as much as the bloom's sqrt.
///
/// Deliberately NOT the fract(sin(dot(...))) hash. That one feeds sin an
/// argument in the tens of thousands, and how much of it survives is a
/// precision question every GPU answers differently - on a part that rounds
/// it coarsely the "noise" degenerates into a second set of bands, which is
/// the artefact this function exists to remove.
///
/// @p p is in PIXELS of the target being written: one step of @p p must be one
/// destination pixel, or the noise is sampled at the wrong pitch and stops
/// being noise. spotlight.frag hands it vLocal (a rotation plus a translation
/// of the framebuffer, so the pattern travels with the beam and two
/// overlapping lamps dither independently); spotlight-blit.frag hands it
/// gl_FragCoord, since by then the lamps are one summed image.
float spotDither(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5;
}

/// The dither offset to add to a colour and its coverage alike, in output
/// units: +/- SPOT_DITHER_STEPS / 2 levels of an 8-bit target.
///
/// Strictly under half a step at SPOT_DITHER_STEPS 1.0 (`fract` returns
/// strictly under 1), so GL's round-to-nearest cannot take a fragment the
/// falloff left at ZERO up to 1 - the strip's dark corners and, on the scaled
/// path, the whole unlit viewport the blit covers stay black rather than
/// acquiring a speckle.
float spotDitherOffset(vec2 p) {
    return spotDither(p) * (SPOT_DITHER_STEPS / 255.0);
}

precision highp float;

// SpotlightRenderer's composite of its reduced-resolution buffer back over the
// caller's framebuffer, below resolutionScale 1.0. Drawn with neon.vert over an
// NDC quad at an identity uMVP, so vPos is NDC. The renderer's screen blend
// (`dst + src * (1 - dst)`, colour and alpha) is still in force.
//
// Its own program rather than blit.frag, because it does one thing more: it is
// where the scaled path DITHERS. spotlight.frag dithers its own output at 1.0,
// but on this path it writes a buffer the bilinear read below filters, and a
// filtered read averages a per-texel dither into blotches - while an RGBA8
// buffer has already rounded the dark tail into one-level plateaus that no
// later noise can recover. So the buffer is half float and undithered, and the
// noise goes on here, once per destination pixel, after the filter: the same
// split the neon makes between mScaledBuffer and neon-blit.frag (R7). Measured
// in SpotlightRenderer.cpp beside SCALED_FORMATS.
//
// The same offset on colour and coverage, as spotlight.frag adds it - the
// layer's alpha screens exactly as its colour does, so the two take the same
// noise and the premultiplication is unchanged.
//
// On a driver that will not render to half float the buffer falls back to
// RGBA8 and spotlight.frag dithers into it as well (uDither 1); this pass
// still dithers on top, which joins the plateaus' rounding to the target's.
//
// TWO THINGS HERE ARE FOR SPEED, and both were measured, because this pass
// covers the whole viewport and is the fixed cost that decides whether the
// scale pays at all (AMD Radeon Pro 5300M, 1920 x 1080, one lamp at 0.25,
// against the undithered blit):
//
//   - the pixel position comes from vPos and uViewportSize, NOT gl_FragCoord.
//     Reading gl_FragCoord in this pass alone cost +0.05 ms (1.29x the frame);
//     the same noise from the interpolated position costs 1.06x. Same pixel
//     centres to well under the noise's own precision.
//   - an EMPTY texel writes an exact 0, not max(0 + noise, 0). The result is
//     the same - under half a level screened onto anything rounds back to it -
//     but a zero source is a write the blend can skip, and most of the
//     viewport is empty.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

/// The caller's viewport in pixels - what the blit's NDC quad covers - so
/// `uv * uViewportSize` is the destination pixel the dither is keyed on.
uniform vec2 uViewportSize;

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    vec4 c = texture(uSource, uv);
    float dither = spotDitherOffset(uv * uViewportSize);
    fragColor = (c == vec4(0.0)) ? vec4(0.0) : max(c + dither, 0.0);
}

)shadersrc";

                static const char *const PARTICLE_VERT_SRC = R"shadersrc(#version 330 core
precision highp float;

// One mote from the ring, written ONCE at spawn (ParticleManager). Its whole
// life is a closed form of its age, so nothing about it is re-uploaded per
// frame.
//
// Adding a particle animation: edit the hook functions below (motePosition,
// moteSize, moteColor) and moteAlpha in particle.frag. Each stays a pure
// function of the mote's record and its age - see "Adding a particle
// animation" in particle-renderer.h for per-mote data, per-arc uniforms and
// per-mote randomness. main() is not a hook: it culls the dead and reads the
// colour latch, and an animation must leave both alone.
layout(location = 0) in vec2  aPos0;      // position at birth, rect-local px (y up)
layout(location = 1) in vec2  aVel0;      // velocity at birth, px/s
layout(location = 2) in float aSpawnTime; // sim time of birth
layout(location = 3) in float aLifetime;  // seconds
layout(location = 4) in float aSize;      // point size at birth, px
layout(location = 5) in float aDrag;      // exponential drag, 1/s
layout(location = 6) in float aSlot;      // this mote's ring slot = its latch texel, an exact integer

uniform mat4  uMVP;
uniform float uSimTime;

// One texel per ring slot - premultiplied colour + coverage - written once
// when the mote spawned (see particle-latch.frag). Read by SLOT rather than by
// position, so the colour is the one the neon showed at the SPAWN point and
// moment, not wherever the mote is now. The slot comes from the record (aSlot),
// not gl_VertexID: the ring is drawn from mid-ring offsets, and an attribute
// is fetched at the right index however a driver numbers the vertices.
uniform sampler2D uColorLatch;
uniform int       uLatchWidth;

out vec4  vColor;
out float vLife;

// ---- Animation hooks --------------------------------------------------------

// Distance travelled by `age` per unit of launch velocity under exponential
// drag: the closed form of v' = -drag * v, (1 - e^(-drag * age)) / drag, and
// age itself as drag -> 0.
float dragTravel(float age) {
    return aDrag > 1e-4 ? (1.0 - exp(-aDrag * age)) / aDrag : age;
}

// Where the mote is at `age`, rect-local px (y up).
vec2 motePosition(float age) {
    return aPos0 + aVel0 * dragTravel(age);
}

// Point size with `life` left (1 at birth, 0 at death), px: shrinks toward
// zero as the mote ages so it does not pop out of view, floored at 1 px.
float moteSize(float life) {
    return max(aSize * life, 1.0);
}

// The colour to draw, from the one latched at spawn (premultiplied colour +
// coverage). Unchanged today; a colour that changes with age starts here.
vec4 moteColor(vec4 latched, float life) {
    return latched;
}

// -----------------------------------------------------------------------------

void main() {
    float age  = uSimTime - aSpawnTime;
    float life = 1.0 - age / max(aLifetime, 1e-6); // fraction of life remaining
    if (age < 0.0 || life <= 0.0) {
        // Dead, or a slot the window still spans: park it past the far plane
        // so it is clipped before rasterisation.
        gl_Position  = vec4(0.0, 0.0, 2.0, 1.0);
        gl_PointSize = 1.0;
        vColor       = vec4(0.0);
        vLife        = 0.0;
        return;
    }

    gl_Position  = uMVP * vec4(motePosition(age), 0.0, 1.0);
    gl_PointSize = moteSize(life);
    int  slot    = int(aSlot + 0.5);
    vec4 latched = texelFetch(uColorLatch, ivec2(slot % uLatchWidth, slot / uLatchWidth), 0);
    vColor       = moteColor(latched, life);
    vLife        = life;
}

)shadersrc";

                static const char *const PARTICLE_FRAG_SRC = R"shadersrc(#version 330 core
precision highp float;

in vec4  vColor; // premultiplied colour + coverage, latched at spawn
in float vLife;  // fraction of life remaining: 1 at birth, 0 at death

out vec4 fragColor;

// ---- Animation hook ---------------------------------------------------------

// How opaque the sprite is at `pointCoord` (gl_PointCoord, [0, 1] across the
// point) with `life` left. A twinkle, another sprite shape or another fade
// over life goes here - see "Adding a particle animation" in
// particle-renderer.h.
float moteAlpha(vec2 pointCoord, float life) {
    // Recentre to [-0.5, 0.5] and use radial distance to shape a soft round
    // mote: full inside 60% of the radius, smoothly to 0 at the rim.
    vec2  uv    = pointCoord - vec2(0.5);
    float d     = length(uv) * 2.0; // 0 at centre, 1 at edge
    float shape = 1.0 - smoothstep(0.6, 1.0, d);

    // Fades out with the life that remains, and faster over its last 15%, so
    // a mote thins away rather than blinking out at its end.
    float endFade = smoothstep(0.0, 0.15, life);
    return shape * life * endFade;
}

// -----------------------------------------------------------------------------

void main() {
    float alpha = moteAlpha(gl_PointCoord, vLife);

    // The latched colour is already premultiplied by its own coverage - the
    // neon pixel's alpha when the particle copied the drawn glow, 1 for the
    // CPU ring, the arc's own alpha for a custom colour - so the particle's
    // shape and fade scale both halves alike. Where the neon drew nothing,
    // coverage 0 makes the particle invisible rather than a dark dot.
    float coverage = alpha * vColor.a;
    if (coverage <= 0.0) { discard; }

    // Premultiplied so the renderer can composite with GL_ONE / one-minus-src-alpha
    // just like the neon shader does.
    fragColor = vec4(vColor.rgb * alpha, coverage);
}

)shadersrc";

                static const char *const PARTICLE_LATCH_VERT_SRC = R"shadersrc(#version 330 core
precision highp float;

// Colour latch: one 1 px point per NEWLY SPAWNED particle, placed on that
// particle's own texel of the latch texture. The texel is written here once,
// at spawn, and read by particle.vert for the rest of the particle's life -
// which is what keeps a particle's colour fixed while the neon it was sampled
// from keeps moving.

layout(location = 0) in float aSlot;     // latch slot, an exact integer
layout(location = 1) in vec2  aSpawnPos; // spawn point on the line, rect-local px (y up)
layout(location = 2) in vec4  aColor;    // CPU colour, straight rgba: the ring's, or the arc's custom colour
layout(location = 3) in float aFromNeon; // 1: copy the neon's drawn glow instead of aColor

uniform int uLatchWidth;
uniform int uLatchHeight;

flat out vec2  vSpawnPos;
flat out vec4  vColor;
flat out float vFromNeon;

void main() {
    int  slot  = int(aSlot + 0.5);
    vec2 texel = vec2(float(slot % uLatchWidth), float(slot / uLatchWidth)) + 0.5;
    gl_Position  = vec4(texel / vec2(float(uLatchWidth), float(uLatchHeight)) * 2.0 - 1.0, 0.0, 1.0);
    gl_PointSize = 1.0;
    vSpawnPos    = aSpawnPos;
    vColor       = aColor;
    vFromNeon    = aFromNeon;
}

)shadersrc";

                static const char *const PARTICLE_LATCH_FRAG_SRC = R"shadersrc(#version 330 core
precision highp float;

flat in vec2  vSpawnPos;
flat in vec4  vColor;
flat in float vFromNeon;

// What the latch holds is PREMULTIPLIED colour + coverage, so particle.frag
// can draw a particle as a copy of the neon pixel it was born on.
//
//   vFromNeon 1 - the neon's DRAWN glow (its scaled output buffer): the pixel
//                 under the spawn point, as stored - colour AND coverage, so
//                 a faint glow makes a faint particle and a strong one a
//                 strong particle. Where the neon drew nothing, coverage is 0
//                 and so is the particle. Only ever set while that buffer is
//                 bound.
//   vFromNeon 0 - vColor, from the CPU: the ring colour at alpha 1, or the
//                 arc's custom colour, whose alpha is its coverage.
uniform sampler2D uNeonOutput;
// The buffer covers a REGION of the frame, not the viewport: rect-local
// full-res px -> its uv, the map the neon's blit reads it through
// (NeonRenderer::GetOutputUVMap).
uniform vec2 uUVScale;
uniform vec2 uUVOffset;

out vec4 fragColor;

void main() {
    if (vFromNeon > 0.5) {
        // The same uv, and the same bilinear read, the blit samples at this
        // point. textureLod, not texture: a 1 px point has no meaningful
        // derivatives.
        vec2 uv   = vSpawnPos * uUVScale + uUVOffset;
        fragColor = textureLod(uNeonOutput, uv, 0.0);
    } else {
        fragColor = vec4(vColor.rgb * vColor.a, vColor.a);
    }
}

)shadersrc";

        }
}

#endif // _EDGE_LIGHTING_SHADERS_H_
