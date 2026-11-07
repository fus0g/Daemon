#version 450
layout(set = 0, binding = 0) uniform sampler2D uAtlas;   // R8 coverage atlas

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vLocal;
layout(location = 3) flat in vec3 vShape;
layout(location = 4) flat in float vRing;
layout(location = 0) out vec4 outColor;

void main() {
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
