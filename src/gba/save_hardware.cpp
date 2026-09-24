// GBA save hardware implementations.

#include "gba/save_hardware.h"
#include "gba/cartridge.h"
#include "gba/state.h"
#include <algorithm>
#include <fstream>

namespace gba
{

// ============================================================================
// SRAM
// ============================================================================

SramSave::SramSave(u32 size_bytes) : data_(size_bytes, 0xFF) {}

u8 SramSave::read(u32 offset) const
{
	if (offset >= data_.size())
		return 0xFF;
	return data_[offset];
}

void SramSave::write(u32 offset, u8 value)
{
	if (offset < data_.size())
		data_[offset] = value;
}

void SramSave::saveToFile(const std::string &path) const
{
	std::ofstream file(path, std::ios::binary);
	if (!file)
		return;
	file.write(reinterpret_cast<const char *>(data_.data()), data_.size());
}

void SramSave::loadFromFile(const std::string &path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
		return;
	std::streamsize size = file.tellg();
	if (size <= 0)
		return;
	file.seekg(0, std::ios::beg);
	std::vector<u8> data(std::min<size_t>(static_cast<size_t>(size), data_.size()));
	file.read(reinterpret_cast<char *>(data.data()), data.size());
	std::copy(data.begin(), data.end(), data_.begin());
}

void SramSave::save(StateBuffer &out) const
{
	out.writeVector(data_);
}

void SramSave::load(const StateBuffer &in)
{
	in.readVector(data_);
}

// ============================================================================
// Flash Base
// ============================================================================

FlashSave::FlashSave(Manufacturer mfr, u32 size_bytes) : data_(size_bytes, 0xFF), mfr_(mfr) {}

u8 FlashSave::read(u32 offset) const
{
	const u32 addr = physAddr(offset);
	if (addr >= data_.size())
		return 0xFF;

	switch (state_) {
	case State::ReadArray:
		return data_[addr];
	case State::ReadId:
		return readId(offset);
	case State::ReadStatus:
		return readStatus();
	default:
		return data_[addr];
	}
}

void FlashSave::write(u32 offset, u8 value)
{
	if (offset >= data_.size())
		return;

	// All commands are written to specific addresses (typically 0x5555, 0x2AAA, 0x0000)
	// but the GBA maps flash to 0x0E000000, so the offsets are relative.
	// Games use the full 32-bit address; we use the lower bits.
	const u32 addr = offset & 0xFFFF;

	// Track state transitions based on command sequences.
	// Correct sequence: 0xAA at 0x5555 → Unlock1, 0x55 at 0x2AAA → Unlock2, then command at 0x5555.
	switch (state_) {
	case State::ReadArray: {
		if (addr == 0x5555 && value == 0xAA) {
			state_ = State::Unlock1;
		} else if (addr == 0x5555 && value == 0xF0) {
			// Reset to read array
			state_ = State::ReadArray;
		}
		break;
	}
	case State::Unlock1: {
		if (addr == 0x2AAA && value == 0x55) {
			state_ = State::Unlock2;
		} else {
			state_ = State::ReadArray;
		}
		break;
	}
	case State::Unlock2: {
		if (addr == 0x5555) {
			if (value == 0x90) {
				state_ = State::ReadId;
			} else if (value == 0xF0) {
				state_ = State::ReadArray;
			} else if (value == 0x80) {
				state_ = State::EraseSetup;
			} else if (value == 0xA0) {
				state_ = State::ProgramSetup;
			} else if (value == 0xB0) {
				state_ = State::BankPending; // 128KB bank switch.
			} else {
				state_ = State::ReadArray;
			}
		} else {
			state_ = State::ReadArray;
		}
		break;
	}
	case State::BankPending: {
		// Bank number arrives at address 0 (pokeemerald SwitchFlashBank).
		// Only 128KB chips have a second bank; anything else is ignored.
		if (addr == 0 && value < 2 && (static_cast<u32>(value) << 16) < data_.size())
			bank_base_ = static_cast<u32>(value) << 16;
		state_ = State::ReadArray;
		break;
	}
	case State::ReadId: {
		if (addr == 0x5555 && value == 0xF0) {
			state_ = State::ReadArray;
		} else if (addr == 0x5555 && value == 0xAA) {
			state_ = State::Unlock1;
		}
		break;
	}
	case State::EraseSetup: {
		if (addr == 0x5555 && value == 0xAA) {
			state_ = State::EraseConfirm;
		} else {
			state_ = State::ReadArray;
		}
		break;
	}
	case State::EraseConfirm: {
		if (addr == 0x2AAA && value == 0x55) {
			// Erase confirm, ready for sector/chip erase command
			handleCommand(addr, value);
			state_ = State::ReadArray;
		} else if ((addr & 0xFFF) == 0x000) {
			// Sector erase command
			handleCommand(addr, value);
			state_ = State::ReadArray;
		} else {
			state_ = State::ReadArray;
		}
		break;
	}
	case State::ProgramSetup: {
		// After 0xA0, next write at any address programs that byte
		handleCommand(addr, value);
		state_ = State::ReadArray;
		break;
	}
	case State::ProgramConfirm: {
		handleCommand(addr, value);
		state_ = State::ReadArray;
		break;
	}
	default:
		state_ = State::ReadArray;
		break;
	}
}

SaveType FlashSave::type() const
{
	switch (mfr_) {
	case Manufacturer::Panasonic:
		return SaveType::Flash_Panasonic;
	case Manufacturer::Sanyo:
		return SaveType::Flash_Sanyo;
	case Manufacturer::Macronix:
		return SaveType::Flash_Macronix;
	case Manufacturer::Atmel:
		return SaveType::Flash_Atmel;
	default:
		return SaveType::None;
	}
}

void FlashSave::saveToFile(const std::string &path) const
{
	std::ofstream file(path, std::ios::binary);
	if (!file)
		return;
	file.write(reinterpret_cast<const char *>(data_.data()), data_.size());
}

void FlashSave::loadFromFile(const std::string &path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
		return;
	std::streamsize size = file.tellg();
	if (size <= 0)
		return;
	file.seekg(0, std::ios::beg);
	std::vector<u8> data(std::min<size_t>(static_cast<size_t>(size), data_.size()));
	file.read(reinterpret_cast<char *>(data.data()), data.size());
	std::copy(data.begin(), data.end(), data_.begin());
}

void FlashSave::reset()
{
	state_ = State::ReadArray;
	erase_suspended_ = false;
	bank_base_ = 0;
}

void FlashSave::save(StateBuffer &out) const
{
	out.write(static_cast<u8>(state_));
	out.write(erase_suspended_);
	out.write(last_addr_);
	out.write(bank_base_);
	out.writeVector(data_);
}

void FlashSave::load(const StateBuffer &in)
{
	u8 s = 0;
	in.read(s);
	state_ = static_cast<State>(s);
	in.read(erase_suspended_);
	in.read(last_addr_);
	// bank_base_ is new in the stream; default 0 when loading older states.
	if (!in.read(bank_base_))
		bank_base_ = 0;
	in.readVector(data_);
}

// ============================================================================
// Flash Panasonic MN63F805MNP (64KB)
// ============================================================================

FlashPanasonic::FlashPanasonic(u32 size_bytes) : FlashSave(Manufacturer::Panasonic, size_bytes) {}

void FlashPanasonic::handleCommand(u32 offset, u8 value)
{
	const u32 addr = offset & 0xFFFF;
	const u32 eff = physAddr(offset); // Bank-mapped data address (128KB chips).
	if (addr == 0x10) { // Sector erase command
		// Erase 4KB sector
		u32 sector = (eff >> 12) * 4096;
		for (u32 i = 0; i < 4096 && (sector + i) < data_.size(); i++) {
			data_[sector + i] = 0xFF;
		}
	} else if (addr == 0x30) { // Chip erase (Panasonic uses 0x10/0x30 differently)
		// Chip erase - erase all
		std::fill(data_.begin(), data_.end(), 0xFF);
	} else {
		// Byte program
		if (eff < data_.size()) {
			data_[eff] = value;
		}
	}
}

u8 FlashPanasonic::readId(u32 offset) const
{
	// Panasonic ID: Manufacturer=0x32, Device=0x1B (for MN63F805MNP)
	if (offset == 0)
		return 0x32;
	if (offset == 1)
		return 0x1B;
	return 0xFF;
}

u8 FlashPanasonic::readStatus() const
{
	return 0x80; // Ready
}

// ============================================================================
// Flash Sanyo LE26FV10N1TS (64KB/128KB)
// ============================================================================

FlashSanyo::FlashSanyo(u32 size_bytes) : FlashSave(Manufacturer::Sanyo, size_bytes) {}

void FlashSanyo::handleCommand(u32 offset, u8 value)
{
	const u32 addr = offset & 0xFFFF;
	const u32 eff = physAddr(offset); // Bank-mapped data address (128KB chips).
	if (addr == 0x2AAA && value == 0x80) {
		state_ = State::SectorEraseSetup;
	} else if (state_ == State::SectorEraseSetup && value == 0x30) {
		// Sector erase
		u32 sector = (eff >> 12) * 4096;
		for (u32 i = 0; i < 4096 && (sector + i) < data_.size(); i++) {
			data_[sector + i] = 0xFF;
		}
		state_ = State::ReadArray;
	} else if (addr == 0x5555 && value == 0x10) {
		// Chip erase
		std::fill(data_.begin(), data_.end(), 0xFF);
	} else {
		// Byte program
		if (eff < data_.size()) {
			data_[eff] = value;
		}
	}
}

u8 FlashSanyo::readId(u32 offset) const
{
	// Sanyo ID: Manufacturer=0x62, Device=0x13 (LE26FV10N1TS)
	if (offset == 0)
		return 0x62;
	if (offset == 1)
		return 0x13;
	return 0xFF;
}

u8 FlashSanyo::readStatus() const
{
	return 0x80; // Ready
}

// ============================================================================
// Flash Macronix MX29L002/004 (64KB/128KB)
// ============================================================================

FlashMacronix::FlashMacronix(u32 size_bytes) : FlashSave(Manufacturer::Macronix, size_bytes) {}

void FlashMacronix::handleCommand(u32 offset, u8 value)
{
	const u32 addr = offset & 0xFFFF;
	const u32 eff = physAddr(offset); // Bank-mapped data address (128KB chips).
	if (addr == 0x2AAA && value == 0x80) {
		state_ = State::SectorEraseSetup;
	} else if (state_ == State::SectorEraseSetup && value == 0x30) {
		u32 sector = (eff >> 12) * 4096;
		for (u32 i = 0; i < 4096 && (sector + i) < data_.size(); i++) {
			data_[sector + i] = 0xFF;
		}
		state_ = State::ReadArray;
	} else if (addr == 0x5555 && value == 0x10) {
		std::fill(data_.begin(), data_.end(), 0xFF);
	} else {
		if (eff < data_.size()) {
			data_[eff] = value;
		}
	}
}

u8 FlashMacronix::readId(u32 offset) const
{
	// Macronix IDs (GBATEK "Device Types", MSB=device, LSB=maker):
	// 64KB MX29L002 = 1CC2h, 128KB MX29L010 = 09C2h (the latter is what
	// Pokemon Emerald's IdentifyFlash expects; the old code returned the
	// Sanyo device byte, so no ID ever matched and the game stalled with
	// gFlashMemoryPresent=FALSE).
	if (offset == 0)
		return 0xC2;
	if (offset == 1)
		return data_.size() >= 128 * 1024 ? 0x09 : 0x1C;
	return 0xFF;
}

u8 FlashMacronix::readStatus() const
{
	return 0x80;
}

// ============================================================================
// Flash Atmel AT29LV512/010 (64KB/128KB)
// ============================================================================

FlashAtmel::FlashAtmel(u32 size_bytes) : FlashSave(Manufacturer::Atmel, size_bytes) {}

void FlashAtmel::handleCommand(u32 offset, u8 value)
{
	const u32 addr = offset & 0xFFFF;
	const u32 eff = physAddr(offset); // Bank-mapped data address (128KB chips).
	if (addr == 0x2AAA && value == 0x80) {
		state_ = State::SectorEraseSetup;
	} else if (state_ == State::SectorEraseSetup && value == 0x30) {
		u32 sector = (eff >> 12) * 4096;
		for (u32 i = 0; i < 4096 && (sector + i) < data_.size(); i++) {
			data_[sector + i] = 0xFF;
		}
		state_ = State::ReadArray;
	} else if (addr == 0x5555 && value == 0x10) {
		std::fill(data_.begin(), data_.end(), 0xFF);
	} else {
		if (eff < data_.size()) {
			data_[eff] = value;
		}
	}
}

u8 FlashAtmel::readId(u32 offset) const
{
	// Atmel IDs (GBATEK "Device Types"): 64KB AT29LV512 = 3D1Fh.
	// 128KB keeps the previously used 0x14 (AT29LV010).
	if (offset == 0)
		return 0x1F;
	if (offset == 1)
		return data_.size() >= 128 * 1024 ? 0x14 : 0x3D;
	return 0xFF;
}

u8 FlashAtmel::readStatus() const
{
	return 0x80;
}

// ============================================================================
// EEPROM
// ============================================================================

EepromSave::EepromSave(Size sz) : size_(sz)
{
	u32 bytes = 0;
	switch (sz) {
	case Size::K4bit:
		bytes = 512;
		break;
	case Size::K64bit:
		bytes = 8 * 1024;
		break;
	case Size::K512bit:
		bytes = 64 * 1024;
		break;
	}
	data_.assign(bytes, 0xFF);
	reset();
}

u8 EepromSave::read(u32 offset) const
{
	(void) offset;
	// No memory-mapped read path on hardware; surface the serial output
	// bit so status polls observe ready/data.
	return static_cast<u8>(dmaRead() & 1u);
}

void EepromSave::write(u32 offset, u8 value)
{
	(void) offset;
	// CPU single-step fallback (detection probes); real transfers use
	// dmaWrite with the DMA unit counter.
	dmaWrite(value, 1);
}

uint16_t EepromSave::dmaRead() const
{
	// Outside an active readback the chip reports ready (bit 0 = 1).
	// Program/erase settle delay (~6.5ms on hardware) is not modeled;
	// writes retire immediately, so busy never surfaces (documented).
	if (cmd_ != Cmd::Read)
		return 1;
	if (read_left_ <= 0) {
		cmd_ = Cmd::Null;
		return 1;
	}
	--read_left_;
	if (read_left_ < 64) {
		// Data phase, MSB first: step counts down from 63.
		const int step = 63 - read_left_;
		const u32 bit_off = read_addr_ + static_cast<u32>(step);
		const u32 byte = bit_off >> 3;
		ensureCapacity(byte); // 64Kbit title on a 4Kbit guess: grow, read erased.
		uint16_t out = 0;
		if (byte < data_.size()) {
			out = static_cast<uint16_t>((data_[byte] >> (7 - (bit_off & 7))) & 1);
		}
		if (read_left_ == 0)
			cmd_ = Cmd::Null;
		return out;
	}
	// First 4 clocks are ignore bits.
	return 0;
}

void EepromSave::dmaWrite(uint16_t value, uint32_t remaining)
{
	const u32 bit = value & 1u;
	switch (cmd_) {
	case Cmd::Null:
		// Leading bit of "10"/"11" is always 1; anything else idles.
		cmd_ = (bit != 0) ? Cmd::Pending : Cmd::Null;
		break;
	case Cmd::Pending:
		cmd_ = (bit != 0) ? Cmd::ReadPending : Cmd::Write;
		write_addr_ = 0;
		read_addr_ = 0;
		break;
	case Cmd::Write:
		// Address bits arrive while more than the 64 data bits plus the
		// stop bit remain (>65); this threshold fits both 6- and 14-bit
		// address widths without a size parameter. Each address bit
		// positions the 64-bit block: bit offset = block << 6.
		if (remaining > 65) {
			write_addr_ = (write_addr_ << 1) | (bit << 6);
		} else if (remaining == 1) {
			cmd_ = Cmd::Null; // Stop bit retires the write.
		} else {
			const u32 byte = write_addr_ >> 3;
			ensureCapacity(byte); // 64Kbit title on a 4Kbit guess: grow, keep data.
			if (byte < data_.size()) {
				u8 cur = data_[byte];
				cur &= static_cast<u8>(~(1 << (7 - (write_addr_ & 7))));
				cur |= static_cast<u8>(bit << (7 - (write_addr_ & 7)));
				data_[byte] = cur;
			}
			++write_addr_;
		}
		break;
	case Cmd::ReadPending:
		if (remaining > 1) {
			read_addr_ = (read_addr_ << 1) | (bit != 0 ? 0x40u : 0u);
		} else {
			read_left_ = 68; // 4 ignore + 64 data.
			cmd_ = Cmd::Read;
		}
		break;
	case Cmd::Read:
		// Stray writes during readback are ignored (bus stays quiet).
		break;
	}
}

SaveType EepromSave::type() const
{
	switch (size_) {
	case Size::K4bit:
		return SaveType::EEPROM_4Kbit;
	case Size::K64bit:
		return SaveType::EEPROM_64Kbit;
	case Size::K512bit:
		return SaveType::EEPROM_512Kbit;
	}
	return SaveType::None;
}

void EepromSave::saveToFile(const std::string &path) const
{
	std::ofstream file(path, std::ios::binary);
	if (!file)
		return;
	file.write(reinterpret_cast<const char *>(data_.data()), data_.size());
}

void EepromSave::loadFromFile(const std::string &path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
		return;
	std::streamsize size = file.tellg();
	if (size <= 0)
		return;
	file.seekg(0, std::ios::beg);
	std::vector<u8> data(std::min<size_t>(static_cast<size_t>(size), data_.size()));
	file.read(reinterpret_cast<char *>(data.data()), data.size());
	std::copy(data.begin(), data.end(), data_.begin());
}

void EepromSave::reset()
{
	cmd_ = Cmd::Null;
	write_addr_ = 0;
	read_addr_ = 0;
	read_left_ = 0;
}

void EepromSave::save(StateBuffer &out) const
{
	out.write(static_cast<u8>(cmd_));
	out.write(write_addr_);
	out.write(read_addr_);
	out.write(read_left_);
	out.write(static_cast<u8>(size_));
	out.writeVector(data_);
}

void EepromSave::load(const StateBuffer &in)
{
	u8 s = 0;
	in.read(s);
	cmd_ = static_cast<Cmd>(s);
	in.read(write_addr_);
	in.read(read_addr_);
	in.read(read_left_);
	u8 sz = 0;
	if (in.read(sz))
		size_ = static_cast<Size>(sz);
	in.readVector(data_);
}

} // namespace gba