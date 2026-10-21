#version 450
// The game area in the window. mode 0 keeps each texel whole, mode 1
// filters the scale, mode 2 draws a CRT over that filtered picture.
// src is the same rectangle the vertex shader maps onto the viewport.
layout(set = 0, binding = 0) uniform sampler2D screen;
layout(push_constant) uniform Push {
    layout(offset = 0) vec4 src;
    layout(offset = 16) float mode;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;

vec3 taken(vec2 uv, int sharp)
{
    if (sharp != 0) {
        vec2 size = vec2(textureSize(screen, 0));
        uv = (floor(uv * size) + 0.5) / size;
    }
    return texture(screen, uv).rgb;
}

void main()
{
    int mode = int(pc.mode + 0.5);
    if (mode != 2) {
        o = vec4(taken(v_uv, mode == 0 ? 1 : 0), 1.0);
        return;
    }

    vec2 span = pc.src.zw - pc.src.xy;
    vec2 p = (v_uv - pc.src.xy) / span;
    vec2 c = p * 2.0 - 1.0;
    c *= 1.0 + dot(c, c) * 0.08;
    p = c * 0.5 + 0.5;
    if (p.x < 0.0 || p.y < 0.0 || p.x > 1.0 || p.y > 1.0) {
        o = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 uv = pc.src.xy + span * p;
    vec3 col = taken(uv, 0);
    float scan = sin(fract(p.y * 448.0) * 3.14159265);
    col *= mix(0.68, 1.0, scan * scan);
    float mask_x = mod(gl_FragCoord.x, 3.0);
    vec3 mask = mask_x < 1.0 ? vec3(1.0, 0.80, 0.80)
              : mask_x < 2.0 ? vec3(0.80, 1.0, 0.80)
                             : vec3(0.80, 0.80, 1.0);
    col *= mask;
    float vig = smoothstep(1.15, 0.35, length((p - 0.5) * vec2(1.12, 1.0)));
    col *= mix(0.78, 1.0, vig);
    vec2 px = span / vec2(640.0, 448.0);
    vec3 glow = taken(uv + vec2(px.x, 0.0), 0) + taken(uv - vec2(px.x, 0.0), 0) +
                taken(uv + vec2(0.0, px.y), 0) + taken(uv - vec2(0.0, px.y), 0);
    o = vec4(col + glow * 0.03, 1.0);
}
