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
