#version 450
// Variant of quad.frag used to paint the translucent window chrome: adds a fine frosted-glass grain.
layout(set = 0, binding = 0) uniform sampler2D uAtlas;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vLocal;
layout(location = 3) flat in vec3 vShape;
layout(location = 0) out vec4 outColor;

float hash(vec2 p) {
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return fract((p.x + p.y) * p.x);
}

void main() {
    float coverage = texture(uAtlas, vUv).r;
    if (vShape.z != 0.0) {
        float r = min(abs(vShape.z), min(vShape.x, vShape.y));
        vec2 q = abs(vLocal) - (vShape.xy - r);
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        float inside = clamp(0.5 - d, 0.0, 1.0);
        coverage *= vShape.z > 0.0 ? inside : 1.0 - inside;
    }
    // Two octaves of grain, mostly bright specks (like frost on glass); premultiplied colour, so scale by alpha.
    vec2 fc = gl_FragCoord.xy;
    float n = hash(fc) * 0.65 + hash(floor(fc * 0.5) + 7.0) * 0.35;
    float grain = (n - 0.42) * 0.07;
    vec3 rgb = clamp(vColor.rgb + vec3(grain) * 0.85, 0.0, 1.0);
    outColor = vec4(rgb, vColor.a * coverage);
}
