#pragma once

#include "gb/types.h"
#include "gba/audio.h"
#include "gba/bios.h"
#include "gba/bus.h"
#include "gba/cartridge.h"
#include "gba/cpu.h"
#include "gba/dma.h"
#include "gba/irq.h"
#include "gba/keypad.h"
#include "gba/ppu.h"
#include "gba/scheduler.h"
#include "gba/state.h"
#include "gba/timers.h"
#include <array>
#include <string>
#include <vector>

namespace gba
{

// GameBoyAdvance core (Phase 4: owns CPU + HLE BIOS; PPU/DMA/timers/IRQ
// attach in Phases 5-8 behind this same interface).
// Mirrors the gb::GameBoy ownership style (member objects, no heap wiring).
class GameBoyAdvance
{
  public:
	GameBoyAdvance();

	bool load(const std::string &path);
	bool loadFromBytes(const std::vector<u8> &data);
	void reset();
	bool saveState(const std::string &path) const;
	bool loadState(const std::string &path);
	void setRomPath(const std::string &path)
	{
		rom_path_ = path;
	}
	const std::string &romPath() const
	{
		return rom_path_;
	}

	// Advance up to `cycles` ticks. Drains the scheduler alongside the CPU
	// so Phase 5+ devices share the same clock. Honors reboot requests
	// (HardReset) and stops early while halted with no wake source.
	void step(u32 cycles);

	bool loaded() const
	{
		return loaded_;
	}
	u64 tick() const
	{
		return scheduler_.now();
	}
	const Cartridge &cartridge() const
	{
		return cartridge_;
	}
	GbaBus &bus()
	{
		return bus_;
	}
	const GbaBus &bus() const
	{
		return bus_;
	}
	GbaScheduler &scheduler()
	{
		return scheduler_;
	}
	Arm7Tdmi &cpu()
	{
		return cpu_;
	}
	HleBios &bios()
	{
		return bios_;
	}
	GbaIrq &irq()
	{
		return irq_;
	}
	GbaTimers &timers()
	{
		return timers_;
	}
	GbaDma &dma()
	{
		return dma_;
	}
	GbaPpu &ppu()
	{
		return ppu_;
	}
	GbaKeypad &keypad()
	{
		return keypad_;
	}
	GbaAudio &audio()
	{
		return audio_;
	}
	// SWI histogram (validation/diagnostics: which services games use).
	const std::array<u64, 0x2B> &swiHistogram() const
	{
		return swi_histogram_;
	}

  private:
	Cartridge cartridge_;
	GbaBus bus_;
	GbaScheduler scheduler_;
	Arm7Tdmi cpu_;
	HleBios bios_;
	GbaIrq irq_;
	GbaTimers timers_;
	GbaDma dma_;
	GbaPpu ppu_;
	GbaKeypad keypad_;
	GbaAudio audio_;
	bool loaded_ = false;
	std::array<u64, 0x2B> swi_histogram_{};
	std::string rom_path_;
	bool irq_trace_ = false;
};

} // namespace gba
