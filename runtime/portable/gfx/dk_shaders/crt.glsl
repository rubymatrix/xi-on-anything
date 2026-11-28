// The deko3d back end's CRT pass at the present (gfx_deko3d.c): gfx_gl.c's CRT_GLSL in uam's rules - each stage
// compiled on its own (GFX_VS or GFX_FS ahead of this), its two settings in a uniform block (uam has no plain
// uniforms), the varying at location 0. A triangle over the screen; row 0 of the picture (D3D's top) at the top.
layout(std140, binding = 0) uniform CRT {
    vec4 u_src;  // the picture: w, h, 1/w, 1/h
    vec4 u_mode; // x: 1 scanlines, 2 with the grille; y: the curve; z: the lines' depth
};
#ifdef GFX_VS
layout(location = 0) out vec2 v_uv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
#endif
#ifdef GFX_FS
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(binding = 0) uniform sampler2D u_tex;
vec3 row_color(float x, float row) { vec3 c = texture(u_tex, vec2(x, row * u_src.w)).rgb; return c * c; }
vec3 beam(float d, vec3 c)
{
    vec3 w = mix(vec3(0.24), vec3(0.42), sqrt(c));
    return exp(-0.5 * d * d / (w * w));
}
void main()
{
    vec2 uv = v_uv;
    float edge = 1.0;
    if (u_mode.y > 0.5)
    {
        vec2 c = uv * 2.0 - 1.0;
        c *= 1.0 + vec2(0.035, 0.05) * (c.yx * c.yx);
        uv = c * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
        vec2 v = uv * (1.0 - uv.yx);
        edge = clamp(pow(v.x * v.y * 24.0, 0.25), 0.0, 1.0);
    }
    float y = uv.y * u_src.y, row = floor(y) + 0.5, d = y - row;
    float other = row + (d < 0.0 ? -1.0 : 1.0);
    vec3 c0 = row_color(uv.x, row), c1 = row_color(uv.x, clamp(other, 0.5, u_src.y - 0.5));
    vec3 lit = c0 * beam(abs(d), c0) + c1 * beam(1.0 - abs(d), c1);
    vec3 c = mix(c0, lit * 1.45, u_mode.z);
    if (u_mode.x > 1.5)
    {
        int m = int(mod(gl_FragCoord.x, 3.0));
        vec3 grille = vec3(0.72);
        grille[m] = 1.0;
        c *= grille * 1.22;
    }
    o_color = vec4(sqrt(clamp(c, 0.0, 1.0)) * edge, 1.0);
}
#endif
