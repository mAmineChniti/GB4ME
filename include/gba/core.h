#pragma once

#include "gb/types.h"
#include "gba/bus.h"
#include "gba/cartridge.h"
#include "gba/scheduler.h"
#include <string>
#include <vector>

namespace gba {

// GameBoyAdvance core skeleton (Phase 1: load/reset/step plumbing only).
// Owns the GBA components; execution devices (ARM7TDMI, PPU, DMA, timers,
// IRQ controller, APU, HLE BIOS) attach in Phases 2-8 behind this same
// interface, so the frontend never touches components directly.
// Deliberately mirrors the gb::GameBoy ownership style (member objects,
// pointer-free wiring) instead of inventing a second pattern.
class GameBoyAdvance {
public:
    GameBoyAdvance();

    bool load(const std::string& path);
    bool loadFromBytes(const std::vector<u8>& data);
    void reset();

    // Advance `cycles` ticks (1 tick = 1 16.78MHz cycle). Until the CPU
    // lands (Phase 2) this only drains the scheduler; devices keep it busy
    // from Phase 5 on.
    void step(u32 cycles);

    bool loaded() const { return loaded_; }
    u64 tick() const { return scheduler_.now(); }
    const Cartridge& cartridge() const { return cartridge_; }
    GbaBus& bus() { return bus_; }
    const GbaBus& bus() const { return bus_; }
    GbaScheduler& scheduler() { return scheduler_; }

private:
    Cartridge cartridge_;
    GbaBus bus_;
    GbaScheduler scheduler_;
    bool loaded_ = false;
};

} // namespace gba
