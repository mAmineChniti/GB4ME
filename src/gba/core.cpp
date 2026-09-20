// GBA core skeleton: owns bus/scheduler/cartridge, wires ROM into the bus.

#include "gba/core.h"

namespace gba {

GameBoyAdvance::GameBoyAdvance() {
    reset();
}

bool GameBoyAdvance::load(const std::string& path) {
    Cartridge cart;
    if (!cart.load(path)) return false;
    if (!loadFromBytes(cart.rom())) return false;
    return true;
}

bool GameBoyAdvance::loadFromBytes(const std::vector<u8>& data) {
    if (!cartridge_.loadFromBytes(data)) {
        loaded_ = false;
        return false;
    }
    reset();
    loaded_ = true;
    return true;
}

void GameBoyAdvance::reset() {
    // Order matters: bus first (clears RAM/devices), then attach the image,
    // then scheduler. HLE BIOS startup (Phase 4) takes over the CPU state
    // and jumps to the ROM entry from here.
    bus_.reset();
    scheduler_.reset();
    if (cartridge_.loaded()) {
        bus_.attachRom(cartridge_.rom().data(), cartridge_.rom().size());
    } else {
        bus_.detachRom();
    }
}

void GameBoyAdvance::step(u32 cycles) {
    scheduler_.step(scheduler_.now() + cycles);
}

} // namespace gba
