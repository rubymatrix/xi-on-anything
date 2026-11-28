// The deko3d back end's characters (gfx_deko3d.c gfx_chars_draw): gfx_gl.c's CHARS_GL in uam's rules - each
// stage compiled on its own (GFX_VS or GFX_FS defined ahead of this, after "#version 460"), the varyings at fixed
// locations, and clip space left as D3D's (deko3d's defaults: y up, z 0..1), so no y negation or z remap.
// Bindings as GL's: blocks 0 the draw's (alpha ref, pixel; the buffers' places, mirror), 1 the state (GfxCharState),
// 2 the palette, 3 the environment layer (GfxCharEnv); textures 1 and 2 the vertex and coordinate buffers (buffer
// images of words over their heap blocks: D.at.x and D.at.y the buffers' first words in them), 8 the texture, 9 and
// 10 the environment layer's (2D, cube).
#define sat(x) clamp(x, 0.0, 1.0)
layout(std140, binding = 0) uniform CU { vec4 params; uvec4 at; } D;
struct CL { vec4 pos, dir, diffuse, ambient, att; };
layout(std140, binding = 1) uniform CSB { mat4 wvp, world; CL light[4]; vec4 ambient, md, ma, me, tf, fog, fog_col; } S;
layout(std140, binding = 3) uniform CEB { mat4 wv, tm; vec4 kind; } E;
#ifdef GFX_VS
layout(std140, binding = 2) uniform CPB { mat4 P[128]; } B;
layout(binding = 1) uniform usamplerBuffer vbt;
layout(binding = 2) uniform usamplerBuffer uvt;
layout(location = 0) out vec2 o_uv;
layout(location = 1) out vec4 o_col;
layout(location = 2) out float o_z;
layout(location = 3) out vec3 o_env;
const vec3 SIGN[4] = vec3[4](vec3(1.0), vec3(-1.0, 1.0, 1.0), vec3(1.0, -1.0, 1.0), vec3(1.0, 1.0, -1.0));
vec4 vw4(int b) { return uintBitsToFloat(uvec4(texelFetch(vbt, b).x, texelFetch(vbt, b + 1).x, texelFetch(vbt, b + 2).x, texelFetch(vbt, b + 3).x)); }
void main() {
  int vid = gl_VertexID, b = int(D.at.x) + vid * 18;
  vec4 va = vw4(b), vb4 = vw4(b + 4);
  vec3 na = vw4(b + 8).xyz, nb = vw4(b + 12).xyz;
  uint bones = texelFetch(vbt, b + 16).x, mir = texelFetch(vbt, b + 17).x;
  uint ba = bones & 0xffu, bb = (bones >> 8) & 0xffu;
  if ((D.at.z != 0u) != (gl_InstanceID != 0)) {
    ba = (bones >> 16) & 0xffu, bb = bones >> 24;
    vec3 sa = SIGN[mir & 3u], sb = SIGN[(mir >> 2) & 3u];
    va.xyz *= sa; vb4.xyz *= sb; na *= sa; nb *= sb;
  }
  mat4 MA = B.P[ba & 127u], MB = B.P[bb & 127u];
  vec3 n = (MA * vec4(na, 0.0)).xyz + (MB * vec4(nb, 0.0)).xyz;
  vec3 p = (MA * va + MB * vb4).xyz;
  vec3 wp = (S.world * vec4(p, 1.0)).xyz;
  vec3 wn = normalize((S.world * vec4(n, 0.0)).xyz);
  vec4 pos = S.wvp * vec4(p, 1.0);
  o_z = pos.w;
  pos.x += pos.w * D.params.y;
  pos.y -= pos.w * D.params.z;
  gl_Position = pos;
  int ub = int(D.at.y) + 2 * vid;
  o_uv = uintBitsToFloat(uvec2(texelFetch(uvt, ub).x, texelFetch(uvt, ub + 1).x));
  o_env = vec3(0.0);
  if (E.kind.x > 0.0) {
    vec3 ne = normalize(mat3(E.wv) * n);
    vec3 c = E.kind.x > 1.5 ? reflect(normalize((E.wv * vec4(p, 1.0)).xyz), ne) : ne;
    o_env = (E.tm * vec4(c, 1.0)).xyz;
  }
  vec3 amb = S.ambient.rgb, dif = vec3(0.0);
  for (int k = 0; k < 4; ++k) {
    CL l = S.light[k];
    if (l.dir.w == 0.0) continue;
    float at = 1.0;
    vec3 Ld = l.dir.xyz;
    if (l.dir.w > 1.5) {
      vec3 d = l.pos.xyz - wp;
      float dist = length(d);
      if (dist > l.pos.w) continue;
      Ld = d / max(dist, 1e-6);
      at = 1.0 / max(l.att.x + l.att.y * dist + l.att.z * dist * dist, 1e-6);
    }
    amb += l.ambient.rgb * at;
    dif += l.diffuse.rgb * at * max(dot(wn, Ld), 0.0);
  }
  o_col = vec4(sat(S.ma.rgb * amb + S.md.rgb * dif + S.me.rgb), S.md.a);
}
#endif
#ifdef GFX_FS
layout(binding = 8) uniform sampler2D t;
layout(binding = 9) uniform sampler2D te;
layout(binding = 10) uniform samplerCube tc;
layout(location = 0) in vec2 o_uv;
layout(location = 1) in vec4 o_col;
layout(location = 2) in float o_z;
layout(location = 3) in vec3 o_env;
layout(location = 0) out vec4 oc;
void main() {
  if (D.params.w > 0.5) { // an actor's shadow picture: flat, the factor the game draws it with (0x80ffffff)
    oc = vec4(1.0, 1.0, 1.0, 128.0 / 255.0);
    return;
  }
  vec4 tx = texture(t, o_uv), tf = S.tf;
  vec3 c;
  float a;
  if (E.kind.x > 0.0) {
    vec3 c0 = sat(2.0 * tx.rgb * o_col.rgb);
    float a0 = sat(2.0 * tx.a * tf.a);
    vec4 e = E.kind.y > 0.5 ? texture(tc, o_env) : texture(te, o_env.xy);
    c = sat(c0 + a0 * e.rgb), a = sat(a0 + e.a);
  } else {
    c = sat(2.0 * sat(2.0 * tx.rgb * o_col.rgb) * tf.rgb);
    a = sat(4.0 * sat(2.0 * tx.a * tf.a) * tf.a);
  }
  if (!(a * 255.0 > D.params.x)) discard;
  if (S.fog.z > 0.0) c = mix(S.fog_col.rgb, c, sat((S.fog.y - o_z) / max(S.fog.y - S.fog.x, 1e-6)));
  oc = vec4(c, a);
}
#endif
