// GBA cartridge: image loading, header parsing, content-based detection.
// https://problemkaputt.de/gbatek-gba-cartridge-header.htm
// Validation requires the fixed byte 96h at 0x0B2 and the complement check
// at 0x0BD. Nintendo-logo bitmap verification is deferred to the PPU/test
// phase (logo lives in ROM data and is readable by software; it is not
// needed for core routing).
// Save hardware: SRAM, Flash (4 vendors), EEPROM (3 sizes).

#include "gba/cartridge.h"
#include "gba/save_hardware.h"
#include <cctype>
#include <fstream>

namespace gba
{

Cartridge::Cartridge() = default;

Cartridge::~Cartridge() = default;

bool Cartridge::load(const std::string &path)
{
	// Cap the read before allocating so a huge or sparse file cannot throw
	// bad_alloc and terminate the process. 64 MiB is well above the 32 MiB
	// maximum for a real GBA cartridge.
	constexpr std::streamsize kMaxRomBytes = 64 * 1024 * 1024;
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
		return false;
	std::streamsize size = file.tellg();
	if (size <= 0 || size > kMaxRomBytes)
		return false;
	file.seekg(0, std::ios::beg);
	std::vector<u8> data(static_cast<size_t>(size));
	if (!file.read(reinterpret_cast<char *>(data.data()), size))
		return false;
	return loadFromBytes(data);
}

bool Cartridge::loadFromBytes(const std::vector<u8> &data)
{
	if (!isGbaImage(data))
		return false;
	rom_ = data;
	loaded_ = parseHeader();
	if (loaded_) {
		save_type_ = detectSaveType();
		createSaveHardware();
		gpio_.reset();
		gpio_.setPresent(detectGpio());
	}
	return loaded_;
}

void Cartridge::createSaveHardware()
{
	// Detect Flash size from ROM marker (128KB for FLASH1M_*, else 64KB).
	const std::string rom_str(reinterpret_cast<const char *>(rom_.data()), rom_.size());
	const bool is_flash_128k = rom_str.find("FLASH1M_V") != std::string::npos ||
							   rom_str.find("FLASH1M_") != std::string::npos;
	const u32 flash_size = is_flash_128k ? (128 * 1024) : (64 * 1024);

	switch (save_type_) {
	case SaveType::SRAM:
		save_ = std::make_unique<SramSave>(64 * 1024);
		break;
	case SaveType::Flash_Panasonic:
		save_ = std::make_unique<FlashPanasonic>(flash_size);
		break;
	case SaveType::Flash_Sanyo:
		save_ = std::make_unique<FlashSanyo>(flash_size);
		break;
	case SaveType::Flash_Macronix:
		save_ = std::make_unique<FlashMacronix>(flash_size);
		break;
	case SaveType::Flash_Atmel:
		save_ = std::make_unique<FlashAtmel>(flash_size);
		break;
	case SaveType::EEPROM_4Kbit:
		save_ = std::make_unique<EepromSave>(EepromSave::Size::K4bit);
		break;
	case SaveType::EEPROM_64Kbit:
		save_ = std::make_unique<EepromSave>(EepromSave::Size::K64bit);
		break;
	case SaveType::EEPROM_512Kbit:
		save_ = std::make_unique<EepromSave>(EepromSave::Size::K512bit);
		break;
	default:
		save_ = nullptr;
		break;
	}
	// Battery file loading stays with the core (it owns the ROM path).
}

void Cartridge::reset()
{
	if (save_)
		save_->reset();
	const bool present = gpio_.present();
	gpio_.reset();
	gpio_.setPresent(present);
}

// RTC-fitted cartridges, by 3-letter game-code prefix (mGBA overrides:
// BPE/AXV/AXP = Pokemon R/S/E, U3I/U32 = Boktai 1/2 + solar, BLJ/BLV =
// Rockman EXE 4.5). Only the RTC is modelled (solar/gyro/rumble are not).
bool Cartridge::detectGpio() const
{
	const std::string code = gameCode();
	if (code.size() < 3)
		return false;
	const std::string prefix = code.substr(0, 3);
	return prefix == "BPE" || prefix == "AXV" || prefix == "AXP" || prefix == "U3I" ||
		   prefix == "U32" || prefix == "BLJ" || prefix == "BLV";
}

bool Cartridge::isGbaImage(const std::vector<u8> &data)
{
	return isGbaImage(data.data(), data.size());
}

bool Cartridge::isGbaImage(const u8 *data, size_t size)
{
	if (data == nullptr)
		return false;
	if (size < kHeaderSize || size > kMaxRomSize)
		return false;
	if (data[kFixedOff] != kFixedValue)
		return false;
	if (calcChecksum(data, size) != data[kChecksumOff])
		return false;
	return true;
}

u8 Cartridge::calcChecksum(const u8 *data, size_t size)
{
	// GBATEK: chk=0; for i=0A0h..0BCh: chk=chk-[i]; chk=(chk-19h).
	if (data == nullptr || size < kHeaderSize)
		return 0xFF;
	u8 chk = 0;
	for (u32 i = kTitleOff; i <= kVersionOff; i++) {
		chk = static_cast<u8>(chk - data[i]);
	}
	chk = static_cast<u8>(chk - 0x19);
	return chk;
}

bool Cartridge::parseHeader()
{
	const u8 *h = rom_.data();
	for (u32 i = 0; i < kTitleLen; i++)
		header_.title[i] = static_cast<char>(h[kTitleOff + i]);
	for (u32 i = 0; i < kGameCodeLen; i++)
		header_.game_code[i] = static_cast<char>(h[kGameCodeOff + i]);
	for (u32 i = 0; i < kMakerLen; i++)
		header_.maker_code[i] = static_cast<char>(h[kMakerOff + i]);
	header_.software_version = h[kVersionOff];
	header_.checksum = h[kChecksumOff];
	return true;
}

namespace
{

std::string trimField(const char *data, size_t len)
{
	std::string out;
	for (size_t i = 0; i < len; i++) {
		const char c = data[i];
		if (c == '\0')
			break;
		if (c < 0x20 || c > 0x7E)
			break;
		out.push_back(c);
	}
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out;
}

} // namespace

std::string Cartridge::title() const
{
	return trimField(header_.title.data(), kTitleLen);
}

std::string Cartridge::gameCode() const
{
	return trimField(header_.game_code.data(), kGameCodeLen);
}

u8 Cartridge::readSave(u32 addr) const
{
	if (!save_)
		return 0xFF;
	return save_->read(addr);
}

void Cartridge::writeSave(u32 addr, u8 value)
{
	if (!save_)
		return;
	save_->write(addr, value);
}

bool Cartridge::isEeprom() const
{
	return save_type_ == SaveType::EEPROM_4Kbit || save_type_ == SaveType::EEPROM_64Kbit ||
		   save_type_ == SaveType::EEPROM_512Kbit;
}

uint16_t Cartridge::eepromDmaRead() const
{
	if (!isEeprom())
		return 1;
	if (auto *e = dynamic_cast<const EepromSave *>(save_.get()))
		return e->dmaRead();
	return 1;
}

void Cartridge::eepromDmaWrite(uint16_t value, uint32_t remaining)
{
	if (!isEeprom())
		return;
	if (auto *e = dynamic_cast<EepromSave *>(save_.get()))
		e->dmaWrite(value, remaining);
}

u32 Cartridge::saveSize() const
{
	if (!save_)
		return 0;
	return save_->size();
}

void Cartridge::saveToFile(const std::string &path) const
{
	if (!save_)
		return;
	save_->saveToFile(path);
}

void Cartridge::loadFromFile(const std::string &path)
{
	if (!save_)
		return;
	save_->loadFromFile(path);
}

SaveType Cartridge::detectSaveType() const
{
	// Industry standard: scan ROM for ASCII marker strings like mGBA/VBA-M.
	// Markers: "EEPROM_Vnnn", "FLASH_V123", "FLASH512_Vnnn", "FLASH1M_Vnnn", "SRAM_Vnnn"
	
	// Search for marker strings in ROM
	const std::string rom_str(reinterpret_cast<const char*>(rom_.data()), rom_.size());
	
	// Per-title overrides FIRST (mGBA src/gba/overrides.c). Some carts ship
	// a marker that contradicts the real chip, so mGBA forces the type for
	// them; without the table those titles silently get a wrong-sized save.
	// Marker detection stays the fallback for everything not listed.
	{
		const std::string gcode = gameCode();
		if (!gcode.empty()) {
			const SaveType forced = forcedSaveType(gcode);
			if (forced != SaveType::None)
				return forced;
		}
	}

	// Check for EEPROM markers
	if (rom_str.find("EEPROM_V") != std::string::npos) {
		// Initial size is a guess only: the EEPROM engine transparently
		// upgrades a 4Kbit backing to 8KB on first out-of-range access,
		// so a wrong guess here self-corrects without data loss. Default
		// 4Kbit (mGBA's autodetect default; most EEPROM titles are 4Kbit).
		return SaveType::EEPROM_4Kbit;
	}
	
	// Check for Flash1M (128KB Flash)
	if (rom_str.find("FLASH1M_V") != std::string::npos ||
	    rom_str.find("FLASH1M_") != std::string::npos) {
		// 128KB Flash - use Macronix (most common)
		// Note: would need to instantiate FlashMacronix with 128KB size
		return SaveType::Flash_Macronix; // Caller needs to handle 128KB
	}
	
	// Check for Flash512 or generic FLASH_V (64KB Flash)
	if (rom_str.find("FLASH512_V") != std::string::npos ||
	    rom_str.find("FLASH_V") != std::string::npos ||
	    rom_str.find("FLASH_") != std::string::npos) {
		return SaveType::Flash_Macronix; // 64KB
	}
	
	// Check for SRAM marker
	if (rom_str.find("SRAM_V") != std::string::npos ||
	    rom_str.find("SRAM_") != std::string::npos) {
		return SaveType::SRAM;
	}
	
	// Fallback: try game code heuristic for games without markers
	std::string gcode = gameCode();
	if (!gcode.empty()) {
		SaveType t = detectSaveTypeFromGameCode(gcode);
		if (t != SaveType::None)
			return t;
	}
	
	// Default to SRAM (most common, largest compatibility)
	return SaveType::SRAM;
}

// Titles where the ROM marker lies, taken from mGBA's override table
// (docs/references/mgba/src/gba/overrides.c). These are checked BEFORE
// marker scanning.
SaveType Cartridge::forcedSaveType(const std::string &code)
{
	struct Row {
		const char *code;
		SaveType type;
	};
	// mGBA forces GBA_SAVEDATA_EEPROM here; the EEPROM engine self-corrects
	// its size on first out-of-range access, so 4Kbit is the right default.
	static constexpr Row kOverrides[] = {
		// DigiCommunication Nyo - marker says SRAM_F_V103, chip is EEPROM.
		{ "BDKJ", SaveType::EEPROM_4Kbit },
		// Crash Bandicoot 2, Boktai series, etc. - no EEPROM marker at all.
		{ "AC8J", SaveType::EEPROM_4Kbit },
		{ "AC8E", SaveType::EEPROM_4Kbit },
		{ "AC8P", SaveType::EEPROM_4Kbit },
		{ "U3IJ", SaveType::EEPROM_4Kbit },
		{ "U3IE", SaveType::EEPROM_4Kbit },
		{ "U3IP", SaveType::EEPROM_4Kbit },
		{ "U32J", SaveType::EEPROM_4Kbit },
		{ "U32E", SaveType::EEPROM_4Kbit },
		{ "U32P", SaveType::EEPROM_4Kbit },
		{ "U33J", SaveType::EEPROM_4Kbit },
		{ "KHPJ", SaveType::EEPROM_4Kbit },
		// No battery-backed save at all (mGBA GBA_SAVEDATA_NONE).
		{ "AI2E", SaveType::None },
		{ "AI2P", SaveType::None },
		{ "ASLE", SaveType::None },
		{ "ASLF", SaveType::None },
		{ "A2YE", SaveType::None },
	};
	for (const Row &r : kOverrides)
		if (code == r.code)
			return r.type;
	return SaveType::None;
}

SaveType Cartridge::detectSaveTypeFromGameCode(const std::string &game_code)
{
	// Heuristics based on known game codes (from GBATEK/mGBA data)
	// Format: XXXX where first two = maker, last two = game
	// This is a partial list; real detection would use a database.

	// Flash games (known game codes)
	static const std::vector<std::string> flash_games = {
		// Panasonic
		"BPRE",
		"BPRE", // Pokemon Emerald (US) - actually uses Flash
		// Sanyo
		"BPRJ",
		"BPRJ", // Pokemon Ruby (JP)
		// Macronix
		"AXVE", // Golden Sun (US)
		"AXPJ", // Golden Sun (JP)
		// Atmel
		"A3ME", // Metroid Fusion (US)
	};

	for (const auto &code : flash_games) {
		if (game_code == code)
			return SaveType::Flash_Macronix; // Most common
	}

	// EEPROM games (mostly Japanese early titles)
	static const std::vector<std::string> eeprom_4k_games = {
		"B2MJ", // Mario Kart Super Circuit (JP)
		"A4MJ", // F-Zero (JP)
	};
	static const std::vector<std::string> eeprom_64k_games = {
		"AMSJ", // Advance Wars (JP)
	};

	for (const auto &code : eeprom_4k_games) {
		if (game_code == code)
			return SaveType::EEPROM_4Kbit;
	}
	for (const auto &code : eeprom_64k_games) {
		if (game_code == code)
			return SaveType::EEPROM_64Kbit;
	}

	return SaveType::None; // Unknown - default to SRAM in detectSaveType()
}

void Cartridge::save(StateBuffer &out) const
{
	out.writeVector(rom_);
	out.write(header_.title);
	out.write(header_.game_code);
	out.write(header_.maker_code);
	out.write(header_.software_version);
	out.write(header_.checksum);
	out.write(loaded_);
	out.write(save_type_);
	if (save_) {
		u8 has = 1;
		out.write(has);
		save_->save(out);
	} else {
		u8 has = 0;
		out.write(has);
	}
	gpio_.save(out);
}

void Cartridge::load(const StateBuffer &in)
{
	in.readVector(rom_);
	in.read(header_.title);
	in.read(header_.game_code);
	in.read(header_.maker_code);
	in.read(header_.software_version);
	in.read(header_.checksum);
	in.read(loaded_);
	in.read(save_type_);
	u8 has = 0;
	in.read(has);
	// Recreate the hardware from the serialized type (C4: the old code
	// only loaded when save_ was already non-null, silently dropping
	// save state on fresh loads).
	createSaveHardware();
	if (has && save_) {
		save_->load(in);
	}
	// GPIO/RTC state is a v2 addition; v1 states simply keep the fresh
	// reset below (failed reads are ignored, not fatal).
	gpio_.reset();
	gpio_.setPresent(detectGpio());
	gpio_.load(in);
}

} // namespace gba
