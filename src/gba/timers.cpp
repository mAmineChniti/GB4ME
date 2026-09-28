// GBA timers: prescaled counting, cascade, overflow IRQ.

#include "gba/timers.h"

#include "gba/irq.h"
#include "gba/scheduler.h"

namespace gba
{

GbaTimers::GbaTimers()
{
	reset();
}

void GbaTimers::reset()
{
	for (auto &ch : channels_)
		ch = Channel{};
	for (unsigned i = 0; i < kCount; i++) {
		armed_[i] = false;
		gen_[i] = 0;
	}
	increments_ = 0;
}

void GbaTimers::armChannel(unsigned i)
{
	Channel &ch = channels_[i];
	if (scheduler_ == nullptr || armed_[i] || !enabled(i) || cascade(i))
		return;
	const u32 wait = prescaler(ch.control) - ch.phase;
	if (wait == 0) {
		// Cannot happen: phase is kept below the prescaler, and a zero here
		// would mean the prescaler shrank under a pending event. Clock it
		// inline rather than spinning.
		clockChannel(i);
		return;
	}
	armed_[i] = true;
	const u32 gen = ++gen_[i];
	scheduler_->schedule(wait, [this, i, gen] { onTimerTick(i, gen); });
}

void GbaTimers::onTimerTick(unsigned i, u32 gen)
{
	if (gen != gen_[i]) {
		// The control register changed under this event (new prescaler or a
		// disable): a newer event owns the channel now.
		return;
	}
	armed_[i] = false;
	channels_[i].phase = 0;
	++increments_;
	clockChannel(i);
	// clockChannel can cascade into the next channel, and the overflow hook
	// can touch the audio unit, so re-arm everything from scratch.
	armAll();
}

void GbaTimers::armAll()
{
	for (unsigned i = 0; i < kCount; i++)
		armChannel(i);
}

void GbaTimers::rearmEvents()
{
	for (unsigned i = 0; i < kCount; i++)
		armed_[i] = false;
	armAll();
}

u32 GbaTimers::ticks_to_next(unsigned i) const
{
	return prescaler(channels_[i].control) - channels_[i].phase;
}

void GbaTimers::invalidate(unsigned i)
{
	// Bump the generation so a pending event becomes a no-op, then re-arm with
	// the new prescaler.
	++gen_[i];
	armed_[i] = false;
}

void GbaTimers::step(u32 cycles)
{
	// With a scheduler the events own the counting (see armChannel); without
	// one - unit tests, headless bring-up - fall back to counting here so the
	// timers still run.
	if (scheduler_ == nullptr) {
		for (unsigned i = 0; i < kCount; i++) {
			if (!enabled(i) || cascade(i))
				continue;
			Channel &ch = channels_[i];
			ch.phase += cycles;
			const u32 scale = prescaler(ch.control);
			while (ch.phase >= scale) {
				ch.phase -= scale;
				clockChannel(i);
			}
		}
		return;
	}
	(void) cycles;
	armAll();
}

void GbaTimers::clockChannel(unsigned i)
{
	Channel &ch = channels_[i];
	if (ch.counter == 0xFFFFu) {
		ch.counter = ch.reload;
		overflow(i);
	} else {
		ch.counter++;
	}
}

void GbaTimers::overflow(unsigned i)
{
	if ((channels_[i].control & kIrqEnable) != 0 && irq_ != nullptr) {
		irq_->raise(GbaIrq::kTimer0 << i);
	}
	if (overflow_hook_)
		overflow_hook_(i);
	// Cascade: the next timer counts this overflow (prescaler ignored).
	if (i + 1 < kCount && enabled(i + 1) && cascade(i + 1)) {
		clockChannel(i + 1);
	}
}

u16 GbaTimers::read(u32 addr) const
{
	const unsigned i = (addr - kIoBase) / 4;
	if (i >= kCount)
		return 0xFFFF;
	const bool low = ((addr - kIoBase) & 2) == 0;
	return low ? channels_[i].counter : channels_[i].control;
}

void GbaTimers::write(u32 addr, u16 value)
{
	const unsigned i = (addr - kIoBase) / 4;
	if (i >= kCount)
		return;
	Channel &ch = channels_[i];
	if (((addr - kIoBase) & 2) == 0) {
		// CNT_L: sets reload only (GBATEK); counter follows on overflow
		// or on the 0->1 enable edge below.
		ch.reload = value;
		return;
	}
	const bool was = enabled(i);
	ch.control = value & kControlMask;
	if (!was && enabled(i)) {
		// 0->1 edge (or combined 32-bit reload+start): load the counter.
		ch.counter = ch.reload;
		ch.phase = 0;
	}
	// GBATEK "GBA Timers": writing CNT_H restarts the prescaler progress, and
	// any event armed for the previous period (or for a now-disabled
	// channel) is no longer valid - invalidate and re-arm from scratch.
	ch.phase = 0;
	invalidate(i);
	armAll();
}

void GbaTimers::save(StateBuffer &out) const
{
	for (unsigned i = 0; i < kCount; i++) {
		const auto &c = channels_[i];
		out.write(c.reload);
		out.write(c.counter);
		out.write(c.control);
		out.write(c.phase);
	}
}

void GbaTimers::load(const StateBuffer &in)
{
	for (unsigned i = 0; i < kCount; i++) {
		auto &c = channels_[i];
		in.read(c.reload);
		in.read(c.counter);
		in.read(c.control);
		in.read(c.phase);
	}
}

} // namespace gba
