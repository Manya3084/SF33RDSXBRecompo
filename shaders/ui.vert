#version 450

layout(push_constant) uniform Push {
    float rot;
} pc;
layout(location = 0) out vec2 v_uv;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    // the swapchain image's quarter turns (Android's pre-rotation): the
    // texture coordinates turn the other way, so the picture is upright
    int r = int(pc.rot + 0.5);
    vec2 st = r == 1 ? vec2(p.y, 1.0 - p.x) : r == 2 ? vec2(1.0 - p.x, 1.0 - p.y) : r == 3 ? vec2(1.0 - p.y, p.x) : p;
    v_uv = st;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
