#version 450
// "Frosted glass" overlay: blurs a copy of the already-rendered scene (set 0 / binding 0) behind a rounded quad and
// lays the quad's colour (vColor, with its alpha as the tint strength) over it.
layout(set = 0, binding = 0) uniform sampler2D uScene;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vLocal;
layout(location = 3) flat in vec3 vShape;
layout(location = 0) out vec4 outColor;

void main() {
    float r = min(abs(vShape.z), min(vShape.x, vShape.y));
    vec2 q = abs(vLocal) - (vShape.xy - r);
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float cov = clamp(0.5 - d, 0.0, 1.0);
    if (cov <= 0.0) { outColor = vec4(0.0); return; }

    vec2 texel = 1.0 / vec2(textureSize(uScene, 0));
    vec2 uv = gl_FragCoord.xy * texel;
    // Per-pixel rotation hides the sampling pattern; a spiral of taps with Gaussian-ish weights does the blur.
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    const int N = 64;
    const float radius = 30.0;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int i = 0; i < N; ++i) {
        float t = (float(i) + 0.5) / float(N);
        float ang = float(i) * 2.399963 + noise * 6.2831853;
        float rr = sqrt(t) * radius;
        vec2 o = vec2(cos(ang), sin(ang)) * rr;
        float w = exp(-1.1 * t);
        sum += texture(uScene, uv + o * texel) * w;
        wsum += w;
    }
    vec4 blurred = sum / wsum;
    float ta = vColor.a;
    vec3 rgb = vColor.rgb * ta + blurred.rgb * (1.0 - ta);
    outColor = vec4(rgb, cov);
}
