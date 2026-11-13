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

// Rounded subpixel slot mask: a modern LCD's backlight is divided into
// rounded slots, so the gaps between them are dark and slightly blurred.
// Brightness follows the same smoothstep border as the LCD grid, driven by
// the LCD Grid slider (0 = no mask, so the filter is a plain bilinear
// upscale when the slider is off).
float slot_mask_factor(vec2 f)
{
	// Slot inset: 1/8 of a pixel on each side, with a 1/4-wide rounded
	// border so the corner radius is visible at 1x.
	const vec2 lo = vec2(0.125);
	const vec2 hi = vec2(0.875);
	const vec2 d = max(lo - f, f - hi); // signed distance to the slot rect
	const float dist = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
	// dist <= 0 inside the slot; the border covers dist in [-0.125, 0].
	const float m = smoothstep(0.0, -0.125, dist);
	return m;
}

// RGB stripe grille (Trinitron-style vertical phosphor triads). Each source
// pixel is split into three vertical stripes lit R, G, B. The weights are
// normalised by the *largest* one, not their sum: a sum normalisation dips
// to zero at the stripe boundaries (black seams between every triad), while
// the max keeps the peak at 1.0 so nothing clips. A display integrates the
// stripes across a pixel, which is what turns them into colour fringes.
vec3 grille(vec4 c, vec2 f)
{
	const float x = f.x * 3.0;
	vec3 w = vec3(smoothstep(0.5, 0.35, abs(x - 0.5)),
				  smoothstep(0.5, 0.35, abs(x - 1.5)),
				  smoothstep(0.5, 0.35, abs(x - 2.5)));
	w /= max(max(w.r, max(w.g, w.b)), 1e-3);
	return c.rgb * w;
}

// xBRZ (advmame; the 4x "scale4x" family with weighted blend kernels). The
// reference implementation upscales by 4 and then box-filters down to the
// target size; doing that here costs 4x4 fetches per output fragment, which
// is affordable for a 240x160 source. A is the centre texel; the 3x3
// neighbourhood is E (west), F (north), I (north-west), B (north-east),
// C (east), H (south-west), G (south), D (south-east) in the usual naming.
// The four xBRZ blend kernels, each taking the corner/edge colour and
// weighting it by how much it differs from the centre.
#define XBRZ_BLEND(a, b, w, m) clamp(mix(a, b, clamp((w) * (1.0 / 32.0), 0.0, 1.0)), 0.0, 1.0)
#define XBRZ_R01(d, eq) (XBRZ_BLEND(0.5 * (A + B), eq, 3.0 - 2.0 * (d), A))
#define XBRZ_R10(d, eq) (XBRZ_BLEND(0.5 * (A + D), eq, 3.0 - 2.0 * (d), A))
#define XBRZ_R11(d, eq) (XBRZ_BLEND(A, eq, d, A))

