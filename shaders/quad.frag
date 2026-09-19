#version 450

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(binding = 1) uniform sampler2D gb_tex;

void main() {
    out_color = texture(gb_tex, frag_uv);
}
