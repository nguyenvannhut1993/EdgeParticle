precision highp float;

// Per-particle vertex attributes.
layout(location = 0) in vec2  aPos;   // rect-local coords (rect-centered)
layout(location = 1) in float aSlot;  // colour latch slot, an exact integer
layout(location = 2) in float aLife;  // fractional life remaining in [0, 1]
layout(location = 3) in float aSize;  // spawn size in pixels

uniform mat4 uMVP;

// One texel per slot - premultiplied colour + coverage - written once when
// the particle spawned (see particle-latch.frag). Read by slot rather than by
// position, so the colour is the one the neon showed at the SPAWN point and
// moment, not wherever the particle has drifted to since.
uniform sampler2D uColorLatch;
uniform int       uLatchWidth;

out vec4  vColor;
out float vLife;

void main() {
    gl_Position  = uMVP * vec4(aPos, 0.0, 1.0);
    // Shrink toward zero as the particle ages so they don't pop out of view.
    gl_PointSize = max(aSize * aLife, 1.0);
    int slot     = int(aSlot + 0.5);
    vColor       = texelFetch(uColorLatch, ivec2(slot % uLatchWidth, slot / uLatchWidth), 0);
    vLife        = aLife;
}
