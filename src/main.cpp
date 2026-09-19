#include <cstdio>
#include <cstring>
#include <string>

#include "debug/debug_runner.h"
#include "gb/gameboy.h"

namespace {

void usage(const char* prog) {
    std::printf("usage:\n");
    std::printf("  %s [rom.gb]              play in a window (ROM menu if omitted)\n", prog);
    std::printf("  %s --list-roms <folder>  print playable ROMs in a folder\n", prog);
    std::printf("  %s <rom.gb> --debug [opts]  run headless (no window) for testing\n",
                prog);
    std::printf("debug options:\n");
    std::printf("  [max_mcycles]              cycle budget (default 100000000)\n");
    std::printf("  --ppm <file> --frames <N>  save screenshot after N frames\n");
    std::printf("  --wav <file> --seconds <N> capture N seconds of audio\n");
    std::printf("  --press <F>:<Key>          hold Key at frame F\n");
    std::printf("  --release <F>:<Key>        release Key at frame F\n");
    std::printf("  Keys: Right Left Up Down A B Select Start\n");
    std::printf("exit codes (debug): 0=Passed 1=Failed 2=timeout/error\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool debug = false;
    std::string rom_path;
    std::string list_folder;
    gb::DebugOptions dopts;
    bool have_cycles = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--debug")) {
            debug = true;
        } else if (!std::strcmp(argv[i], "--list-roms") && i + 1 < argc) {
            list_folder = argv[i + 1];
            i += 1;
        } else if (debug && !std::strcmp(argv[i], "--ppm") && i + 3 < argc &&
                   !std::strcmp(argv[i + 2], "--frames")) {
            dopts.ppm_path = argv[i + 1];
            dopts.ppm_frames = std::stoull(argv[i + 3]);
            i += 3;
        } else if (debug && !std::strcmp(argv[i], "--wav") && i + 3 < argc &&
                   !std::strcmp(argv[i + 2], "--seconds")) {
            dopts.wav_path = argv[i + 1];
            dopts.wav_seconds = std::stoull(argv[i + 3]);
            i += 3;
        } else if (debug && (!std::strcmp(argv[i], "--press") ||
                             !std::strcmp(argv[i], "--release")) &&
                   i + 1 < argc) {
            const char* spec = argv[i + 1];
            const char* colon = std::strchr(spec, ':');
            if (colon != nullptr) {
                dopts.script.push_back({std::stoull(std::string(spec, colon)), colon + 1,
                                        !std::strcmp(argv[i], "--press")});
            }
            i += 1;
        } else if (argv[i][0] != '-' && rom_path.empty()) {
            rom_path = argv[i];
        } else if (debug && argv[i][0] != '-' && !have_cycles) {
            dopts.max_mcycles = std::stoull(argv[i]);
            have_cycles = true;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (debug) {
        if (rom_path.empty()) {
            usage(argv[0]);
            return 1;
        }
        dopts.rom_path = rom_path;
        return gb::run_debug(dopts);
    }

    if (!list_folder.empty()) {
        for (const auto& e : gb::GameBoy::scan_rom_folder(list_folder)) {
            std::printf("%s | %s | %s\n", e.title.c_str(), e.path.c_str(),
                        e.cgb ? "CGB" : "DMG");
        }
        return 0;
    }

    gb::GameBoy gb;
    if (!gb.initialize()) {
        std::printf("failed to initialize (video)\n");
        return 1;
    }
    if (!rom_path.empty() && !gb.load_rom(rom_path)) {
        std::printf("failed to load '%s' — opening ROM menu\n", rom_path.c_str());
    }
    gb.run();
    gb.shutdown();
    return 0;
}
