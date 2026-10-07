#version 450
layout(push_constant) uniform P { ivec4 rect; vec4 data; ivec4 config; uvec4 extra; } p;
layout(location=0) flat out uint output_index;
void main() {
    const vec2 corners[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
    output_index=uint(gl_VertexIndex/6);
    ivec2 selected=ivec2(output_index%16,output_index/16);
    // Full 16x16 output; in-bounds integer source rectangle, both extents>=2.
    // Do not deduplicate source positions: each output pixel owns one word.
    ivec2 source=((p.rect.xy+1)*16+(p.rect.zw-p.rect.xy-1)*selected)/16;
    source.x+=p.config.z;
    vec2 px=vec2(source)+corners[gl_VertexIndex%6];
    gl_Position=vec4(px.x/1024.0*2.0-1.0,1.0-px.y/1024.0*2.0,p.data.x,1.0);
}
