// drawcost's "game" fragment shader (no #version line: build.py and gl_main.c add it; PROG 0-7 defined ahead).
// Two stage textures, and the draw's uniform block (the game's pixel constants are in it too).
layout(std140, binding = 0) uniform U
{
    vec4 xform;
    vec4 color;
    ivec4 base;
    vec4 rest[157];
} u;

layout(binding = 0) uniform sampler2D tex0;
layout(binding = 1) uniform sampler2D tex1;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(tex0, v_uv);
#if (PROG & 3) == 0
    vec4 c = t * v_color;
#elif (PROG & 3) == 1
    vec4 c = t.bgra * v_color;
#elif (PROG & 3) == 2
    vec4 c = vec4(t.rgb * 0.5 + v_color.rgb * 0.5, 1.0);
#else
    vec4 c = vec4((1.0 - t.rgb) * v_color.rgb, 1.0);
#endif
#if PROG >= 4
    c.rgb = mix(c.rgb, texture(tex1, v_uv * 2.0).rgb, 0.25);
#endif
    o_color = c + u.rest[8 + PROG] * 0.0001;
}
