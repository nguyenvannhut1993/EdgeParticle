#ifndef _EDGE_LIGHTING_SPOTLIGHT_SHADOW_TUNING_H_
#define _EDGE_LIGHTING_SPOTLIGHT_SHADOW_TUNING_H_

// ---------------------------------------------------------------------------
// The spotlight shadow caster's constants - single source of truth.
//
// Consumed by BOTH:
//   - the spotlight shaders (spotlight.vert, spotlight.frag), where CMake
//     text-injects this file via @SPOTLIGHT_SHADOW_TUNING@ in shaders.h.in,
//     ahead of the shadow's own chunks (spotlight-shadow-vert.glsl,
//     spotlight-shadow.glsl), and
//   - the C++ side (spotlight-shadow.cpp), which #includes it.
//
// Its own file rather than a block of spotlight-tuning.h for the reason the
// whole shadow is its own module: the light's files are edited by several
// people, and the shadow should not have to move with them. The rest of the
// shadow is spotlight-shadow.{h,cpp}, spotlight-shadow.glsl and
// spotlight-shadow-vert.glsl; docs/guides/spotlight-shadow.md is the
// reference.
//
// WHICH SIDE READS WHAT
//     SPOT_LAMP_WALL_DISTANCE                   CPU only - folded with the
//                                               caster's distance into a
//                                               per-lamp ratio before the
//                                               shader sees it.
//     SPOT_SHADOW_FULL_REACH                    shader only - the darkening
//                                               only writes where its lamp's
//                                               light does, inside the strip.
//     SPOT_FILL_FULL_RATIO                      shader only - the fill only
//                                               ever restores light the lamp
//                                               had before its shadow, so it
//                                               moves nothing the CPU bounds.
//     SPOT_SOLID_SEARCH_STEPS                   shader only - a solid caster
//                                               only removes light, like the
//                                               panel.
//     SPOT_SHADOW_ATTRIB_*                      BOTH - the vertex attribute
//                                               locations the CPU binds the
//                                               shadow's buffer to and
//                                               spotlight-shadow-vert.glsl
//                                               declares.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a single definition that compiles
// as both GLSL and C++ has to be a plain #define. Same reasoning as
// spotlight-tuning.h.
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off

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
/// point is filled (see docs/guides/spotlight-shadow.md). Lower still and a
/// soft beam's gaussian tail - which reaches a long way - washes out shadows
/// far outside where the beam reads as lit.
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

/// Vertex attribute locations of the shadow's per-lamp data - read from a
/// buffer of the shadow's own (SpotlightShadow::AttachAttributes), not from
/// SpotlightRenderer's StripVertex.
///
/// The TOP of the 16 locations GL 3.3 and GLES 3.0 both guarantee, on
/// purpose: the light's own attributes count up from 0 (0-6 today), so a new
/// one of them takes 7, 8, ... and never lands on these. A new light
/// attribute that would reach 13 has to move these first - a vertex input on
/// a location two declarations share is a link error on GLES and undefined on
/// desktop GL.
///
///   SHADOW  vec4   lamp app position (xy), t (z), penumbra half width (w)
///   WEIGHT  float  ShadowCaster::opacity for a casting lamp, else 0
///   RECESS  vec2   the rails' t_in (x) and penumbra half width (y)
#define SPOT_SHADOW_ATTRIB_SHADOW 13
#define SPOT_SHADOW_ATTRIB_WEIGHT 14
#define SPOT_SHADOW_ATTRIB_RECESS 15

// clang-format on

#endif // _EDGE_LIGHTING_SPOTLIGHT_SHADOW_TUNING_H_
