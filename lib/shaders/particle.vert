precision highp float;

// One mote from the ring, written ONCE at spawn (ParticleManager). Its whole
// life is a function of its age, so nothing about it is re-uploaded per frame.
layout(location = 0) in vec2  aPos0;      // position at birth, rect-local px (y up)
layout(location = 1) in vec2  aVel0;      // velocity at birth, px/s
layout(location = 2) in float aSpawnTime; // sim time of birth
layout(location = 3) in float aLifetime;  // seconds
layout(location = 4) in float aSize;      // point size at birth, px
layout(location = 5) in float aDrag;      // exponential drag, 1/s

uniform mat4  uMVP;
uniform float uSimTime;

// One texel per ring slot - premultiplied colour + coverage - written once
// when the mote spawned (see particle-latch.frag). gl_VertexID IS the slot:
// the ring is drawn with glDrawArrays, whose vertex id is the buffer index.
// Read by slot rather than by position, so the colour is the one the neon
// showed at the SPAWN point and moment, not wherever the mote is now.
uniform sampler2D uColorLatch;
uniform int       uLatchWidth;

out vec4  vColor;
out float vLife;

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

    // Closed form of v' = -drag * v from (aPos0, aVel0): the distance covered
    // by `age` is v0 * (1 - e^(-drag * age)) / drag, and v0 * age as drag -> 0.
    float travel = aDrag > 1e-4 ? (1.0 - exp(-aDrag * age)) / aDrag : age;
    gl_Position  = uMVP * vec4(aPos0 + aVel0 * travel, 0.0, 1.0);
    // Shrink toward zero as the mote ages so they don't pop out of view.
    gl_PointSize = max(aSize * life, 1.0);
    int slot     = gl_VertexID;
    vColor       = texelFetch(uColorLatch, ivec2(slot % uLatchWidth, slot / uLatchWidth), 0);
    vLife        = life;
}
