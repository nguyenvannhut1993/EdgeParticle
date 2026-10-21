precision highp float;

// THE SHADOW's vertex half: one lamp's share of SpotlightConfig::shadowCaster,
// passed through to spotlight-shadow.glsl. Injected into spotlight.vert via
// @SPOTLIGHT_SHADOW_VERT@ in shaders.h.in, after @SPOTLIGHT_SHADOW_TUNING@
// (the locations below are its macros), and reached from spotlight.vert's
// main() through ONE call, spotShadowForward. Not a shader: no main().
//
// These attributes come from a buffer of the shadow's own
// (SpotlightShadow::AttachAttributes), bound into SpotlightRenderer's strip
// VAO beside the light's StripVertex buffer - so the light's vertex layout
// never carries them. Baked per lamp by DeriveLampShadow in
// spotlight-shadow.cpp, and constant across a lamp's strip like everything in
// spotlight.vert but aPos and aLocal. Declares its own precision, since it
// lands ahead of the file's own precision line.

/// The lamp's app position (xy), the caster-to-lamp wall-distance ratio t
/// (z), and the penumbra's half width on the caster's plane (w).
layout(location = SPOT_SHADOW_ATTRIB_SHADOW) in vec4 aShadow;
/// A WEIGHT, like aClipWeight: ShadowCaster::opacity for a lamp that casts a
/// shadow, 0 for one that does not.
layout(location = SPOT_SHADOW_ATTRIB_WEIGHT) in float aShadowWeight;
/// The frame's rails over the recessed picture: t_in and the penumbra half
/// width at the rails' front.
layout(location = SPOT_SHADOW_ATTRIB_RECESS) in vec2 aRecess;

flat out vec4 vShadow;
flat out float vShadowWeight;
flat out vec2 vRecess;

/// Forward the shadow's attributes to the fragment stage. The one call
/// spotlight.vert's main() makes into this chunk.
void spotShadowForward() {
    vShadow = aShadow;
    vShadowWeight = aShadowWeight;
    vRecess = aRecess;
}
