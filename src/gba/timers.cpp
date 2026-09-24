// GBA timers: prescaled counting, cascade, overflow IRQ.

#include "gba/timers.h"
#include "gba/irq.h"

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
}

void GbaTimers::step(u32 cycles)
{
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
