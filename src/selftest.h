#pragma once

#include <string>

namespace gb
{

// Built-in self test (`GB4ME --selftest [rom]`). Exercises the GBA core
// (HLE BIOS/SWIs, ARM+THUMB execution, exceptions, IRQ, bus, PPU, DMA,
// timers, keypad, save state) and the GB/GBC core (SM83, MMU banking,
// interrupts, CGB palettes/double speed). Returns 0 when every check
// passed, 1 otherwise. When `rom_path` is non-empty the ROM is also booted
// and its progress reported (a smoke test, never a hard failure).
//
// Kept in the binary (like --debug) so hardware behaviour can be verified
// without an external test framework or window.
int run_selftest(const std::string &rom_path);

} // namespace gb
