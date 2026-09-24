#pragma once

#include "gb/types.h"
#include "gba/cartridge.h"
#include <string>
#include <vector>

namespace gba
{

// Abstract save hardware interface (polymorphic, like GB MBC).
// All GBA save hardware maps to 0x0E000000-0x0EFFFFFF (SRAM region).
class SaveHardware
{
  public:
	virtual ~SaveHardware() = default;

	// Read/write at offset within save region (0 = 0x0E000000).
	virtual u8 read(u32 offset) const = 0;
	virtual void write(u32 offset, u8 value) = 0;

	// Total size in bytes.
	virtual u32 size() const = 0;

	// Save type identifier.
	virtual SaveType type() const = 0;

	// Persistence.
	virtual void saveToFile(const std::string &path) const = 0;
	virtual void loadFromFile(const std::string &path) = 0;

	// Save state serialization.
	virtual void save(StateBuffer &out) const = 0;
	virtual void load(const StateBuffer &in) = 0;

	// Reset hardware state (not data).
	virtual void reset() = 0;
};

// SRAM: 64KB at 0x0E000000, battery-backed, simple read/write.
// https://problemkaputt.de/gbatek.htm#gbasramsram
class SramSave : public SaveHardware
{
  public:
	explicit SramSave(u32 size_bytes = 64 * 1024);

	u8 read(u32 offset) const override;
	void write(u32 offset, u8 value) override;
	u32 size() const override
	{
		return static_cast<u32>(data_.size());
	}
	SaveType type() const override
	{
		return SaveType::SRAM;
	}
	void saveToFile(const std::string &path) const override;
	void loadFromFile(const std::string &path) override;
	void save(StateBuffer &out) const override;
	void load(const StateBuffer &in) override;
	void reset() override {}

  private:
	std::vector<u8> data_;
};

// Flash memory base class (command-driven).
// https://problemkaputt.de/gbatek.htm#gbaflashmemory
class FlashSave : public SaveHardware
{
  public:
	enum class Manufacturer : u8 {
		Unknown = 0,
		Panasonic, // MN63F805MNP - 64KB
		Sanyo,     // LE26FV10N1TS - 64KB/128KB
		Macronix,  // MX29L002/004 - 64KB/128KB
		Atmel,     // AT29LV512/010 - 64KB/128KB
	};

	FlashSave(Manufacturer mfr, u32 size_bytes);

	u8 read(u32 offset) const override;
	void write(u32 offset, u8 value) override;
	u32 size() const override
	{
		return static_cast<u32>(data_.size());
	}
	SaveType type() const override;
	void saveToFile(const std::string &path) const override;
	void loadFromFile(const std::string &path) override;
	void save(StateBuffer &out) const override;
	void load(const StateBuffer &in) override;
	void reset() override;

  protected:
	std::vector<u8> data_;
	Manufacturer mfr_;
	enum class State : u8 {
		ReadArray,
		Unlock1,       // After 0xAA at 0x5555
		Unlock2,       // After 0x55 at 0x2AAA
		ReadId,
		ReadStatus,
		EraseSetup,
		EraseConfirm,
		ProgramSetup,
		ProgramConfirm,
		SectorEraseSetup,
		SectorEraseConfirm,
		BankPending, // After B0 at 0x5555: next write at 0x0000 selects bank.
	} state_ = State::ReadArray;
	bool erase_suspended_ = false;
	u32 last_addr_ = 0;
	// Active 64KB bank base for 128KB chips (0 or 0x10000). mGBA parity:
	// all data accesses go through bank_base_ + (offset & 0xFFFF); the B0
	// switch command (Emerald 128KB saves use it) selects the bank.
	u32 bank_base_ = 0;
	u32 physAddr(u32 offset) const
	{
		return bank_base_ + (offset & 0xFFFFu);
	}

	// Command handling (to be implemented by specific manufacturers).
	virtual void handleCommand(u32 offset, u8 value) = 0;
	virtual u8 readId(u32 offset) const = 0;
	virtual u8 readStatus() const = 0;
};

// Panasonic MN63F805MNP (64KB).
class FlashPanasonic : public FlashSave
{
  public:
	explicit FlashPanasonic(u32 size_bytes = 64 * 1024);

  protected:
	void handleCommand(u32 offset, u8 value) override;
	u8 readId(u32 offset) const override;
	u8 readStatus() const override;
};

// Sanyo LE26FV10N1TS (64KB/128KB).
class FlashSanyo : public FlashSave
{
  public:
	explicit FlashSanyo(u32 size_bytes = 64 * 1024);

