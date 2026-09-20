// GBA cartridge: image loading, header parsing, content-based detection.
// https://problemkaputt.de/gbatek-gba-cartridge-header.htm
// Validation requires the fixed byte 96h at 0x0B2 and the complement check
// at 0x0BD. Nintendo-logo bitmap verification is deferred to the PPU/test
// phase (logo lives in ROM data and is readable by software; it is not
// needed for core routing).

#include "gba/cartridge.h"
#include <fstream>

namespace gba {

bool Cartridge::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) return false;
    return loadFromBytes(data);
}

bool Cartridge::loadFromBytes(const std::vector<u8>& data) {
    if (!isGbaImage(data)) return false;
    rom_ = data;
    loaded_ = parseHeader();
    return loaded_;
}

void Cartridge::reset() {
    // Banking/save state resets here in later phases (Flash/EEPROM state);
    // the ROM image itself is immutable.
}

bool Cartridge::isGbaImage(const std::vector<u8>& data) {
    return isGbaImage(data.data(), data.size());
}

bool Cartridge::isGbaImage(const u8* data, size_t size) {
    if (data == nullptr) return false;
    if (size < kHeaderSize || size > kMaxRomSize) return false;
    if (data[kFixedOff] != kFixedValue) return false;
    if (calcChecksum(data, size) != data[kChecksumOff]) return false;
    return true;
}

u8 Cartridge::calcChecksum(const u8* data, size_t size) {
    // GBATEK: chk=0; for i=0A0h..0BCh: chk=chk-[i]; chk=(chk-19h).
    if (data == nullptr || size < kHeaderSize) return 0xFF;
    u8 chk = 0;
    for (u32 i = kTitleOff; i <= kVersionOff; i++) {
        chk = static_cast<u8>(chk - data[i]);
    }
    chk = static_cast<u8>(chk - 0x19);
    return chk;
}

bool Cartridge::parseHeader() {
    const u8* h = rom_.data();
    for (u32 i = 0; i < kTitleLen; i++) header_.title[i] = static_cast<char>(h[kTitleOff + i]);
    for (u32 i = 0; i < kGameCodeLen; i++) header_.game_code[i] = static_cast<char>(h[kGameCodeOff + i]);
    for (u32 i = 0; i < kMakerLen; i++) header_.maker_code[i] = static_cast<char>(h[kMakerOff + i]);
    header_.software_version = h[kVersionOff];
    header_.checksum = h[kChecksumOff];
    return true;
}

namespace {

std::string trimField(const char* data, size_t len) {
    std::string out;
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c == '\0') break;
        if (c < 0x20 || c > 0x7E) break;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

std::string Cartridge::title() const {
    return trimField(header_.title.data(), kTitleLen);
}

std::string Cartridge::gameCode() const {
    return trimField(header_.game_code.data(), kGameCodeLen);
}

} // namespace gba
