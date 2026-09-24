#pragma once

#include "gba/gpio.h"
#include "gba/state.h"
#include <array>
#include <cstddef>

namespace gba
{

class GbaIrq;
class GbaTimers;
class GbaDma;
class GbaPpu;
class GbaKeypad;
class GbaAudio;
class Cartridge;

// GBA memory bus (Phase 1 skeleton: map + RAM + protection, no devices yet).
// https://problemkaputt.de/gbatek.htm#gbamemorymap
// Independent from gb::MMU: 32-bit addresses, 8/16/32-bit accesses, and a
// completely different region layout. Shares only the design discipline
// (all accesses route through read/write; devices attach later).
class GbaBus
{
  public:
	// Region bases/sizes (GBATEK "GBA Memory Map").
	static constexpr u32 kBiosBase = 0x00000000, kBiosSize = 0x4000;
	static constexpr u32 kEwramBase = 0x02000000, kEwramSize = 0x40000; // 256K
	static constexpr u32 kIwramBase = 0x03000000, kIwramSize = 0x8000;  // 32K
	static constexpr u32 kIoBase = 0x04000000, kIoSize = 0x400;
	static constexpr u32 kPalBase = 0x05000000, kPalSize = 0x400;     // 1K
	static constexpr u32 kVramBase = 0x06000000, kVramSize = 0x18000; // 96K
	static constexpr u32 kOamBase = 0x07000000, kOamSize = 0x400;     // 1K
	static constexpr u32 kRomWs0Base = 0x08000000;
	static constexpr u32 kRomWs1Base = 0x0A000000;
	static constexpr u32 kRomWs2Base = 0x0C000000;
	static constexpr u32 kRomMirrorSize = 0x02000000; // 32M per wait-state slot.
	static constexpr u32 kSramBase = 0x0E000000,
						 kSramSize = 0x1000000; // 16MB (save hardware maps here)

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
	void attachRom(const u8 *data, size_t size);
	void detachRom();

	// Device routing (Phase 5+, gb::MMU-style delegation): DMA, timer,
	// interrupt, PPU/video-memory and LCD registers are owned by their
	// units; everything else lives in the io_ backing store. Pointers are
	// set once by the core.
	void setIrq(GbaIrq *irq)
	{
		irq_ = irq;
	}
	void setTimers(GbaTimers *timers)
	{
		timers_ = timers;
	}
	void setDma(GbaDma *dma)
	{
		dma_ = dma;
	}
	void setPpu(GbaPpu *ppu)
	{
		ppu_ = ppu;
	}
	void setKeypad(GbaKeypad *keypad)
	{
		keypad_ = keypad;
	}
	void setAudio(GbaAudio *audio)
	{
		audio_ = audio;
	}
	void setCartridge(Cartridge *cart)
	{
		cartridge_ = cart;
	}

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

	// BIOS protection (HLE BIOS, Phase 4): while the CPU executes outside
	// the BIOS region, BIOS reads return open-bus bytes and writes are
	// ignored. setBiosActive(true) is used by HLE startup/SWI entry.
	void setBiosActive(bool active)
	{
		bios_active_ = active;
	}
	bool biosActive() const
	{
		return bios_active_;
	}
	// One-time HLE vector install (Phase 5): bypasses the write protection
	// above. Only the HLE BIOS calls this, once per boot.
	void writeBios(u32 offset, u8 value)
	{
		if (offset < kBiosSize)
			bios_[offset] = value;
	}

