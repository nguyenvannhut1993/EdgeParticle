precision highp float;

flat in vec2 vSpawnPos;
flat in vec3 vFallback;

// What the latch holds is PREMULTIPLIED colour + coverage, so particle.frag
// can draw a particle as a copy of the neon pixel it was born on.
//
//   uFromNeon 1 - the neon's DRAWN glow (its scaled output buffer): the pixel
//                 under the spawn point, exactly as the blit composites it.
//                 Where the neon drew nothing, coverage is 0 and so is the
//                 particle.
//   uFromNeon 0 - vFallback, the CPU ring colour, at full coverage.
uniform sampler2D uNeonOutput;
uniform int  uFromNeon;
uniform vec2 uRectCenter; // rect centre in window px (y up)
uniform vec2 uViewport;   // viewport size in px - the buffer spans it

out vec4 fragColor;

void main() {
    if (uFromNeon != 0) {
        // The same uv the blit samples at this window position. textureLod,
        // not texture: a 1 px point has no meaningful derivatives.
        vec2 uv   = (uRectCenter + vSpawnPos) / uViewport;
        fragColor = textureLod(uNeonOutput, uv, 0.0);
    } else {
        fragColor = vec4(vFallback, 1.0);
    }
}
