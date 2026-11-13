#pragma once

#include "gba/state.h"

namespace gba
{

class GbaBus;
class GbaIrq;
class GbaScheduler;

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
	// Scheduler wiring: the transfer's bus moves happen at trigger time
	// (as before), but completion - enable clear, repeat reload, IRQ - is a
	// scheduler event after the transfer's bus latency, so timer/PPU events
	// landing mid-transfer fire before completion instead of after it.
	// Events are guarded by a per-channel generation counter (no cancel):
	// a manual stop or re-enable invalidates the pending one.
	void setScheduler(GbaScheduler *scheduler)
	{
		scheduler_ = scheduler;
	}
	// Re-arm from restored state; the queue itself is not serialized.
	void rearmEvents();
	// True while a completion event is scheduled (tests / diagnostics).
	bool completion_pending(unsigned ch) const
	{
		return ch < kChannels && pending_[ch];
	}

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

	u16 read(u32 addr);
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
	GbaScheduler *scheduler_ = nullptr;
	Channel channels_[kChannels]{};
	u64 pending_cycles_ = 0;
	// Per-channel completion state: `gen_` invalidates a pending event when
	// the channel is stopped or reprogrammed under it; `deadline_` is the
	// absolute tick the completion fires at (serialized for save states).
	bool pending_[kChannels]{};
	u64 deadline_[kChannels]{};
	u32 gen_[kChannels]{};

	static unsigned maxCount(unsigned ch)
	{
		return ch == 3 ? 0x10000u : 0x4000u;
	}
	static u32 maskAddr(unsigned ch, u32 addr, bool dest);

	void latch(unsigned ch);
	void transfer(unsigned ch, bool fifo);
	void finish(unsigned ch);
	void scheduleCompletion(unsigned ch, u64 latency);
	void onCompletion(unsigned ch, u32 gen);
	void cancelPending(unsigned ch);
};

} // namespace gba
