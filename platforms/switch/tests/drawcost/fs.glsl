// drawcost's fragment shader: no #version line here (build.py adds "#version 460" for uam, gl_main.c
// "#version 430 core"), and PROG (0-3) is defined ahead of it, giving 4 different programs.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(tex, v_uv);
#if PROG == 0
    o_color = t * v_color;
#elif PROG == 1
    o_color = t.bgra * v_color;
#elif PROG == 2
    o_color = vec4(t.rgb * 0.5 + v_color.rgb * 0.5, 1.0);
#else
    o_color = vec4((1.0 - t.rgb) * v_color.rgb, 1.0);
#endif
}
