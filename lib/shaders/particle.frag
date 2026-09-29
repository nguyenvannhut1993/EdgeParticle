precision highp float;

in vec4  vColor; // premultiplied colour + coverage, latched at spawn
in float vLife;

out vec4 fragColor;

void main() {
    // gl_PointCoord in [0, 1] across the point sprite. Recentre to [-0.5, 0.5]
    // and use radial distance to shape a soft round mote.
    vec2  uv = gl_PointCoord - vec2(0.5);
    float d  = length(uv) * 2.0;         // 0 at centre, 1 at edge.

    // Soft edge: full brightness inside 60% radius, smoothly fades to 0 at rim.
    float shape = 1.0 - smoothstep(0.6, 1.0, d);

    // Life fade in on the last 30% of the particle's life so newborns pop in
    // softly rather than appearing at full size.
    float birthFade = smoothstep(0.0, 0.15, vLife);
    float alpha     = shape * vLife * birthFade;

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
