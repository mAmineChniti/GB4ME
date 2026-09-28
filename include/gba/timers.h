#pragma once

#include "gba/state.h"
#include <functional>

namespace gba
{

class GbaIrq;
class GbaScheduler;

// GBA timers 0-3 (Phase 5). Prescalers F/1, F/64, F/256, F/1024; timers 1-3
// support count-up (cascade) on the previous timer's overflow. Overflow
// reloads the counter, optionally raises the timer IRQ, and cascades.
// https://problemkaputt.de/gbatek.htm#gbatimers
class GbaTimers
{
  public:
	static constexpr u32 kIoBase = 0x04000100;
	static constexpr unsigned kCount = 4;

	// CNT_H bits.
	static constexpr u16 kPrescalerMask = 0x0003;
	static constexpr u16 kCascade = 0x0004;
	static constexpr u16 kIrqEnable = 0x0040;
	static constexpr u16 kEnable = 0x0080;
	static constexpr u16 kControlMask = 0x00C7;

	GbaTimers();

	void reset();
	void setIrq(GbaIrq *irq)
	{
		irq_ = irq;
	}
	// Phase 8 hook: audio FIFO consumption on timer 0/1 overflow.
	void setOverflowHook(std::function<void(unsigned)> hook)
	{
		overflow_hook_ = hook;
	}

	// Scheduler wiring: every prescaled increment is an event, so an
	// overflow (and its IRQ) lands on the exact tick instead of whenever the
	// current step() slice stops. Events are guarded by a per-channel
	// generation counter rather than cancelled, so a prescaler/enable write
	// simply invalidates the pending one (GbaScheduler has no cancel).
	void setScheduler(GbaScheduler *scheduler)
	{
		scheduler_ = scheduler;
	}
	// Re-arm from restored state; the queue itself is not serialized.
	void rearmEvents();
	// Events still queued for a channel (tests / diagnostics).
	bool event_armed(unsigned i) const
	{
		return armed_[i];
	}
	// Ticks until this channel's next increment (tests / diagnostics).
	u32 ticks_to_next(unsigned i) const;
	// Increments performed (tests / diagnostics).
	u32 increments() const
	{
		return increments_;
	}

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

	// Called every step by the core; with a scheduler attached the events do
	// the counting and this only makes sure they are armed.
	void step(u32 cycles);

	u16 read(u32 addr) const; // CNT_L (live counter) / CNT_H (control).
	void write(u32 addr, u16 value);

	u16 counter(unsigned i) const
	{
		return channels_[i].counter;
	}
	u16 control(unsigned i) const
	{
		return channels_[i].control;
	}

  private:
	struct Channel {
		u16 reload = 0;
		u16 counter = 0;
		u16 control = 0;
		u32 phase = 0; // Prescaler progress in ticks.
	};

	GbaIrq *irq_ = nullptr;
	GbaScheduler *scheduler_ = nullptr;
	std::function<void(unsigned)> overflow_hook_;
	Channel channels_[kCount]{};
	// Per-channel event state: `gen_` invalidates a pending event when the
	// prescaler or enable changes under it.
	bool armed_[kCount]{};
	u32 gen_[kCount]{};
	u32 increments_ = 0;

	static u32 prescaler(u16 control)
	{
		static constexpr u32 kTable[4] = {1, 64, 256, 1024};
		return kTable[control & kPrescalerMask];
	}

	bool enabled(unsigned i) const
	{
		return (channels_[i].control & kEnable) != 0;
	}
	bool cascade(unsigned i) const
	{
		return i != 0 && (channels_[i].control & kCascade) != 0;
	}

	void armChannel(unsigned i);
	void armAll();
	void onTimerTick(unsigned i, u32 gen);
	void invalidate(unsigned i);
	void clockChannel(unsigned i); // One counter increment (may overflow).
	void overflow(unsigned i);
};

} // namespace gba
