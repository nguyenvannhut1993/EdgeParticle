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
