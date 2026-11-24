// drawcost's character fragment shader (no #version line: build.py and gl_main.c add it).
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = texture(tex, v_uv) * v_color;
}
