#version 450
// A screen-space vertex of the 640x480 game screen. D3D puts pixel centres
// on integer coordinates and Vulkan on half-integers, hence the half pixel.
layout(location = 0) in vec3 pos;
layout(location = 1) in vec4 color;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main()
{
    gl_Position = vec4((pos.x + 0.5) / 320.0 - 1.0, (pos.y + 0.5) / 240.0 - 1.0, pos.z, 1.0);
    v_color = color;
    v_uv = uv;
}
