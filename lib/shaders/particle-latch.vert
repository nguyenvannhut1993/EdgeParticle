precision highp float;

// Colour latch: one 1 px point per NEWLY SPAWNED particle, placed on that
// particle's own texel of the latch texture. The texel is written here once,
// at spawn, and read by particle.vert for the rest of the particle's life -
// which is what keeps a particle's colour fixed while the neon it was sampled
// from keeps moving.

layout(location = 0) in float aSlot;     // latch slot, an exact integer
layout(location = 1) in vec2  aSpawnPos; // spawn point on the line, rect-local px (y up)
layout(location = 2) in vec3  aFallback; // CPU ring colour, used when there is no drawn glow

uniform int uLatchWidth;
uniform int uLatchHeight;

flat out vec2 vSpawnPos;
flat out vec3 vFallback;

void main() {
    int  slot  = int(aSlot + 0.5);
    vec2 texel = vec2(float(slot % uLatchWidth), float(slot / uLatchWidth)) + 0.5;
    gl_Position  = vec4(texel / vec2(float(uLatchWidth), float(uLatchHeight)) * 2.0 - 1.0, 0.0, 1.0);
    gl_PointSize = 1.0;
    vSpawnPos    = aSpawnPos;
    vFallback    = aFallback;
}
