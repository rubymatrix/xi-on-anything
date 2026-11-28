// drawcost's character vertex shader (no #version line: build.py and gl_main.c add it). Like gfx_gl.c's
// CHARS_GL: three uniform blocks (the item, the character's state, the palette) and two texture buffers (the
// vertices and the UVs), drawn indexed, a second instance beside the first.
layout(std140, binding = 0) uniform Item
{
    vec4 params;
    uvec4 at;
} it;

layout(std140, binding = 1) uniform State
{
    vec4 xform; // offset xy, scale zw
    vec4 bones[127];
} st;

layout(std140, binding = 2) uniform Pal
{
    vec4 pal[32];
} pal;

layout(binding = 3) uniform usamplerBuffer vbt;
layout(binding = 4) uniform usamplerBuffer uvt;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
    int v = gl_VertexID;
    vec2 p = uintBitsToFloat(uvec2(texelFetch(vbt, 2 * v).x, texelFetch(vbt, 2 * v + 1).x));
    vec2 uv = uintBitsToFloat(uvec2(texelFetch(uvt, 2 * v).x, texelFetch(uvt, 2 * v + 1).x));
    p += st.bones[v & 7].xy * 0.0001;
    vec2 o = st.xform.xy + vec2(float(gl_InstanceID) * st.xform.z * 2.5, 0.0);
    gl_Position = vec4(p * st.xform.zw + o, 0.0, 1.0);
    v_uv = uv;
    v_color = pal.pal[v & 31] * it.params;
}
