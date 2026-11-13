#version 450
layout(set = 0, binding = 0) uniform sampler2D uAtlas;   // R8 coverage atlas
layout(set = 0, binding = 1) uniform sampler2D uImages;  // RGBA8 premultiplied picture atlas (Markdown preview)

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vLocal;
layout(location = 3) flat in vec3 vShape;
layout(location = 4) flat in float vRing;
layout(location = 5) flat in float vImage;
layout(location = 0) out vec4 outColor;

void main() {
    if (vImage > 0.5) {
        vec4 t = texture(uImages, vUv);
        vec3 rgb = t.a > 0.0 ? t.rgb / t.a : vec3(0.0);   // the atlas holds premultiplied colour; blending here is straight alpha
        float cov = 1.0;
        if (vShape.z > 0.0) {   // rounded corners
            float r = min(vShape.z, min(vShape.x, vShape.y));
            vec2 q = abs(vLocal) - (vShape.xy - r);
            cov = clamp(0.5 - (length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r), 0.0, 1.0);
        }
        outColor = vec4(rgb * vColor.rgb, t.a * vColor.a * cov);
        return;
    }
    float coverage = texture(uAtlas, vUv).r;
    if (vShape.z != 0.0) {
        // Signed distance to a rounded box; one pixel of anti-aliasing.
        float r = min(abs(vShape.z), min(vShape.x, vShape.y));
        vec2 q = abs(vLocal) - (vShape.xy - r);
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        float inside = clamp(0.5 - d, 0.0, 1.0);
        coverage *= vShape.z > 0.0 ? inside : 1.0 - inside;
        if (vRing > 0.0) {   // outline: outer rounded box minus the same box shrunk by the ring width
            float inner = clamp(0.5 - (d + vRing), 0.0, 1.0);
            coverage = max(coverage - inner, 0.0);
        }
    }
    outColor = vec4(vColor.rgb, vColor.a * coverage);
}
