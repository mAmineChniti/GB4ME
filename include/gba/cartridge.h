#pragma once

#include "gba/gpio.h"
#include "gba/state.h"
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace gba
{

// Save hardware types (GBATEK "GBA Save Types").
enum class SaveType : u8 {
	None = 0,
	SRAM,
	Flash_Panasonic, // 64KB
	Flash_Sanyo,     // 64KB/128KB
	Flash_Macronix,  // 64KB/128KB
	Flash_Atmel,     // 64KB/128KB
	EEPROM_4Kbit,    // 512 bytes
	EEPROM_64Kbit,   // 8KB
	EEPROM_512Kbit,  // 64KB
};

// Forward declaration for save hardware interface.
class SaveHardware;

class Cartridge
{
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

	Cartridge();
	~Cartridge();

	Cartridge(const Cartridge &) = delete;
	Cartridge &operator=(const Cartridge &) = delete;

	bool load(const std::string &path);
	bool loadFromBytes(const std::vector<u8> &data);
	void reset();

	// Content-based detection: true when `data` looks like a GBA image
	// (size + fixed byte + complement check). Used by ROM-folder scanning
	// and by the frontend to route .gba files to the GBA core.
	static bool isGbaImage(const std::vector<u8> &data);
	static bool isGbaImage(const u8 *data, size_t size);

	// GBATEK complement check: chk=0; for i=0A0h..0BCh: chk-=rom[i];
	// chk-=19h. The result must equal rom[0BDh].
	static u8 calcChecksum(const u8 *data, size_t size);

	bool loaded() const
	{
		return loaded_;
	}
	const Header &header() const
	{
		return header_;
	}
	const std::vector<u8> &rom() const
	{
		return rom_;
	}
	u32 romSize() const
	{
		return static_cast<u32>(rom_.size());
	}
	std::string title() const;
	std::string gameCode() const;

	// Save hardware access (delegated to polymorphic implementation).
	u8 readSave(u32 addr) const;
	void writeSave(u32 addr, u8 value);
	// EEPROM serial engine (0x0D000000 DMA path). Valid only when
	// saveType() is an EEPROM variant; otherwise no-ops/returns ready.
	bool isEeprom() const;
	uint16_t eepromDmaRead() const;
	void eepromDmaWrite(uint16_t value, uint32_t remaining);
	// Cart GPIO + RTC (0x080000C4-0x080000C8, Pokemon R/S/E, Boktai, ...).
	// The bus routes that window here exactly when hasGpio() holds.
	bool hasGpio() const
	{
		return gpio_.present();
	}
	GpioRtc &gpio()
	{
		return gpio_;
	}
	const GpioRtc &gpio() const
	{
		return gpio_;
	}
	bool hasSave() const
	{
		return save_ != nullptr;
	}
	SaveType saveType() const
	{
		return save_type_;
	}
	u32 saveSize() const;

	// Persistence: save/load battery-backed data to/from file.
	void saveToFile(const std::string &path) const;
	void loadFromFile(const std::string &path);

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

  private:
	std::vector<u8> rom_;
	Header header_{};
	bool loaded_ = false;
	SaveType save_type_ = SaveType::None;
	std::unique_ptr<SaveHardware> save_;
	GpioRtc gpio_;

	void createSaveHardware();
	bool parseHeader();
	SaveType detectSaveType() const;
	static SaveType detectSaveTypeFromGameCode(const std::string &game_code);
	// Per-title forced save type (mGBA overrides.c), checked before markers.
	static SaveType forcedSaveType(const std::string &game_code);
	bool detectGpio() const;
};

} // namespace gba
