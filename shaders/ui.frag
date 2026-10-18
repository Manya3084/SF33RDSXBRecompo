#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D canvas;

void main() {
    // Clip Y is up and Vulkan's v is down, so the canvas top stays the screen top.
    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
    out_color = texture(canvas, uv);
}
