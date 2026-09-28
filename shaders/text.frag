#version 450
layout(binding = 0) uniform sampler2D atlas;
layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;
layout(location = 0) out vec4 out_color;
void main() {
    // Signed-distance coverage: resolution-independent edges, one smoothstep
    // wide. fwidth keeps the transition ~1 screen pixel at any text size.
    float d = texture(atlas, frag_uv).r;
    float w = fwidth(d) * 0.75 + 0.02;
    float a = smoothstep(0.5 - w, 0.5 + w, d);
    if (a < 0.01) discard;
    out_color = vec4(frag_color.rgb, frag_color.a * a);
}
