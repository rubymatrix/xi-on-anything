#version 450
layout(push_constant) uniform P { ivec4 rect; vec4 data; ivec4 config; uvec4 extra; } p;
layout(location=1) out vec4 mask_color;
void main() { mask_color=p.data.y==0.0?vec4(0):vec4(1,1,1,128.0/255.0); }
