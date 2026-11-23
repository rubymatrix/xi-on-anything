// drawcost's vertex shader: no #version line here (build.py adds "#version 460" for uam, gl_main.c
// "#version 430 core"), and PROG (0-3) is defined ahead of it, giving 4 different programs.
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;

layout(std140, binding = 0) uniform U
{
    vec4 xform; // offset xy, scale zw
    vec4 color;
    vec4 pad[14];
} u;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
    vec2 p = a_pos;
#if PROG == 1
    p = vec2(p.x + 0.1 * p.y, p.y);
#elif PROG == 2
    p = p.yx;
#elif PROG == 3
    p = p * 0.9;
#endif
    gl_Position = vec4(p * u.xform.zw + u.xform.xy, 0.0, 1.0);
#if PROG == 2
    v_uv = a_uv.yx;
#else
    v_uv = a_uv;
#endif
    v_color = u.color;
}