	// EEPROM serial region (0x0D000000-0x0DFFFFFF). On EEPROM carts
	// this range is the bitstream port (DMA3, one halfword per bit);
	// otherwise it mirrors ROM. Active only when the cartridge reports
	// an EEPROM save type.
	static bool isEepromRegion(u32 addr)
	{
		return (addr >> 24) == 0x0Du;
	}
	bool eepromActive() const;
	uint16_t eepromDmaRead() const;
	void eepromDmaWrite(uint16_t value, uint32_t remaining);
	// Cart GPIO/RTC window (0x080000C4-0x080000C9). Active only when the
	// cartridge reports GPIO hardware (RTC carts); otherwise ROM.
	static bool isGpioRegion(u32 addr)
	{
		return addr >= GpioRtc::kDataAddr && addr < GpioRtc::kEndAddr;
	}
	bool gpioActive() const;

	// WAITCNT (0x04000204) backing. Reset default 0x0000; Phase 5 routes
	// the I/O write here. Tables below follow GBATEK "Address Bus Width
	// and CPU Read/Write Access Widths" + WAITCNT defaults.
	u16 waitcnt() const
	{
		return waitcnt_;
	}
	void setWaitcnt(u16 v)
	{
		waitcnt_ = v;
	}

	// Bus-cycle cost of one CPU access of `width_bytes` (1/2/4) at `addr`.
	// `sequential` selects S vs N timings where they differ (ROM; second
	// half of a split 32-bit access is always sequential). Used for both
	// code fetches and data accesses (Phase 3); the CPU accumulates these
	// on top of its fixed per-instruction overhead.
	u32 accessCycles(u32 addr, unsigned width_bytes, bool sequential) const;

	static Region decode(u32 addr);

	u8 read8(u32 addr);
	u16 read16(u32 addr);
	u32 read32(u32 addr);
	void write8(u32 addr, u8 value);
	void write16(u32 addr, u16 value);
	void write32(u32 addr, u32 value);

	// Direct views for later devices/tests (PPU owns VRAM/PAL/OAM in
	// Phase 6; until then the bus holds the backing store).
	std::array<u8, kEwramSize> &ewram()
	{
		return ewram_;
	}
	std::array<u8, kIwramSize> &iwram()
	{
		return iwram_;
	}

  private:
	std::array<u8, kEwramSize> ewram_{};
	std::array<u8, kIwramSize> iwram_{};
	std::array<u8, kIoSize> io_{};
	// Palette/VRAM/OAM live in the PPU (gb::PPU precedent); the bus only
	// routes those regions there.
	// SRAM/Flash/EEPROM now handled by Cartridge's SaveHardware
	std::array<u8, kBiosSize> bios_{};

	const u8 *rom_ = nullptr;
	size_t rom_size_ = 0;
	bool bios_active_ = false;
	u16 waitcnt_ = 0;
	// Re-entrancy guard for the 8-bit I/O merge path (write8 -> write16 ->
	// write8 fallback). Without it an unhandled register would recurse.
	bool io_merge_active_ = false;
	GbaIrq *irq_ = nullptr;
	GbaTimers *timers_ = nullptr;
	GbaDma *dma_ = nullptr;
	GbaPpu *ppu_ = nullptr;
	GbaKeypad *keypad_ = nullptr;
	GbaAudio *audio_ = nullptr;
	Cartridge *cartridge_ = nullptr;

	u8 readRom(u32 addr) const;
	// Region-relative SRAM/Flash offset with the GBATEK mirror masking
	// applied (32MB window + chip-size repeat).
	u32 sramOffset(u32 addr) const;
	// Routes a 16-bit write to the owning device (DMA/timers/IRQ/WAITCNT/
	// keypad/audio/PPU/VRAM). Returns false when nothing claims the address.
	bool dispatchIo16(u32 addr, u16 value);
	// 16-bit view of a device-owned I/O register for the byte-read path
	// (read8). Returns 0xFFFF for addresses no unit owns.
	u16 deviceByte16(u32 addr, bool *owned = nullptr) const;
	// WAITCNT field decoders (GBATEK "System Control").
	unsigned romWaitN(unsigned slot) const; // First-access (non-sequential).
	unsigned romWaitS(unsigned slot) const; // Sequential.
	unsigned sramWait() const;
};

} // namespace gba
