#version 450
// One triangle over the viewport, which is where the screen lands in the window.
layout(push_constant) uniform Push {
    vec4 src; // the screen's x0, y0, x1, y1, normalized
} pc;
layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = mix(pc.src.xy, pc.src.zw, p);
}
