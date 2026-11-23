#version 450
layout(push_constant) uniform P { ivec4 rect; vec4 data; ivec4 config; uvec4 extra; } p;
layout(location=0) out vec2 uv;
void main() {
    const vec2 corners[4]=vec2[4](vec2(0,0),vec2(1,0),vec2(0,1),vec2(1,1));
    vec2 t=corners[gl_VertexIndex];
    vec2 px=p.config.y==1?t*16.0:mix(vec2(p.rect.xy),vec2(p.rect.zw),t);
    float extent=float(p.config.x);
    gl_Position=vec4(px.x/extent*2.0-1.0+1.0/extent,
                     1.0-px.y/extent*2.0-1.0/extent,p.data.x,1.0);
    uv=mix((vec2(p.rect.xy)+1.0)/1024.0,vec2(p.rect.zw)/1024.0,t);
}
