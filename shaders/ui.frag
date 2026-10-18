#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D canvas;

void main() {
    // A positive viewport puts clip y of -1 at the top, which is where v_uv.y is 0.
    out_color = texture(canvas, v_uv);
}
