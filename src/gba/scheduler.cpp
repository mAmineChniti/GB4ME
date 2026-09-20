// GBA scheduler: tick-ordered FIFO event queue.

#include "gba/scheduler.h"

namespace gba {

GbaScheduler::GbaScheduler() {
    reset();
}

void GbaScheduler::reset() {
    now_ = 0;
    seq_ = 0;
    queue_ = decltype(queue_)();
}

void GbaScheduler::schedule(u64 delay, Callback cb) {
    queue_.push(Event{now_ + delay, seq_++, std::move(cb)});
}

void GbaScheduler::step(u64 target) {
    while (!queue_.empty() && queue_.top().tick <= target) {
        Event ev = queue_.top();
        queue_.pop();
        now_ = ev.tick;
        ev.cb();
    }
    now_ = target;
}

} // namespace gba
