#version 450

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;

layout(binding = 0) uniform UBO {
    mat4 mvp;
} ubo;

layout(location = 0) out vec2 frag_uv;

void main() {
    gl_Position = ubo.mvp * vec4(in_pos, 0.0, 1.0);
    frag_uv = in_uv;
}
