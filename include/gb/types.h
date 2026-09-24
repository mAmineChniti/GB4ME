#pragma once

#include <algorithm>

#include <array>
#include <cstddef>
#include <cstdint>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using f32 = float;
using f64 = double;

constexpr u16 SCREEN_WIDTH = 160;
constexpr u16 SCREEN_HEIGHT = 144;
constexpr u32 CYCLES_PER_FRAME = 70224;

// https://gbdev.io/pandocs/CGB_Registers.html
// GBC-Ready Rule 1: DMG clock speed is NOT a global used for timing logic.
// It is only the default used to initialise CPU::clock_speed_hz.
// All timing code MUST read the CPU member (which GBC will change via KEY1
// double-speed mode), never this constant directly.
// GBC: this will branch on double_speed mode (KEY1 register).
constexpr u32 kDefaultDmgClockHz = 4194304;

// GBC-Ready Rule 8: detect CGB mode at cartridge load time, even if unused.
// https://gbdev.io/pandocs/The_Cartridge_Header.html
// Header byte 0x0143: 0x80 = CGB-compatible (works on DMG + CGB),
// 0xC0 = CGB-only. Anything else = DMG-only.
// GBC: CGB_Compatible/CGB_Only will select CGB timing, palettes, HDMA, etc.
// Upscaling FILTER used when the emulated frame is stretched to the window.
// This is image QUALITY, not window size: the frame always fills the same
// area, but is resampled with a different kernel. Implemented in
// shaders/quad.frag. The frame is drawn at a fractional scale (it is fitted
// into a fractional bezel rect), so Nearest alone gives uneven pixel sizes
// and shimmer; the filtered modes fix that.
enum class VideoFilter : u8 {
	Nearest = 0,    // Crisp texels; the original behaviour (default).
	Bilinear,       // Linear interpolation (gamma-correct: the texture is sRGB).
	SmoothBilinear, // Bilinear with smoothstep weights (SameBoy SmoothBilinear).
	Scale2x,        // EPX: smooths diagonals, keeps axis-aligned edges crisp.
	Count,
};

// Plain float rectangle (the renderer's viewport math).
struct PixelRectF {
	f32 x, y, w, h;
};

// Letterboxes (x,y,w,h) to the source aspect ratio, centred, so the image is
// never stretched and never overflows the available area. This is the layout
// the renderer has always used; it is NOT an integer/whole-multiple scale.
// Pure geometry, so the selftest can verify the aspect property without a GPU.
inline PixelRectF fit_aspect_rect(f32 x, f32 y, f32 w, f32 h, u32 sw, u32 sh)
{
	if (sw == 0 || sh == 0 || w <= 0.0f || h <= 0.0f)
		return {x, y, w, h};
	const f32 fit = std::min(w / static_cast<f32>(sw), h / static_cast<f32>(sh));
	const f32 dw = static_cast<f32>(sw) * fit;
	const f32 dh = static_cast<f32>(sh) * fit;
	return {x + (w - dw) * 0.5f, y + (h - dh) * 0.5f, dw, dh};
}

// Human-readable filter name for the settings UI.
inline const char *video_filter_name(VideoFilter f)
{
	switch (f) {
	case VideoFilter::Nearest:
		return "Nearest";
	case VideoFilter::Bilinear:
		return "Bilinear";
	case VideoFilter::SmoothBilinear:
		return "Smooth Bilinear";
	case VideoFilter::Scale2x:
		return "Scale2x";
	case VideoFilter::Count:
		break;
	}
	return "Nearest";
}

enum class HardwareMode : u8 {
	DMG,
	CGB_Compatible,
	CGB_Only,
	// Alias for docs that name the revision CGB.
	CGB = CGB_Only,
};

inline bool is_cgb_mode(HardwareMode m)
{
	return m == HardwareMode::CGB_Compatible || m == HardwareMode::CGB_Only;
}

struct alignas(16) Vec2 {
	f32 x, y;
};

struct alignas(16) Vec3 {
	f32 x, y, z;
};

struct alignas(16) Vec4 {
	f32 x, y, z, w;
};

struct alignas(16) Mat4 {
	Vec4 rows[4];
};

inline u16 make_u16(u8 hi, u8 lo)
{
	return (static_cast<u16>(hi) << 8) | static_cast<u16>(lo);
}

inline u8 hi_byte(u16 val)
{
	return static_cast<u8>(val >> 8);
}

inline u8 lo_byte(u16 val)
{
	return static_cast<u8>(val & 0xFF);
}
