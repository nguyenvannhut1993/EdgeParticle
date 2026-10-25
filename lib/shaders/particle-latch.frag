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
