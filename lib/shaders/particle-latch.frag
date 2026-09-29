precision highp float;

flat in vec2  vSpawnPos;
flat in vec4  vColor;
flat in float vFromNeon;

// What the latch holds is PREMULTIPLIED colour + coverage, so particle.frag
// can draw a particle as a copy of the neon pixel it was born on.
//
//   vFromNeon 1 - the neon's DRAWN glow (its scaled output buffer): the pixel
//                 under the spawn point, exactly as the blit composites it.
//                 Where the neon drew nothing, coverage is 0 and so is the
//                 particle. Only ever set while that buffer is bound.
//   vFromNeon 0 - vColor, from the CPU: the ring colour at alpha 1, or the
//                 arc's custom colour, whose alpha is its coverage.
uniform sampler2D uNeonOutput;
uniform vec2 uRectCenter; // rect centre in window px (y up)
uniform vec2 uViewport;   // viewport size in px - the buffer spans it

out vec4 fragColor;

void main() {
    if (vFromNeon > 0.5) {
        // The same uv the blit samples at this window position. textureLod,
        // not texture: a 1 px point has no meaningful derivatives.
        vec2 uv   = (uRectCenter + vSpawnPos) / uViewport;
        fragColor = textureLod(uNeonOutput, uv, 0.0);
    } else {
        fragColor = vec4(vColor.rgb * vColor.a, vColor.a);
    }
}