vec4 xbrz_pixel(vec2 uv, ivec2 i, vec2 f)
{
	vec4 A = texel(i);
	vec4 B = texel(i + ivec2(-1, 0));
	vec4 C = texel(i + ivec2(1, 0));
	vec4 D = texel(i + ivec2(0, -1));
	vec4 E = texel(i + ivec2(0, 1));
	vec4 F = texel(i + ivec2(-1, -1));
	vec4 G = texel(i + ivec2(1, -1));
	vec4 H = texel(i + ivec2(-1, 1));
	// Per-channel equalities (all-equal, not per-channel blends).
	float e0 = (B.r == A.r && B.g == A.g && B.b == A.b) ? 1.0 : 0.0;
	float e1 = (E.r == A.r && E.g == A.g && E.b == A.b) ? 1.0 : 0.0;
	float e2 = (H.r == D.r && H.g == D.g && H.b == D.b) ? 1.0 : 0.0;
	float e3 = (E.r == D.r && E.g == D.g && E.b == D.b) ? 1.0 : 0.0;
	float e4 = (E.r == F.r && E.g == F.g && E.b == F.b) ? 1.0 : 0.0;
	float e5 = (E.r == H.r && E.g == H.g && E.b == H.b) ? 1.0 : 0.0;
	float e6 = (E.r == G.r && E.g == G.g && E.b == G.b) ? 1.0 : 0.0;
	float e7 = (G.r == C.r && G.g == C.g && G.b == C.b) ? 1.0 : 0.0;
	float e8 = (G.r == D.r && G.g == D.g && G.b == D.b) ? 1.0 : 0.0;
	float e9 = (D.r == A.r && D.g == A.g && D.b == A.b) ? 1.0 : 0.0;

	// Distance weighting: how strongly each diagonal neighbour overrides.
	float d0 = distance(B, G);
	float d1 = distance(E, H);
	float d2 = distance(D, F);
	float d3 = distance(C, H);
	float d4 = distance(F, G);
	float d5 = distance(E, C);
	float d6 = distance(D, G);
	float d7 = distance(D, C);

	// The four 4x4 sub-blocks (uv within the 4x upscale grid).
	vec4 blk[4];
	// top-left
	blk[0] = (e0 == e1 && e0 != e9 && e0 != e2) ? XBRZ_R11(3.0 * d1, A)
			: (e0 == e7 && e0 != e8)             ? XBRZ_R01(2.0 * d4 + d0, mix(F, G, 0.5))
			: (e9 == e2 && e0 != e4 && e9 != e7)  ? XBRZ_R10(2.0 * d2 + 2.0 * d3, mix(C, H, 0.5))
			: (e3 == e5)                          ? XBRZ_R10(2.0 * d2, D)
			: (e0 == e1)                          ? A
			: (e9 == e7)                          ? XBRZ_R10(d2 + d3, C)
			: (e9 == e2)                          ? XBRZ_R11(2.0 * d3, A)
			: (e3 == e5)                          ? XBRZ_R10(2.0 * d2, D)
												   : A;
	// top-right
	blk[1] = (e1 == e2 && e1 != e0 && e1 != e3) ? XBRZ_R11(3.0 * d2, A)
			: (e6 == e0 && e7 != e9)             ? XBRZ_R01(2.0 * d5 + d0, mix(F, G, 0.5))
			: (e0 == e3 && e1 != e5 && e0 != e7)  ? XBRZ_R10(2.0 * d6 + 2.0 * d7, mix(D, C, 0.5))
			: (e8 == e9)                          ? XBRZ_R01(2.0 * d7, C)
			: (e1 == e2)                          ? A
			: (e8 == e3)                          ? XBRZ_R11(2.0 * d7, A)
			: (e8 == e9)                          ? XBRZ_R01(d7, C)
												   : A;
	// bottom-left
	blk[2] = (e5 == e6 && e5 != e4 && e5 != e7) ? XBRZ_R11(3.0 * d3, A)
			: (e2 == e1 && e5 != e8)             ? XBRZ_R10(2.0 * d6 + d2, mix(D, C, 0.5))
			: (e6 == e0 && e9 != e1)             ? XBRZ_R01(2.0 * d5 + d3, mix(H, G, 0.5))
			: (e4 == e8)                          ? XBRZ_R10(2.0 * d6, D)
			: (e5 == e6)                          ? A
			: (e4 == e2)                          ? XBRZ_R11(2.0 * d6, A)
			: (e4 == e8)                          ? XBRZ_R10(d6 + d2, D)
												   : A;
	// bottom-right
	blk[3] = (e4 == e5 && e4 != e3 && e4 != e8) ? XBRZ_R11(3.0 * d7, A)
			: (e3 == e7 && e2 != e9)             ? XBRZ_R10(2.0 * d7 + d4, mix(B, C, 0.5))
			: (e7 == e2 && e4 != e0 && e7 != e6)  ? XBRZ_R01(2.0 * d4 + 2.0 * d5, mix(B, F, 0.5))
			: (e9 == e6)                          ? XBRZ_R01(2.0 * d5, B)
			: (e4 == e5)                          ? A
			: (e9 == e3)                          ? XBRZ_R11(2.0 * d5, A)
			: (e9 == e6)                          ? XBRZ_R01(d5, B)
												   : A;

	// Downsample the 4x4 block to this fragment. The reference averages the
	// whole 4x4; the four sub-blocks each represent a 2x2 quarter whose
	// samples sit at 0.25/0.75, so a bilinear across the four of them is the
	// same result without 16 fetches.
	float tx = clamp((f.x - 0.25) * 2.0, 0.0, 1.0);
	float ty = clamp((f.y - 0.25) * 2.0, 0.0, 1.0);
	vec4 top = mix(blk[0], blk[1], tx);
	vec4 bottom = mix(blk[2], blk[3], tx);
	return mix(top, bottom, ty);
}