  protected:
	void handleCommand(u32 offset, u8 value) override;
	u8 readId(u32 offset) const override;
	u8 readStatus() const override;
};

// Macronix MX29L002/004 (64KB/128KB).
class FlashMacronix : public FlashSave
{
  public:
	explicit FlashMacronix(u32 size_bytes = 64 * 1024);

  protected:
	void handleCommand(u32 offset, u8 value) override;
	u8 readId(u32 offset) const override;
	u8 readStatus() const override;
};

// Atmel AT29LV512/010 (64KB/128KB).
class FlashAtmel : public FlashSave
{
  public:
	explicit FlashAtmel(u32 size_bytes = 64 * 1024);

  protected:
	void handleCommand(u32 offset, u8 value) override;
	u8 readId(u32 offset) const override;
	u8 readStatus() const override;
};

// EEPROM: DMA-driven serial bitstream interface.
// https://problemkaputt.de/gbatek.htm#gbaeeprom
// Hardware protocol (GBATEK "Using DMA"): bitstreams move through DMA3
// to/from 0x0D000000, one halfword per bit with only bit 0 significant.
// Write stream: "10" + n address bits (MSB first) + 64 data bits (MSB
// first) + stop "0". Read address stream: "11" + n address bits + "0",
// then 68 halfwords are read back (4 ignore bits + 64 data bits, MSB
// first). n is 6 (512B) or 14 (8KB); the engine below self-synchronizes
// on the transfer count so no width parameter is needed. K512bit is
// retained for API compatibility but no licensed title uses it (real
// EEPROM tops out at 8KB); it behaves like K64bit.
class EepromSave : public SaveHardware
{
  public:
	enum class Size : u8 {
		K4bit = 0, // 512 bytes
		K64bit,    // 8KB
		K512bit,   // 64KB (unlicensed/unused; protocol-identical)
	};

	explicit EepromSave(Size sz);

	// CPU-visible accessors (GBATEK: manual LDRH/STRH transfers do not
	// work on hardware; these exist only so detection probes and the
	// polled-ready read behave sanely). Serial state advances one bit.
	u8 read(u32 offset) const override;
	void write(u32 offset, u8 value) override;

	// DMA engine (the real path; called by the bus/DMA for 0x0D traffic).
	// dmaRead returns the next output bit in bit 0. dmaWrite consumes bit
	// 0 of value; `remaining` is the number of halfword transfers left
	// *including* the current one (mirrors the DMA unit counter, which is
	// how the engine tells address bits from data bits from the stop bit).
	uint16_t dmaRead() const;
	void dmaWrite(uint16_t value, uint32_t remaining);
	u32 size() const override
	{
		return static_cast<u32>(data_.size());
	}
	SaveType type() const override;
	void saveToFile(const std::string &path) const override;
	void loadFromFile(const std::string &path) override;
	void save(StateBuffer &out) const override;
	void load(const StateBuffer &in) override;
	void reset() override;

  private:
	enum class Cmd : u8 {
		Null = 0,       // Waiting for the leading 1 bit.
		Pending = 1,    // First bit seen; second selects read vs write.
		Write = 2,      // Receiving address + data bits ("10" seen).
		ReadPending = 3, // Receiving read address ("11" seen).
		Read = 4,       // Streaming the 68-bit readback.
	};
	// Lazy size upgrade: a 4Kbit backing transparently grows to 8KB the
	// first time the game addresses beyond 512 bytes (a 64Kbit title on
	// a 4Kbit initial guess, e.g. when the ROM-size heuristic guesses
	// low). Growth fills 0xFF (erased state), so unwritten high blocks
	// read back erased, exactly like a true 64Kbit chip. Mutable
	// alongside the rest of the serial engine (const bus/cartridge path).
	void ensureCapacity(u32 byte) const
	{
		if (byte < data_.size() || size_ != Size::K4bit)
			return;
		data_.resize(8 * 1024, 0xFF); // keep low 512B, erase-fill the rest
		size_ = Size::K64bit;
	}
	mutable std::vector<u8> data_;
	mutable Size size_ = Size::K4bit;
	// Serial reads mutate protocol state (as on hardware); the whole
	// engine is mutable so the const bus/cartridge read path can clock it.
	mutable Cmd cmd_ = Cmd::Null;
	// Bit offsets into data_ (byte = off>>3, MSB-first bit = 7-(off&7)).
	mutable u32 write_addr_ = 0;
	mutable u32 read_addr_ = 0;
	mutable int read_left_ = 0; // Bits left in the active 68-bit readback.
};

} // namespace gba