#pragma once

#include "gba/gpio.h"
#include "gba/scheduler.h"
#include "gba/state.h"
#include <array>
#include <cstddef>

namespace gba
{

class GbaIrq;
class GbaTimers;
class GbaDma;
class GbaPpu;
class GbaAudio;
class GbaKeypad;
class Cartridge;
class Cheats;

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
	// Open-bus value for locked BIOS reads: mGBA's biosPrefetch reset value
	// (its HLE equivalent never leaves it, since no real BIOS executes).
	// Sliced per access width/address exactly like mGBA's Load8/16/32.
	static constexpr u32 kBiosOpenBus = 0xE3A02004u;
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
	void setCheats(Cheats *c)
	{
		cheats_ = c;
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
	// Serial transfers carry the core tick so the EEPROM busy window
	// (program/erase settle time) is measured in real core cycles.
	void setScheduler(GbaScheduler *scheduler)
	{
		scheduler_ = scheduler;
	}
	u64 cycle() const
	{
		return scheduler_ != nullptr ? scheduler_->now() : 0;
	}
	uint16_t eepromDmaRead() const;
	void eepromDmaWrite(uint16_t value, uint32_t remaining);
	// Cart GPIO/RTC window (0x080000C4-0x080000C9). Active only when the
	// cartridge reports GPIO hardware (RTC carts); otherwise ROM.
	static bool isGpioRegion(u32 addr)
	{
		return addr >= GpioRtc::kDataAddr && addr < GpioRtc::kEndAddr;
	}
	bool gpioActive() const;
	// Serial (SIO) ports 0x04000120-0x0400012B: no link hardware, so
	// reads answer "idle" and writes are dropped (see bus.cpp).
	u16 readSerialIdle(u32 addr) const;
	void noteSerialUse(u32 addr) const;
	// mGBA debug channel (mgba-emu/suite reporting, NOT SIO): string buffer
	// at 0x04FFF600-0x04FFF6FF, flags at 0x04FFF700, enable at 0x04FFF780.
	// A flags store with bit 8 set publishes the buffered string (level =
	// low 3 bits, informational only; the guest always stores halfwords so
	// the high byte decides exactly once). The enable register reads
	// 0x1DEA after a 0xC0DE write, which is how guest code detects the
	// channel. Headless-safe; the --gba-suite runner parses debug_log.
	static bool isMgbaDebug(u32 addr)
	{
		return addr >= 0x04FFF600u && addr < 0x04FFF800u;
	}
	static constexpr u32 kMgbaDbgString = 0x04FFF600u;
	static constexpr u32 kMgbaDbgFlags = 0x04FFF700u;
	static constexpr u32 kMgbaDbgEnable = 0x04FFF780u;
	static constexpr u32 kMgbaDbgSize = 0x100u;
	static constexpr u16 kMgbaDbgMagic = 0xC0DEu;
	static constexpr u16 kMgbaDbgAck = 0x1DEAu;
	std::string debug_log;

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
	//
	// With tick-clocking enabled (GameBoyAdvance::step turns it on around
	// each instruction), the cost also advances the scheduler clock right
	// away, so armed events fire mid-instruction at their exact ticks
	// instead of at the next instruction boundary. Off by default, so unit
	// probes and GbaMachine runs keep bulk timing.
	u32 accessCycles(u32 addr, unsigned width_bytes, bool sequential);
	void setTickClock(bool tick)
	{
		tick_clock_ = tick;
	}
	// Opcode-fetch cost with the WAITCNT game-pak prefetch buffer (GBATEK
	// "System Control" bit 14). Data accesses and DMA use accessCycles and
	// never touch the buffer. With prefetch off this is exactly
	// accessCycles; with it on, a non-sequential fetch inside the buffered
	// 16-byte window (mGBA's 8-halfword overlap) costs S instead of N, which
	// is what makes tight ROM loops cheap. Everything else - including the
	// first-event arming slack documented in GameBoyAdvance::step() - is
	// unchanged.
	u32 fetchCycles(u32 addr, unsigned width_bytes, bool sequential);
	bool prefetchEnabled() const
	{
		return (waitcnt_ & 0x4000u) != 0;
	}

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
	// Game-pak prefetch buffer (see fetchCycles): the 16-byte block of the
	// last ROM fetch while WAITCNT bit 14 is set. Serialized (a load must
	// continue with the same timing the save had).
	bool prefetch_valid_ = false;
	u32 prefetch_base_ = 0;
	// Re-entrancy guard for the 8-bit I/O merge path (write8 -> write16 ->
	// write8 fallback). Without it an unhandled register would recurse.
	bool io_merge_active_ = false;
	GbaIrq *irq_ = nullptr;
	GbaTimers *timers_ = nullptr;
	GbaDma *dma_ = nullptr;
	GbaPpu *ppu_ = nullptr;
	GbaKeypad *keypad_ = nullptr;
	GbaAudio *audio_ = nullptr;
	Cheats *cheats_ = nullptr;
	GbaScheduler *scheduler_ = nullptr;
	// Per-cycle clock advancement (see accessCycles). Owned by whoever
	// drives execution; never serialized (a unit of work always ends at an
	// instruction boundary, where the clock is whole).
	bool tick_clock_ = false;
	void tickClock(unsigned cost);
	u32 accessCyclesUnticked(u32 addr, unsigned width_bytes, bool sequential) const;
	// mGBA debug channel state (see isMgbaDebug): enable latch, string
	// staging buffer, last flags. Serialized (a load continues the same
	// session); debug_log is runner output and is not (serial_log precedent).
	// mgba_enable_word_ is intra-store scratch for byte-wise assembly (like
	// io_merge_active_) and is deliberately not serialized.
	bool mgba_debug_on_ = false;
	std::array<u8, 0x100> mgba_dbg_{};
	u16 mgba_flags_ = 0;
	u16 mgba_enable_word_ = 0;
	void writeMgbaDebug(u32 addr, u8 value);
	u8 readMgbaDebug(u32 addr) const;
	void commitMgbaDebug();
	mutable bool serial_warned_ = false;
	Cartridge *cartridge_ = nullptr;

	u8 read8_raw(u32 addr);
	u16 read16_raw(u32 addr);
	u32 read32_raw(u32 addr);
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
