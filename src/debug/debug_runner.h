#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gb {

// Headless test harness compiled into the GB4ME binary and selected with
// `--debug`. Runs a ROM with no window/audio (core only): captures the
// serial log (Blargg ROMs print Passed/Failed there), dumps framebuffer
// screenshots, and replays scripted joypad input.
//
// Exit codes: 0 = "Passed" in log (or screenshot taken), 1 = "Failed", 2 =
// timeout/unclear.

struct DebugKeyEvent {
    uint64_t frame = 0;
    std::string key;
    bool pressed = false;
};

struct DebugOptions {
    std::string rom_path;
    uint64_t max_mcycles = 100'000'000;
    std::string ppm_path;    // Empty = no screenshot.
    uint64_t ppm_frames = 0;
    std::vector<DebugKeyEvent> script;
    std::string wav_path;    // Empty = no audio capture.
    uint64_t wav_seconds = 0;
};

int run_debug(const DebugOptions& opts);

}  // namespace gb
