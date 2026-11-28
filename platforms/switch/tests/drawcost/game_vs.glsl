// drawcost's "game" vertex shader (no #version line: build.py and gl_main.c add it; PROG 0-7 defined ahead).
// Like gfx_gl.c's generated ones: the vertices are 32-bit words in a texture buffer read by texelFetch, at a word
// offset from the uniform block (GfxU's offset[]), and the block is one for both stages.
layout(std140, binding = 0) uniform U
{
    vec4 xform; // offset xy, scale zw
    vec4 color;
    ivec4 base; // x: the first vertex's word in the stream
    vec4 rest[157];
} u;

layout(binding = 2) uniform usamplerBuffer stream0;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
    int b = u.base.x + gl_VertexID * 4;
    vec2 p = uintBitsToFloat(uvec2(texelFetch(stream0, b).x, texelFetch(stream0, b + 1).x));
    vec2 uv = uintBitsToFloat(uvec2(texelFetch(stream0, b + 2).x, texelFetch(stream0, b + 3).x));
#if (PROG & 3) == 1
    p = vec2(p.x + 0.1 * p.y, p.y);
#elif (PROG & 3) == 2
    p = p.yx;
    uv = uv.yx;
#elif (PROG & 3) == 3
    p = p * 0.9;
#endif
    gl_Position = vec4(p * u.xform.zw + u.xform.xy, 0.0, 1.0);
    v_uv = uv;
    v_color = u.color + u.rest[PROG] * 0.0001;
}