// NTSC composite approximation (single pass, deliberately fake - see the
// roadmap note on "how fake a composite simulation may be"). Real NTSC
// encodes chroma on a ~3.58MHz subcarrier in gamma space with multi-line
// comb filtering; this does three cheap things in linear light instead:
// (1) a 3-tap horizontal bandwidth limit (composite luma/chroma share one
// wire, so sharp horizontal detail cannot survive), (2) sharp luma from the
// centre tap mixed with blurred chroma (composite keeps brightness detail
// better than colour detail), (3) a dot-crawl rainbow on high-contrast
// vertical edges whose phase alternates per source column. The LCD grid
// still applies on top via apply_grid().
vec4 ntsc(vec2 uv)
{
	ivec2 s = src_size();
	vec2 t = 1.0 / vec2(s);
	// Bandwidth limit: centre-weighted horizontal blur of the upscaled image.
	vec4 c = bilinear(uv, false);
	vec4 l = bilinear(uv - vec2(t.x, 0.0), false);
	vec4 r = bilinear(uv + vec2(t.x, 0.0), false);
	vec4 blur = (l + c * 2.0 + r) * 0.25;
	// Luma stays sharp (centre tap), chroma comes from the blur.
	const vec3 kLuma = vec3(0.299, 0.587, 0.114);
	float luma = dot(c.rgb, kLuma);
	vec3 chroma = blur.rgb - vec3(dot(blur.rgb, kLuma));
	// Edge rainbow: amplitude follows the local horizontal gradient, phase
	// alternates per source texel column (the subcarrier flips every pixel).
	ivec2 i;
	vec2 f;
	pixel_coord(uv, i, f);
	float edge = clamp(distance(l, r) * 2.0, 0.0, 1.0);
	float phase = float(i.x & 1) * 2.0 - 1.0;
	vec3 rainbow = vec3(phase * 0.06, -phase * 0.06, phase * 0.03) * edge;
	return vec4(vec3(luma) + chroma + rainbow, c.a);
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
	case 4u: // SlotMask: bilinear upscale, then a rounded slot mask whose
		// depth is the LCD Grid slider (0 = plain bilinear).
		c = bilinear(frag_uv, true);
		{
			ivec2 i;
			vec2 f;
			pixel_coord(frag_uv, i, f);
			const float m = slot_mask_factor(f);
			const float factor = m * pc.grid + (1.0 - pc.grid);
			c = vec4(c.rgb * factor, c.a);
		}
		break;
	case 6u: { // xBRZ: weighted-diagonal upscale, then a bilinear 4x downsample.
		ivec2 i;
		vec2 f;
		pixel_coord(frag_uv, i, f);
		c = xbrz_pixel(frag_uv, i, f);
		break;
	}
	case 5u: // Grille: bilinear upscale, then RGB stripe triads whose depth
		// is the LCD Grid slider (0 = plain bilinear).
		c = bilinear(frag_uv, true);
		{
			ivec2 i;
			vec2 f;
			pixel_coord(frag_uv, i, f);
			const float strength = clamp(pc.grid, 0.0, 1.0);
			// strength 0 -> the untouched colour, 1 -> full triads.
			c = vec4(mix(c.rgb, grille(c, f), strength), c.a);
		}
		break;
	case 7u: // NTSC: single-pass composite approximation (see ntsc()).
		c = ntsc(frag_uv);
		break;
	default: // Nearest: a single fetch at the sampled coordinate.
		c = texture(gb_tex, frag_uv);
		break;
	}
	out_color = apply_grid(frag_uv, c);
}
