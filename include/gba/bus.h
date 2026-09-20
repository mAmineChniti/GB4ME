#pragma once

#include "gb/types.h"
#include <array>
#include <cstddef>

namespace gba {

// GBA memory bus (Phase 1 skeleton: map + RAM + protection, no devices yet).
// https://problemkaputt.de/gbatek.htm#gbamemorymap
// Independent from gb::MMU: 32-bit addresses, 8/16/32-bit accesses, and a
// completely different region layout. Shares only the design discipline
// (all accesses route through read/write; devices attach later).
class GbaBus {
public:
    // Region bases/sizes (GBATEK "GBA Memory Map").
    static constexpr u32 kBiosBase = 0x00000000, kBiosSize = 0x4000;
    static constexpr u32 kEwramBase = 0x02000000, kEwramSize = 0x40000; // 256K
    static constexpr u32 kIwramBase = 0x03000000, kIwramSize = 0x8000;  // 32K
    static constexpr u32 kIoBase = 0x04000000, kIoSize = 0x400;
    static constexpr u32 kPalBase = 0x05000000, kPalSize = 0x400;   // 1K
    static constexpr u32 kVramBase = 0x06000000, kVramSize = 0x18000; // 96K
    static constexpr u32 kOamBase = 0x07000000, kOamSize = 0x400;   // 1K
    static constexpr u32 kRomWs0Base = 0x08000000;
    static constexpr u32 kRomWs1Base = 0x0A000000;
    static constexpr u32 kRomWs2Base = 0x0C000000;
    static constexpr u32 kRomMirrorSize = 0x02000000; // 32M per wait-state slot.
    static constexpr u32 kSramBase = 0x0E000000, kSramSize = 0x10000; // 64K

    enum class Region : u8 {
        Bios,
        Ewram,
        Iwram,
        Io,
        Palette,
        Vram,
        Oam,
        Rom,
        Sram,
        Open, // Unmapped: open-bus reads (0xFF bytes, like gb::MMU unusable).
    };

    GbaBus();
    void reset();

    // ROM image owned by gba::Cartridge; bus holds a non-owning view.
    // Phase 9 replaces the SRAM path with real save hardware.
    void attachRom(const u8* data, size_t size);
    void detachRom();

    // BIOS protection (HLE BIOS, Phase 4): while the CPU executes outside
    // the BIOS region, BIOS reads return open-bus bytes and writes are
    // ignored. setBiosActive(true) is used by HLE startup/SWI entry.
    void setBiosActive(bool active) { bios_active_ = active; }
    bool biosActive() const { return bios_active_; }

    static Region decode(u32 addr);

    u8 read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    // Direct views for later devices/tests (PPU owns VRAM/PAL/OAM in
    // Phase 6; until then the bus holds the backing store).
    std::array<u8, kEwramSize>& ewram() { return ewram_; }
    std::array<u8, kIwramSize>& iwram() { return iwram_; }

private:
    std::array<u8, kEwramSize> ewram_{};
    std::array<u8, kIwramSize> iwram_{};
    std::array<u8, kIoSize> io_{};
    std::array<u8, kPalSize> pal_{};
    std::array<u8, kVramSize> vram_{};
    std::array<u8, kOamSize> oam_{};
    std::array<u8, kSramSize> sram_{};
    // HLE BIOS backing (Phase 4 installs vectors/handlers metadata here;
    // executable semantics arrive with the CPU). Zero until then.
    std::array<u8, kBiosSize> bios_{};

    const u8* rom_ = nullptr;
    size_t rom_size_ = 0;
    bool bios_active_ = false;

    u8 readRom(u32 addr) const;
};

} // namespace gba
