#version 450

// Upscaling shader for the emulated frame (GB/GBC 160x144 and GBA 240x160).
//
// The texture is B8G8R8A8_SRGB and the swapchain is sRGB too, so every fetch
// below returns LINEAR light and the final write is re-encoded to sRGB for
// free. Interpolating between those linearised fetches is therefore already
// gamma-correct, which is exactly what SameBoy has to do by hand with
// pow(c, 2.2) on each sample (Shaders/MasterShader.fsh) because its texture
// is a plain GL_RGBA8. We get the same result with no extra pow() calls.
//
// The sampler is VK_FILTER_NEAREST and every fetch is taken at an exact texel
// centre, so `texel()` returns one precise texel and all filtering is done
// here. That keeps the sampler immutable and makes the grid effect able to
// reach neighbouring texels by integer offset.

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(binding = 1) uniform sampler2D gb_tex;

layout(push_constant) uniform Push {
	uint mode;     // VideoFilter
	float grid;    // LCD grid border depth, 0 = off
	uint flags;    // reserved, must be 0
	uint pad_;
} pc;

ivec2 src_size()
{
	return textureSize(gb_tex, 0);
}

// One exact texel, linearised by the sRGB image view, clamped to the edge.
vec4 texel(ivec2 p)
{
	ivec2 s = src_size();
	return texture(gb_tex, (vec2(clamp(p, ivec2(0), s - ivec2(1))) + 0.5) / vec2(s));
}

// Source-pixel coordinate plus the fractional position inside that pixel.
void pixel_coord(vec2 uv, out ivec2 base, out vec2 f)
{
	vec2 s = vec2(src_size());
	vec2 p = uv * s - 0.5;
	base = ivec2(floor(p));
	f = p - vec2(base);
}

vec4 bilinear(vec2 uv, bool smoothstepped)
{
	ivec2 i;
	vec2 f;
	pixel_coord(uv, i, f);
	if (smoothstepped)
		f = f * f * (3.0 - 2.0 * f); // smoothstep weights (SameBoy)
	vec4 c00 = texel(i);
	vec4 c10 = texel(i + ivec2(1, 0));
	vec4 c01 = texel(i + ivec2(0, 1));
	vec4 c11 = texel(i + ivec2(1, 1));
	return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

// Scale2x / EPX (SameBoy Shaders/Scale2x.fsh). Picks one of four source
// texels per quadrant from the 3x3 neighbourhood, which smooths diagonal
// edges without touching axis-aligned ones.
vec4 scale2x(vec2 uv)
{
	ivec2 i;
	vec2 f;
	pixel_coord(uv, i, f);
	vec4 B = texel(i + ivec2(-1, -1));
	vec4 D = texel(i + ivec2(0, -1));
	vec4 E = texel(i);
	vec4 F = texel(i + ivec2(1, -1));
	vec4 H = texel(i + ivec2(-1, 0));
	vec4 tl = (D == B && D != F && D != H) ? D : E;
	vec4 tr = (B == F && B != D && F != H) ? F : E;
	vec4 bl = (D == H && D != B && H != F) ? D : E;
	vec4 br = (H == F && D != H && B != F) ? F : E;
	return (f.y < 0.5) ? ((f.x < 0.5) ? tl : tr) : ((f.x < 0.5) ? bl : br);
}

// LCD grid (SameBoy MonoLCD.fsh): darken a fixed-width border on all four
// sides of every source pixel. The border is 1/6 of a pixel, as in SameBoy's
// six-cell LCD construction, so it scales with the window instead of being a
// fixed pixel count.
vec4 apply_grid(vec2 uv, vec4 c)
{
	if (pc.grid <= 0.0)
		return c;
	ivec2 i;
	vec2 f;
	pixel_coord(uv, i, f);
	// Corner regions use the smaller of the two axis factors.
	float mx = 1.0;
	if (f.x < 1.0 / 6.0)
		mx = min(mx, f.x * 6.0);
	else if (f.x > 5.0 / 6.0)
		mx = min(mx, (1.0 - f.x) * 6.0);
	float my = 1.0;
	if (f.y < 1.0 / 6.0)
		my = min(my, f.y * 6.0);
	else if (f.y > 5.0 / 6.0)
		my = min(my, (1.0 - f.y) * 6.0);
	// pc.grid is the maximum darkening; edge of the border is fully lit.
	float m = min(mx, my);
	float factor = m * pc.grid + (1.0 - pc.grid);
	return vec4(c.rgb * factor, c.a);
}

void main()
{
	vec4 c;
	switch (pc.mode) {
	case 1u: // Bilinear
		c = bilinear(frag_uv, false);
		break;
	case 2u: // SmoothBilinear
		c = bilinear(frag_uv, true);
		break;
	case 3u: // Scale2x
		c = scale2x(frag_uv);
		break;
	default: // Nearest: a single fetch at the sampled coordinate.
		c = texture(gb_tex, frag_uv);
		break;
	}
	out_color = apply_grid(frag_uv, c);
}
