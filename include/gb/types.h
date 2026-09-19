#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using f32 = float;
using f64 = double;

constexpr u16 SCREEN_WIDTH  = 160;
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
enum class HardwareMode : u8 {
    DMG,
    CGB_Compatible,
    CGB_Only,
    // Alias for docs that name the revision CGB.
    CGB = CGB_Only,
};

inline bool is_cgb_mode(HardwareMode m) {
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

inline u16 make_u16(u8 hi, u8 lo) {
    return (static_cast<u16>(hi) << 8) | static_cast<u16>(lo);
}

inline u8 hi_byte(u16 val) {
    return static_cast<u8>(val >> 8);
}

inline u8 lo_byte(u16 val) {
    return static_cast<u8>(val & 0xFF);
}
