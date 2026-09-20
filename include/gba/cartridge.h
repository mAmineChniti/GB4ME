#pragma once

#include "gb/types.h"
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace gba {

// GBA cartridge header + ROM image.
// https://problemkaputt.de/gbatek-gba-cartridge-header.htm
// Kept independent from gb::Cartridge: the GBA header layout, validation
// (complement check) and save hardware differ completely from GB MBCs.
// Reuses only the hardware-independent integer aliases from gb/types.h.
class Cartridge {
public:
    static constexpr u32 kHeaderSize = 0xC0;
    static constexpr u32 kMaxRomSize = 32 * 1024 * 1024; // 256 Mbit.

    // Header offsets (GBATEK "GBA Cartridge Header").
    static constexpr u32 kEntryOff = 0x000;
    static constexpr u32 kTitleOff = 0x0A0;
    static constexpr u32 kTitleLen = 12;
    static constexpr u32 kGameCodeOff = 0x0AC;
    static constexpr u32 kGameCodeLen = 4;
    static constexpr u32 kMakerOff = 0x0B0;
    static constexpr u32 kMakerLen = 2;
    static constexpr u32 kFixedOff = 0x0B2; // Must be 96h ("required!").
    static constexpr u8 kFixedValue = 0x96;
    static constexpr u32 kVersionOff = 0x0BC;
    static constexpr u32 kChecksumOff = 0x0BD;

    struct Header {
        std::array<char, kTitleLen> title{};
        std::array<char, kGameCodeLen> game_code{};
        std::array<char, kMakerLen> maker_code{};
        u8 software_version = 0;
        u8 checksum = 0;
    };

    Cartridge() = default;

    bool load(const std::string& path);
    bool loadFromBytes(const std::vector<u8>& data);
    void reset();

    // Content-based detection: true when `data` looks like a GBA image
    // (size + fixed byte + complement check). Used by ROM-folder scanning
    // and by the frontend to route .gba files to the GBA core.
    static bool isGbaImage(const std::vector<u8>& data);
    static bool isGbaImage(const u8* data, size_t size);

    // GBATEK complement check: chk=0; for i=0A0h..0BCh: chk-=rom[i];
    // chk-=19h. The result must equal rom[0BDh].
    static u8 calcChecksum(const u8* data, size_t size);

    bool loaded() const { return loaded_; }
    const Header& header() const { return header_; }
    const std::vector<u8>& rom() const { return rom_; }
    u32 romSize() const { return static_cast<u32>(rom_.size()); }
    std::string title() const;
    std::string gameCode() const;

private:
    std::vector<u8> rom_;
    Header header_{};
    bool loaded_ = false;

    bool parseHeader();
};

} // namespace gba
