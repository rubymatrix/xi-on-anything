#version 450
layout(early_fragment_tests) in;
layout(push_constant) uniform P { ivec4 rect; vec4 data; ivec4 config; uvec4 extra; } p;
layout(location=0) flat in uint output_index;
layout(set=0,binding=1,std430) writeonly buffer Pixels { uint words[]; } pixels;
void main() {
    // The slice was cleared before the render pass. Failed depth tests leave 0.
    // Literal BGRA8 bytes preserve full original white, alpha 128, not a count.
    pixels.words[p.extra.x+output_index]=0x80ffffffu;
}
