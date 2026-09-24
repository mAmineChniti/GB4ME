// GBA bus: region decode, RAM backing, ROM mirror, save routing, protection.
// https://problemkaputt.de/gbatek.htm#gbamemorymap
// Mirroring follows GBATEK (EWRAM/IWRAM/PAL/VRAM/OAM mirrors). 0x0E
// accesses route to the cartridge save backend with region-relative
// offsets; 0x0D routes to the EEPROM serial engine on EEPROM carts and
// otherwise mirrors ROM.

#include "gba/bus.h"
#include "gba/audio.h"
#include "gba/cartridge.h"
#include "gba/dma.h"
#include "gba/irq.h"
#include "gba/keypad.h"
#include "gba/ppu.h"
#include "gba/timers.h"

namespace gba
{

// GBATEK: the 64K SRAM field is mirrored across the whole 32MB window at
// 0x0E000000-0x0FFFFFFF, and a 32K chip repeats twice inside that field.
// Masking here makes every alias hit the same physical byte.
u32 GbaBus::sramOffset(u32 addr) const
{
	u32 off = addr - kSramBase;
	if (cartridge_ != nullptr) {
		const u32 size = cartridge_->saveSize();
		if (size > 0)
			off &= size - 1;
	}
	return off & 0xFFFFu;
}

GbaBus::GbaBus()
{
	reset();
}

void GbaBus::reset()
{
	ewram_.fill(0);
	iwram_.fill(0);
	io_.fill(0);
	bios_.fill(0);
	bios_active_ = false;
	waitcnt_ = 0; // Reset default; Phase 5 routes the I/O write here.
				  // ROM attachment survives reset (cartridge stays inserted).
				  // Video memory resets with the PPU (core calls ppu.reset()).
}

void GbaBus::attachRom(const u8 *data, size_t size)
{
	rom_ = data;
	rom_size_ = size;
}

void GbaBus::detachRom()
{
	rom_ = nullptr;
	rom_size_ = 0;
}

GbaBus::Region GbaBus::decode(u32 addr)
{
	const u32 hi = addr >> 24;
	switch (hi) {
	case 0x00:
		return addr < kBiosSize ? Region::Bios : Region::Open;
	case 0x02:
		return Region::Ewram;
	case 0x03:
		return Region::Iwram;
	case 0x04:
		return addr - kIoBase < kIoSize ? Region::Io : Region::Open;
	case 0x05:
		return Region::Palette;
	case 0x06:
		return Region::Vram;
	case 0x07:
		return Region::Oam;
	case 0x08:
	case 0x09:
	case 0x0A:
	case 0x0B:
	case 0x0C:
	case 0x0D:
		return Region::Rom;
	case 0x0E:
	case 0x0F:
		return Region::Sram;
	default:
		return Region::Open;
	}
}

u8 GbaBus::readRom(u32 addr) const
{
	if (rom_ == nullptr || rom_size_ == 0)
		return 0xFF;
	// Each 32M wait-state slot mirrors the image; non-power-of-two images
	// wrap by modulo (hardware mirrors by address-line truncation).
	const u32 off = (addr & (kRomMirrorSize - 1)) % static_cast<u32>(rom_size_);
	return rom_[off];
}

bool GbaBus::eepromActive() const
{
	return cartridge_ != nullptr && cartridge_->isEeprom();
}

bool GbaBus::gpioActive() const
{
	return cartridge_ != nullptr && cartridge_->hasGpio();
}

uint16_t GbaBus::eepromDmaRead() const
{
	if (cartridge_ != nullptr)
		return cartridge_->eepromDmaRead();
	return 1;
}

void GbaBus::eepromDmaWrite(uint16_t value, uint32_t remaining)
{
	if (cartridge_ != nullptr)
		cartridge_->eepromDmaWrite(value, remaining);
}

u8 GbaBus::read8(u32 addr)
{
	// EEPROM carts: 0x0D is the serial port, not a ROM mirror.
	if (isEepromRegion(addr) && eepromActive())
		return static_cast<u8>(eepromDmaRead() & 1u);
	// RTC carts: 0x080000C4-0x080000C9 are the GPIO registers (GBATEK),
	// not ROM bytes.
	if (isGpioRegion(addr) && gpioActive())
		return cartridge_->gpio().read(addr);
	switch (decode(addr)) {
	case Region::Bios: {
		// BIOS protection: locked reads are open-bus on hardware (last
		// prefetch value); 0xFF bytes are a documented simplification.
		if (!bios_active_)
			return 0xFF;
		return bios_[addr & (kBiosSize - 1)];
	}
	case Region::Ewram:
		return ewram_[addr & (kEwramSize - 1)];
	case Region::Iwram:
		return iwram_[addr & (kIwramSize - 1)];
	case Region::Io: {
		// LCD block reads compose from the 16-bit registers (always
		// fresh); keypad registers compose the same way. Byte writes
		// below stay on backing (games use 16-bit LCD/keypad access;
		// cross-width access revisited on evidence).
		if (ppu_ != nullptr && addr >= GbaPpu::kIoBase && addr <= GbaPpu::kIoEnd) {
			const u16 reg = ppu_->readIo(addr & ~1u);
			return static_cast<u8>((addr & 1u) != 0 ? (reg >> 8) : (reg & 0xFFu));
		}
		if (keypad_ != nullptr &&
			(addr == GbaKeypad::kKeyInputAddr || addr == GbaKeypad::kKeyInputAddr + 1 ||
			 addr == GbaKeypad::kKeyCntAddr || addr == GbaKeypad::kKeyCntAddr + 1)) {
			const u16 reg =
				(addr < GbaKeypad::kKeyCntAddr) ? keypad_->readInput() : keypad_->readCnt();
			return static_cast<u8>((addr & 1u) != 0 ? (reg >> 8) : (reg & 0xFFu));
		}
		// Sound block (PSG + SOUNDCNT + SOUNDBIAS + wave + FIFOs).
		if (audio_ != nullptr && addr >= 0x04000060u && addr < 0x040000A8u) {
			return audio_->read8(addr);
		}
		// Device-owned registers outside the LCD/keypad/audio blocks:
		// compose the byte from the owning unit's 16-bit register so byte
		// reads observe live device state instead of the io_ shadow.
		const bool device_owned =
			(addr >= 0x040000B0u && addr < 0x040000E0u) ||  // DMA
			(addr >= 0x04000100u && addr < 0x04000110u) || // Timers
			(addr >= 0x04000200u && addr <= 0x04000205u) || // IE/IF/WAITCNT
			(addr >= 0x04000208u && addr <= 0x04000209u);   // IME
		if (device_owned) {
			const u16 reg = deviceByte16(addr);
			return static_cast<u8>((addr & 1u) != 0 ? (reg >> 8) : (reg & 0xFFu));
		}
		return io_[addr - kIoBase];
	}
	case Region::Palette:
		return ppu_ != nullptr ? ppu_->readPal(addr - kPalBase) : 0xFF;
	case Region::Vram:
		return ppu_ != nullptr ? ppu_->readVram(addr - kVramBase) : 0xFF;
	case Region::Oam:
		return ppu_ != nullptr ? ppu_->readOam(addr - kOamBase) : 0xFF;
	case Region::Rom:
		return readRom(addr);
	case Region::Sram: {
		// SaveHardware takes region-relative offsets (0 = 0x0E000000).
		if (cartridge_)
			return cartridge_->readSave(sramOffset(addr));
		return 0xFF;
	}
	case Region::Open:
		return 0xFF;
	}
	return 0xFF;
}

// 16-bit view of a device-owned I/O register for the byte-read path in
// read8(). Returns 0xFFFF when the address is not owned by a unit.
// https://problemkaputt.de/gbatek.htm#gbaiomap

u16 GbaBus::deviceByte16(u32 addr, bool *owned) const
{
	addr &= ~1u;
	if (owned != nullptr)
		*owned = true;
	if (dma_ != nullptr && addr >= 0x040000B0u && addr < 0x040000E0u)
		return dma_->read(addr);
	if (timers_ != nullptr && addr >= 0x04000100u && addr < 0x04000110u)
		return timers_->read(addr);
	if (irq_ != nullptr) {
		if (addr == 0x04000200u)
			return irq_->readIe();
		if (addr == 0x04000202u)
			return irq_->readIf();
		if (addr == 0x04000208u)
			return irq_->readIme();
	}
	if (addr == 0x04000204u)
		return waitcnt_;
	// LCD, keypad and audio registers are device-owned too, so a byte
	// write must merge against the live value rather than the shadow.
	if (ppu_ != nullptr && addr >= GbaPpu::kIoBase && addr <= GbaPpu::kIoEnd)
		return ppu_->readIo(addr);
	if (keypad_ != nullptr && addr == GbaKeypad::kKeyCntAddr)
		return keypad_->readCnt();
	// No unit owns it: report the shadow instead of 0xFFFF, otherwise an
	// 8-bit write would merge 0xFF into the untouched half (e.g. a STRB to
	// DISPCNT would turn 0x34 into 0xFF34).
	if (owned != nullptr)
		*owned = false;
	const u32 i = addr - kIoBase;
	return static_cast<u16>((static_cast<u16>(io_[i]) & 0xFFu) |
							(static_cast<u16>(io_[i + 1]) << 8));
}

// 16-bit accesses ignore A0 on hardware (GBATEK "Memory Alignments");
// masking here also keeps the device-register and video-memory paths from
// composing bogus values out of two different registers.
u16 GbaBus::read16(u32 addr)
{
	// EEPROM serial read (CPU poll path; real streaming uses DMA3).
	if (isEepromRegion(addr) && eepromActive())
		return eepromDmaRead() & 1u;
	addr &= ~1u;
	// Fast path for the plain RAM/ROM regions. Decomposing these into two
	// read8() calls costs two extra decode()s and four extra calls per
	// access, and the CPU does millions of them per frame; the masks are
	// identical to what read8() applies.
	switch (decode(addr)) {
	case Region::Iwram:
		return static_cast<u16>(iwram_[addr & (kIwramSize - 1)] |
								(static_cast<u16>(iwram_[(addr + 1) & (kIwramSize - 1)]) << 8));
	case Region::Ewram:
		return static_cast<u16>(ewram_[addr & (kEwramSize - 1)] |
								(static_cast<u16>(ewram_[(addr + 1) & (kEwramSize - 1)]) << 8));
	case Region::Rom:
		// GPIO/RTC registers live in ROM space; leave them to the generic
		// path in read8() so they don't read as raw ROM bytes.
		if (!isGpioRegion(addr))
			return static_cast<u16>(readRom(addr) | (static_cast<u16>(readRom(addr + 1)) << 8));
		break;
	default:
		break;
	}
	// Device-owned registers (16-bit access per GBATEK).
	if ((addr & 1) == 0) {
		if (dma_ != nullptr && addr >= 0x040000B0u && addr < 0x040000E0u) {
			return dma_->read(addr);
		}
		if (timers_ != nullptr && addr >= 0x04000100u && addr < 0x04000110u) {
			return timers_->read(addr);
		}
		if (irq_ != nullptr) {
			if (addr == 0x04000200u)
				return irq_->readIe();
			if (addr == 0x04000202u)
				return irq_->readIf();
			if (addr == 0x04000208u)
				return irq_->readIme();
		}
		if (addr == 0x04000204u)
			return waitcnt_;
	}
	// GBATEK "Accessing SRAM Area by 16bit/32bit": SRAM sits on an 8-bit
	// bus, so a wide read fetches ONE byte and replicates it, rather than
	// assembling two adjacent bytes.
	if (decode(addr) == Region::Sram && cartridge_ != nullptr) {
		const u8 b = cartridge_->readSave(sramOffset(addr));
		return static_cast<u16>(b * 0x0101u);
	}
	const u8 lo = read8(addr);
	const u8 hi = read8(addr + 1);
	return static_cast<u16>(lo | (static_cast<u16>(hi) << 8));
}

u32 GbaBus::read32(u32 addr)
{
	// 32-bit accesses ignore A1-0 (GBATEK "Memory Alignments"): the address
	// is rounded down, and the CPU layer applies the LDR rotation.
	addr &= ~3u;
	// EEPROM serial read must be handled BEFORE the ROM fast path: 0x0D
	// lives in ROM space, so the fast path would return raw cartridge bytes
	// instead of the serial data the two read16() calls would have got.
	if (isEepromRegion(addr) && eepromActive()) {
		const u32 v = eepromDmaRead() & 1u;
		return v * 0x01010101u;
	}
	// Same 8-bit SRAM bus rule: one byte replicated four times.
	if (decode(addr) == Region::Sram && cartridge_ != nullptr) {
		const u8 b = cartridge_->readSave(sramOffset(addr));
		return static_cast<u32>(b * 0x01010101u);
	}
	// Fast path: plain RAM/ROM read directly instead of two read16() calls
	// (which would each re-decode). Masks match read8()'s per-byte masking.
	switch (decode(addr)) {
	case Region::Iwram: {
		const u32 m = kIwramSize - 1;
		return static_cast<u32>(iwram_[addr & m]) | (static_cast<u32>(iwram_[(addr + 1) & m]) << 8) |
			   (static_cast<u32>(iwram_[(addr + 2) & m]) << 16) |
			   (static_cast<u32>(iwram_[(addr + 3) & m]) << 24);
	}
	case Region::Ewram: {
		const u32 m = kEwramSize - 1;
		return static_cast<u32>(ewram_[addr & m]) | (static_cast<u32>(ewram_[(addr + 1) & m]) << 8) |
			   (static_cast<u32>(ewram_[(addr + 2) & m]) << 16) |
			   (static_cast<u32>(ewram_[(addr + 3) & m]) << 24);
	}
	case Region::Rom:
		if (!isGpioRegion(addr))
			return static_cast<u32>(readRom(addr)) | (static_cast<u32>(readRom(addr + 1)) << 8) |
				   (static_cast<u32>(readRom(addr + 2)) << 16) |
				   (static_cast<u32>(readRom(addr + 3)) << 24);
		break;
	default:
		break;
	}
	const u16 lo = read16(addr);
	const u16 hi = read16(addr + 2);
	return lo | (static_cast<u32>(hi) << 16);
}

void GbaBus::write8(u32 addr, u8 value)
{
	if (isEepromRegion(addr) && eepromActive()) {
		eepromDmaWrite(value, 1);
		return;
	}
	switch (decode(addr)) {
	case Region::Bios:
		return; // Never writable (HLE installs via reset path).
	case Region::Ewram:
		ewram_[addr & (kEwramSize - 1)] = value;
		return;
	case Region::Iwram:
		iwram_[addr & (kIwramSize - 1)] = value;
		return;
	case Region::Io: {
		if (audio_ != nullptr && addr >= 0x04000060u && addr < 0x040000A8u) {
			audio_->write8(addr, value);
			return;
		}
		// GBATEK: I/O registers are 16-bit; an 8-bit write is merged with
		// the neighbouring byte and dispatched as a 16-bit write. Without
		// this, STRB to a device-owned register (DISPCNT, DMA*, timers, IE,
		// IF, KEYCNT) only touched the shadow and never reached the device.
		if (!io_merge_active_) {
			const u32 index = addr - kIoBase;
			const u32 aligned = index & ~1u;
			// Merge against the LIVE register when a unit owns it, and the
			// io_ shadow otherwise: the shadow can be stale for a device the
			// CPU also changes (IF is raised by hardware, DMA re-arms
			// itself). deviceByte16() already falls back to the shadow, but
			// an explicit `owned` flag keeps the two cases obvious.
			bool owned = false;
			const u16 cur = deviceByte16(kIoBase + aligned, &owned);
			(void)owned;
			u16 merged = 0;
			if ((index & 1u) != 0)
				merged = static_cast<u16>((cur & 0x00FFu) | (static_cast<u16>(value) << 8));
			else
				merged = static_cast<u16>((static_cast<u16>(value) & 0x00FFu) |
										  (cur & 0xFF00u));
			// Keep the shadow in step so the next merge sees this write.
			io_[aligned] = static_cast<u8>(merged & 0xFFu);
			io_[aligned + 1] = static_cast<u8>(merged >> 8);
			io_merge_active_ = true;
			write16(kIoBase + aligned, merged);
			io_merge_active_ = false;
			return;
		}
		io_[addr - kIoBase] = value;
		return;
	}
	// Video memory writes go through the PPU (GBA width rules live
	// there; 8-bit OAM writes and OBJ-VRAM byte writes are ignored).
	case Region::Palette:
		if (ppu_ != nullptr)
			ppu_->writePal(addr - kPalBase, value);
		return;
	case Region::Vram:
		if (ppu_ != nullptr)
			ppu_->writeVram(addr - kVramBase, value);
		return;
	case Region::Oam:
		if (ppu_ != nullptr)
			ppu_->writeOam(addr - kOamBase, value);
		return;
	case Region::Rom:
		return; // ROM not writable (Flash cmds: Phase 9).
	case Region::Sram:
		if (cartridge_)
			cartridge_->writeSave(sramOffset(addr), value);
		return;
	case Region::Open:
		return;
	}
}

void GbaBus::write16(u32 addr, u16 value)
{
	// EEPROM serial write (CPU probe path; real streaming uses DMA3).
	if (isEepromRegion(addr) && eepromActive()) {
		eepromDmaWrite(value, 1);
		return;
	}
	// GPIO registers are 16-bit (GBATEK: STRB opcodes ignored on ROM bus).
	if (isGpioRegion(addr) && gpioActive()) {
		cartridge_->gpio().write(addr & ~1u, value);
		return;
	}
	// SRAM sits on an 8-bit bus: a 16/32-bit write updates only the single
	// addressed byte, taking the LSB of the source rotated right by
	// address*8 (GBATEK "Accessing SRAM Area by 16bit/32bit"). Flash chips
	// additionally expose only that one byte, so a phantom second write
	// cannot corrupt the unlock sequence (mGBA parity).
	if (decode(addr) == Region::Sram && cartridge_ != nullptr) {
		const u32 rot = (addr & 3u) * 8u;
		const u32 src = (rot == 0) ? static_cast<u32>(value)
								   : ((static_cast<u32>(value) >> rot) |
									  (static_cast<u32>(value) << (32u - rot)));
		cartridge_->writeSave(sramOffset(addr), static_cast<u8>(src & 0xFFu));
		return;
	}
	addr &= ~1u; // 16-bit accesses ignore A0 (GBATEK "Memory Alignments").
	if (dispatchIo16(addr, value)) {
		// Record the written halfwords in the shadow so a later 8-bit write
		// can merge against the correct neighbouring byte.
		if (addr >= kIoBase && addr < kIoBase + kIoSize) {
			const u32 i = addr - kIoBase;
			io_[i] = static_cast<u8>(value & 0xFFu);
			io_[i + 1] = static_cast<u8>(value >> 8);
		}
		return;
	}
	write8(addr, static_cast<u8>(value & 0xFFu));
	write8(addr + 1, static_cast<u8>(value >> 8));
}

bool GbaBus::dispatchIo16(u32 addr, u16 value)
{
	if (dma_ != nullptr && addr >= 0x040000B0u && addr < 0x040000E0u) {
		dma_->write(addr, value);
		return true;
	}
	if (timers_ != nullptr && addr >= 0x04000100u && addr < 0x04000110u) {
		timers_->write(addr, value);
		return true;
	}
	if (irq_ != nullptr) {
		if (addr == 0x04000200u) {
			irq_->writeIe(value);
			return true;
		}
		if (addr == 0x04000202u) {
			irq_->writeIf(value);
			return true;
		}
		if (addr == 0x04000208u) {
			irq_->writeIme(value);
			return true;
		}
	}
	if (addr == 0x04000204u) {
		waitcnt_ = value;
		return true;
	}
	if (keypad_ != nullptr && addr == GbaKeypad::kKeyCntAddr) {
		keypad_->writeCnt(value);
		return true;
	}
	if (audio_ != nullptr && addr >= 0x04000060u && addr < 0x040000A8u) {
		audio_->write16(addr, value);
		return true;
	}
	// Video memory 16-bit writes are native (byte semantics differ:
	// replication/ignore rules must not apply here). Ranges match
	// decode() (whole-region mirrors; the PPU masks internally).
	if (ppu_ != nullptr) {
		const u32 hi = addr >> 24;
		if (hi == 0x05) {
			ppu_->writePal16(addr - kPalBase, value);
			return true;
		}
		if (hi == 0x06) {
			ppu_->writeVram16(addr - kVramBase, value);
			return true;
		}
		if (hi == 0x07) {
			ppu_->writeOam16(addr - kOamBase, value);
			return true;
		}
		if (addr >= GbaPpu::kIoBase && addr <= GbaPpu::kIoEnd) {
			ppu_->writeIo(addr, value);
			return true;
		}
	}
	return false;
}

void GbaBus::write32(u32 addr, u32 value)
{
	// 32-bit accesses ignore A1-0 (GBATEK "Memory Alignments"); the CPU
	// layer rounds the address down before calling, and DMA does the same.
	addr &= ~3u;
	// SRAM sits on an 8-bit bus: a 32-bit write must update exactly ONE
	// byte, taking the LSB of `value ROR (addr*8)` (GBATEK "Accessing SRAM
	// Area by 16bit/32bit"). Splitting into two write16() calls would write
	// a second byte at addr+2 - corrupting a neighbouring save location and
	// sending Flash a phantom command byte.
	if (decode(addr) == Region::Sram && cartridge_ != nullptr) {
		const u32 rot = (addr & 3u) * 8u;
		const u32 src = (rot == 0) ? value : ((value >> rot) | (value << (32u - rot)));
		cartridge_->writeSave(sramOffset(addr), static_cast<u8>(src & 0xFFu));
		return;
	}
	write16(addr, static_cast<u16>(value & 0xFFFF));
	write16(addr + 2, static_cast<u16>(value >> 16));
}

// ---- WAITCNT decoders (GBATEK "System Control") ----
// Reset default WAITCNT = 0x0000: SRAM 4, WS0 N=4/S=2, WS1 N=4/S=4,
// WS2 N=4/S=8, prefetch off (prefetch not modeled yet — Phase 5+).

unsigned GbaBus::romWaitN(unsigned slot) const
{
	// N-cycle tables per slot: {WS0 bits 3-2, WS1 bits 6-5, WS2 bits 9-8}.
	static constexpr unsigned kTable[4] = {4, 3, 2, 8};
	unsigned shift = 2 + slot * 3;
	if (slot > 2)
		return 4;
	return kTable[(waitcnt_ >> shift) & 3];
}

unsigned GbaBus::romWaitS(unsigned slot) const
{
	// S-cycle bits: WS0 bit 4, WS1 bit 7, WS2 bit 10. WS0: 0->2, 1->1;
	// WS1: 0->4, 1->1; WS2: 0->8, 1->1.
	static constexpr unsigned kSlow[3] = {2, 4, 8};
	if (slot > 2)
		return 4;
	const unsigned bit = (waitcnt_ >> (4 + slot * 3)) & 1;
	return bit != 0 ? 1 : kSlow[slot];
}

unsigned GbaBus::sramWait() const
{
	static constexpr unsigned kTable[4] = {4, 3, 2, 8};
	return kTable[waitcnt_ & 3];
}

u32 GbaBus::accessCycles(u32 addr, unsigned width_bytes, bool sequential) const
{
	if (width_bytes != 1 && width_bytes != 2 && width_bytes != 4)
		return 1;
	switch (decode(addr)) {
	case Region::Bios:
	case Region::Iwram:
	case Region::Io:
		// 32-bit internal bus: any width costs one cycle.
		return 1;
	case Region::Palette:
	case Region::Vram:
	case Region::Oam:
		// 16-bit video bus: 8/16-bit cost one, 32-bit costs two.
		return width_bytes == 4 ? 2 : 1;
	case Region::Ewram:
		// 16-bit bus, 2 waitstates (WAITCNT 4000800h default 0Dh):
		// 8/16-bit cost two cycles, 32-bit costs four (GBATEK).
		return width_bytes == 4 ? 4 : 2;
	case Region::Rom: {
		// 16-bit bus with N/S waitstates; 32-bit splits N+S.
		// Slots are 32 MB each: WS0 08-09, WS1 0A-0B, WS2 0C-0D.
		const unsigned slot = (addr >> 25) & 3; // 0 WS0, 1 WS1, 2 WS2.
		const unsigned first = sequential ? romWaitS(slot) : romWaitN(slot);
		if (width_bytes == 4)
			return first + romWaitS(slot);
		return first;
	}
	case Region::Sram:
		// 8-bit bus: one SRAM wait per byte transferred.
		return sramWait() * width_bytes;
	case Region::Open:
		return 1;
	}
	return 1;
}

void GbaBus::save(StateBuffer &out) const
{
	out.writeBytes(ewram_.data(), ewram_.size());
	out.writeBytes(iwram_.data(), iwram_.size());
	out.writeBytes(io_.data(), io_.size());
	out.writeBytes(bios_.data(), bios_.size());
	out.write(bios_active_);
	out.write(waitcnt_);
}

void GbaBus::load(const StateBuffer &in)
{
	in.readBytes(ewram_.data(), ewram_.size());
	in.readBytes(iwram_.data(), iwram_.size());
	in.readBytes(io_.data(), io_.size());
	in.readBytes(bios_.data(), bios_.size());
	in.read(bios_active_);
	in.read(waitcnt_);
}

} // namespace gba
