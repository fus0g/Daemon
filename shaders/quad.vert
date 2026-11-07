#version 450
// One instanced quad per rectangle / glyph. Positions are in framebuffer pixels.
layout(location = 0) in vec4 inRect;    // x, y, w, h
layout(location = 1) in vec4 inUv;      // u0, v0, u1, v1
layout(location = 2) in vec4 inColor;   // rgba8 unorm
layout(location = 3) in float inRadius; // corner radius in px; <0 = inverted (corner mask)

layout(push_constant) uniform PC { vec2 viewport; } pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec2 vLocal;        // pixel offset from the quad centre
layout(location = 3) flat out vec3 vShape;   // half width, half height, radius
layout(location = 4) flat out float vRing;   // > 0: draw only an outline of this width (inRadius >= 1000 marks a ring)

void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 p = inRect.xy + corner * inRect.zw;
    gl_Position = vec4(p / pc.viewport * 2.0 - 1.0, 0.0, 1.0);
    bool ring = inRadius >= 1000.0;
    vUv = ring ? inUv.xy : mix(inUv.xy, inUv.zw, corner);   // rings keep their width in inUv.z
    vColor = inColor;
    vLocal = (corner - 0.5) * inRect.zw;
    vShape = vec3(inRect.zw * 0.5, ring ? inRadius - 1000.0 : inRadius);
    vRing = ring ? inUv.z : 0.0;
}
