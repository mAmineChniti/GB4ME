// GBA scheduler: tick-ordered FIFO event queue.

#include "gba/scheduler.h"

namespace gba
{

GbaScheduler::GbaScheduler()
{
	reset();
}

void GbaScheduler::reset()
{
	now_ = 0;
	seq_ = 0;
	queue_ = decltype(queue_)();
}

void GbaScheduler::schedule(u64 delay, Callback cb)
{
	queue_.push(Event{now_ + delay, seq_++, std::move(cb)});
}

void GbaScheduler::step(u64 target)
{
	while (!queue_.empty() && queue_.top().tick <= target) {
		Event ev = queue_.top();
		queue_.pop();
		now_ = ev.tick;
		ev.cb();
	}
	now_ = target;
}

void GbaScheduler::save(StateBuffer &out) const
{
	out.write(now_);
	out.write(seq_);
	// Event queue is intentionally not serialized: callbacks are
	// type-erased std::functions and cannot round-trip. Producers therefore
	// re-arm from their own serialized state on load - the PPU does this in
	// GbaPpu::rearmEvents(), called by GameBoyAdvance::loadState() AFTER the
	// clock is restored (arming against the wrong "now" would put the event
	// in the wrong place and stall the frame). Any new producer owes the
	// same, or its events must become serializable.
}

void GbaScheduler::load(const StateBuffer &in)
{
	in.read(now_);
	in.read(seq_);
	queue_ = decltype(queue_)();
}

} // namespace gba
