#pragma once

#include "gb/types.h"
#include <functional>
#include <queue>
#include <vector>

namespace gba {

// Cycle/event scheduler foundation (Phase 1).
// The GB core fans fixed M-cycle counts per frame; GBA DMA-by-HBlank, FIFO
// underrun and timer cascade need finer-grained events, so the GBA core
// schedules work on a 1-cycle tick grid. PPU/DMA/timers/APU attach in
// Phases 5-6/8; this file only provides ordering + stepping.
class GbaScheduler {
public:
    using Callback = std::function<void()>;

    GbaScheduler();

    void reset();
    u64 now() const { return now_; }
    bool empty() const { return queue_.empty(); }
    size_t pending() const { return queue_.size(); }

    // Run `cb` after `delay` ticks (delay 0 runs on the next step() call).
    void schedule(u64 delay, Callback cb);
    // Advance to target (must be >= now), running all due events in order.
    // Same-tick events run in schedule order (FIFO).
    void step(u64 target);

private:
    struct Event {
        u64 tick;
        u64 seq;
        Callback cb;
    };
    struct Compare {
        bool operator()(const Event& a, const Event& b) const {
            if (a.tick != b.tick) return a.tick > b.tick;
            return a.seq > b.seq;
        }
    };

    u64 now_ = 0;
    u64 seq_ = 0;
    std::priority_queue<Event, std::vector<Event>, Compare> queue_;
};

} // namespace gba
