// GBA bus skeleton: region decode, RAM backing, ROM mirror, protection.
// https://problemkaputt.de/gbatek.htm#gbamemorymap
// Mirroring follows GBATEK (EWRAM/IWRAM/PAL/VRAM/OAM mirrors). I/O register
// behavior, VRAM banking rules and EEPROM-in-WS2 handling arrive with their
// owning devices (Phases 4-6, 9); until then: I/O is plain backing store,
// EEPROM area reads through the ROM mirror.
// Misaligned 16/32-bit accesses are byte-assembled little-endian for now;
// rotation semantics are pinned with the CPU in Phase 2.

#include "gba/bus.h"

namespace gba {

GbaBus::GbaBus() {
    reset();
}

void GbaBus::reset() {
    ewram_.fill(0);
    iwram_.fill(0);
    io_.fill(0);
    pal_.fill(0);
    vram_.fill(0);
    oam_.fill(0);
    sram_.fill(0xFF); // Battery-backed RAM powers up erased (0xFF).
    bios_.fill(0);
    bios_active_ = false;
    // ROM attachment survives reset (cartridge stays inserted).
}

void GbaBus::attachRom(const u8* data, size_t size) {
    rom_ = data;
    rom_size_ = size;
}

void GbaBus::detachRom() {
    rom_ = nullptr;
    rom_size_ = 0;
}

GbaBus::Region GbaBus::decode(u32 addr) {
    const u32 hi = addr >> 24;
    switch (hi) {
        case 0x00: return addr < kBiosSize ? Region::Bios : Region::Open;
        case 0x02: return Region::Ewram;
        case 0x03: return Region::Iwram;
        case 0x04: return addr - kIoBase < kIoSize ? Region::Io : Region::Open;
        case 0x05: return Region::Palette;
        case 0x06: return Region::Vram;
        case 0x07: return Region::Oam;
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
            return Region::Rom;
        case 0x0E: case 0x0F: return Region::Sram;
        default: return Region::Open;
    }
}

u8 GbaBus::readRom(u32 addr) const {
    if (rom_ == nullptr || rom_size_ == 0) return 0xFF;
    // Each 32M wait-state slot mirrors the image; non-power-of-two images
    // wrap by modulo (hardware mirrors by address-line truncation).
    const u32 off = (addr & (kRomMirrorSize - 1)) % static_cast<u32>(rom_size_);
    return rom_[off];
}

u8 GbaBus::read8(u32 addr) {
    switch (decode(addr)) {
        case Region::Bios: {
            if (!bios_active_) return 0xFF; // BIOS protection (Phase 4 pins exact value).
            return bios_[addr & (kBiosSize - 1)];
        }
        case Region::Ewram: return ewram_[addr & (kEwramSize - 1)];
        case Region::Iwram: return iwram_[addr & (kIwramSize - 1)];
        case Region::Io: return io_[addr - kIoBase];
        case Region::Palette: return pal_[addr & (kPalSize - 1)];
        case Region::Vram: return vram_[addr & (kVramSize - 1)];
        case Region::Oam: return oam_[addr & (kOamSize - 1)];
        case Region::Rom: return readRom(addr);
        case Region::Sram: {
            const u32 off = addr & (kSramSize - 1);
            return sram_[off];
        }
        case Region::Open: return 0xFF;
    }
    return 0xFF;
}

u16 GbaBus::read16(u32 addr) {
    const u8 lo = read8(addr);
    const u8 hi = read8(addr + 1);
    return static_cast<u16>(lo | (static_cast<u16>(hi) << 8));
}

u32 GbaBus::read32(u32 addr) {
    const u16 lo = read16(addr);
    const u16 hi = read16(addr + 2);
    return lo | (static_cast<u32>(hi) << 16);
}

void GbaBus::write8(u32 addr, u8 value) {
    switch (decode(addr)) {
        case Region::Bios: return; // Never writable (HLE installs via reset path).
        case Region::Ewram: ewram_[addr & (kEwramSize - 1)] = value; return;
        case Region::Iwram: iwram_[addr & (kIwramSize - 1)] = value; return;
        case Region::Io:
            io_[addr - kIoBase] = value;
            return;
        case Region::Palette: pal_[addr & (kPalSize - 1)] = value; return;
        case Region::Vram: vram_[addr & (kVramSize - 1)] = value; return;
        case Region::Oam: oam_[addr & (kOamSize - 1)] = value; return;
        case Region::Rom: return; // ROM not writable (Flash cmds: Phase 9).
        case Region::Sram:
            sram_[addr & (kSramSize - 1)] = value;
            return;
        case Region::Open: return;
    }
}

void GbaBus::write16(u32 addr, u16 value) {
    write8(addr, static_cast<u8>(value & 0xFF));
    write8(addr + 1, static_cast<u8>(value >> 8));
}

void GbaBus::write32(u32 addr, u32 value) {
    write16(addr, static_cast<u16>(value & 0xFFFF));
    write16(addr + 2, static_cast<u16>(value >> 16));
}

} // namespace gba
