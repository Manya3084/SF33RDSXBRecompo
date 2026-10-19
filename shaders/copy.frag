#version 450
// The screen onto the output: opaque for the PS2-sized picture, alpha
// blended for what is drawn over it.
layout(set = 0, binding = 0) uniform sampler2D screen;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;

void main()
{
    o = texture(screen, v_uv);
}
