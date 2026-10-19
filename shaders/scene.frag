#version 450
// The game's fixed-function stage 0: a texture (indexed through a palette,
// or direct), the vertex colour, the colour and alpha ops (D3DTOP numbers)
// with texture as argument 1 and the vertex colour as argument 2, and the
// alpha test. Filtering and addressing are done here so that an indexed
// texture is filtered after its palette lookup.
layout(set = 0, binding = 0) uniform texture2D tex[256];
layout(set = 0, binding = 1) uniform texture2D pal;
layout(set = 0, binding = 2) uniform sampler smp;

layout(push_constant) uniform Push {
    uint tex;
    uint pal;
    uint ops;   // colour op | alpha op << 8 | address u << 16 | address v << 24
    uint misc;  // point 1, indexed 2, texel uv 4, textured 8, alpha test 16, func << 5, ref << 8
    vec2 size;
    float scale; // screen pixels per 640x480 pixel
} pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in vec4 v_rect; // the quad's u0, v0, u1, v1
layout(location = 0) out vec4 o;

int address(int i, int n, uint mode)
{
    if (mode == 3u) {
        return clamp(i, 0, n - 1);
    }
    if (mode == 2u) {
        int m = i % (2 * n);
        if (m < 0) {
            m += 2 * n;
        }
        return m < n ? m : 2 * n - 1 - m;
    }
    int w = i % n;
    return w < 0 ? w + n : w;
}

// The texels the quad covers. Drawn 1:1, as retail draws it, every sample
// lands on a texel centre inside them; drawn larger, the edge samples
// would blend in the next cell of the texture's atlas, so taps clamp here.
ivec2 lo;
ivec2 hi;

vec4 fetch(ivec2 t)
{
    ivec2 n = ivec2(pc.size);
    t = clamp(t, lo, hi);
    ivec2 a = ivec2(address(t.x, n.x, (pc.ops >> 16) & 0xFFu), address(t.y, n.y, (pc.ops >> 24) & 0xFFu));
    vec4 c = texelFetch(sampler2D(tex[pc.tex], smp), a, 0);
    if ((pc.misc & 2u) != 0u) {
        int index = int(c.r * 255.0 + 0.5);
        c = texelFetch(sampler2D(pal, smp), ivec2(index, int(pc.pal)), 0);
    }
    return c;
}

vec4 sample_texture()
{
    vec2 scale = (pc.misc & 4u) != 0u ? vec2(1.0) : pc.size;
    // the texture coordinate at the centre of the 640x480 pixel this one
    // is part of, which is where a 640x480 screen samples it
    vec2 base = (floor(gl_FragCoord.xy / pc.scale) + 0.5) * pc.scale;
    vec2 d = base - gl_FragCoord.xy;
    vec2 uv = v_uv + dFdx(v_uv) * d.x + dFdy(v_uv) * d.y;
    vec2 t = uv * scale;
    vec2 r0 = v_rect.xy * scale;
    vec2 r1 = v_rect.zw * scale;
    lo = ivec2(floor(r0 + 1.0 / 1024.0));
    hi = max(lo, ivec2(ceil(r1 - 1.0 / 1024.0)) - 1);
    if ((pc.misc & 1u) != 0u) {
        // NV2A interpolates in fixed point, so a sample exactly on a texel
        // edge takes the texel after it; float interpolation lands a hair
        // either side, and the bias settles it the hardware's way.
        return fetch(ivec2(floor(t + 1.0 / 1024.0)));
    }
    t -= 0.5;
    ivec2 i = ivec2(floor(t));
    vec2 f = t - floor(t);
    vec4 a = mix(fetch(i), fetch(i + ivec2(1, 0)), f.x);
    vec4 b = mix(fetch(i + ivec2(0, 1)), fetch(i + ivec2(1, 1)), f.x);
    return mix(a, b, f.y);
}

float op1(uint op, float t, float d)
{
    if (op == 2u) return t;
    if (op == 3u) return d;
    if (op == 7u) return min(t + d, 1.0);
    return t * d;
}

vec3 op3(uint op, vec3 t, vec3 d)
{
    if (op == 2u) return t;
    if (op == 3u) return d;
    if (op == 7u) return min(t + d, vec3(1.0));
    return t * d;
}

bool passes(uint func, float a, float ref)
{
    if (func == 0u) return false;
    if (func == 1u) return a < ref;
    if (func == 2u) return a == ref;
    if (func == 3u) return a <= ref;
    if (func == 4u) return a > ref;
    if (func == 5u) return a != ref;
    if (func == 6u) return a >= ref;
    return true;
}

void main()
{
    uint cop = pc.ops & 0xFFu;
    uint aop = (pc.ops >> 8) & 0xFFu;
    vec4 c = v_color;
    if ((pc.misc & 8u) != 0u && cop != 1u) {
        vec4 t = sample_texture();
        c.rgb = op3(cop, t.rgb, v_color.rgb);
        if (aop != 1u) {
            c.a = op1(aop, t.a, v_color.a);
        }
    }
    if ((pc.misc & 16u) != 0u) {
        float a = floor(c.a * 255.0 + 0.5);
        if (!passes((pc.misc >> 5) & 7u, a, float((pc.misc >> 8) & 0xFFu))) {
            discard;
        }
    }
    o = c;
}
