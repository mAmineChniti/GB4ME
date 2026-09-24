#pragma once

#include "gba/state.h"

namespace gba
{

class GbaBus;
class GbaIrq;

// GBA DMA channels 0-3 (Phase 5). Immediate transfers run synchronously on
// the enable edge; VBlank/HBlank/FIFO transfers fire via trigger() from the
// PPU (Phase 6) and audio (Phase 8). Transfers move through GbaBus so
// WAITCNT timing accrues; the consumed cycles sit in pendingCycles() for
// the core to feed the scheduler/timers (CPU stalls during DMA).
// https://problemkaputt.de/gbatek.htm#gbadmatransfers
class GbaDma
{
  public:
	static constexpr u32 kIoBase = 0x040000B0;
	static constexpr unsigned kChannels = 4;

	static constexpr u32 kFifoA = 0x040000A0;
	static constexpr u32 kFifoB = 0x040000A4;

	// CNT_H bits.
	static constexpr u16 kDstMask = 0x0060;
	static constexpr u16 kSrcMask = 0x0180;
	static constexpr u16 kRepeat = 0x0200;
	static constexpr u16 kWord32 = 0x0400;
	static constexpr u16 kDrq = 0x0800; // DMA3 Game Pak DRQ (ignored here).
	static constexpr u16 kTimingMask = 0x3000;
	static constexpr u16 kIrq = 0x4000;
	static constexpr u16 kEnable = 0x8000;

	enum class Timing : u8 {
		Immediate = 0,
		VBlank = 1,
		HBlank = 2,
		Special = 3,
	};

	GbaDma();

	void reset();
	void setBus(GbaBus *bus)
	{
		bus_ = bus;
	}
	void setIrq(GbaIrq *irq)
	{
		irq_ = irq;
	}

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

	u16 read(u32 addr) const;
	void write(u32 addr, u16 value);

	// PPU/audio triggers. Fires armed channels whose timing matches.
	void trigger(Timing t);
	// Sound-FIFO request (DMA1/2, Special timing, repeat): 4x32-bit to FIFO.
	void triggerFifo(unsigned ch);

	// Cycles consumed by transfers since the last call (core drains these
	// into the scheduler + timers).
	u64 takePendingCycles()
	{
		const u64 c = pending_cycles_;
		pending_cycles_ = 0;
		return c;
	}

  private:
	struct Channel {
		u32 sad = 0;
		u32 dad = 0;
		u16 count = 0; // CNT_L as written.
		u16 control = 0;
		// Latched internal pointers/counter (SAD/DAD/CNT_L stay untouched).
		u32 src = 0;
		u32 dst = 0;
		u32 remaining = 0;
		bool armed = false;
	};

	GbaBus *bus_ = nullptr;
	GbaIrq *irq_ = nullptr;
	Channel channels_[kChannels]{};
	u64 pending_cycles_ = 0;

	static unsigned maxCount(unsigned ch)
	{
		return ch == 3 ? 0x10000u : 0x4000u;
	}
	static u32 maskAddr(unsigned ch, u32 addr, bool dest);

	void latch(unsigned ch);
	void transfer(unsigned ch, bool fifo);
	void finish(unsigned ch);
};

} // namespace gba
