// Headless test harness (--debug): core-only ROM execution for automated
// testing. No window, no Vulkan, no audio.

#include "debug_runner.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "gb/apu.h"
#include "gb/cartridge.h"
#include "gb/cpu.h"
#include "gb/joypad.h"
#include "gb/mmu.h"
#include "gb/ppu.h"
#include "gb/timer.h"

namespace gb {
namespace {

void write_wav(const char* path, const std::vector<i16>& samples) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::printf("cannot open %s\n", path);
        return;
    }
    const u32 data_bytes = static_cast<u32>(samples.size() * sizeof(i16));
    const u32 riff_size = 36 + data_bytes;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riff_size, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    const u32 fmt_size = 16;
    const u16 audio_fmt = 1, channels = 2;
    const u32 rate = 44100;
    const u32 byte_rate = rate * channels * sizeof(i16);
    const u16 block_align = channels * sizeof(i16);
    const u16 bits = 16;
    std::fwrite(&fmt_size, 4, 1, f);
    std::fwrite(&audio_fmt, 2, 1, f);
    std::fwrite(&channels, 2, 1, f);
    std::fwrite(&rate, 4, 1, f);
    std::fwrite(&byte_rate, 4, 1, f);
    std::fwrite(&block_align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&data_bytes, 4, 1, f);
    std::fwrite(samples.data(), sizeof(i16), samples.size(), f);
    std::fclose(f);
}

void write_ppm(const char* path, const PPU::FrameData& frame) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::printf("cannot open %s\n", path);
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    for (u32 px : frame.pixels) {
        const u8 rgb[3] = {
            static_cast<u8>((px >> 16) & 0xFF),
            static_cast<u8>((px >> 8) & 0xFF),
            static_cast<u8>(px & 0xFF),
        };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

Joypad::Key key_from_name(const char* n) {
    if (!std::strcmp(n, "Right")) return Joypad::Key::Right;
    if (!std::strcmp(n, "Left")) return Joypad::Key::Left;
    if (!std::strcmp(n, "Up")) return Joypad::Key::Up;
    if (!std::strcmp(n, "Down")) return Joypad::Key::Down;
    if (!std::strcmp(n, "A")) return Joypad::Key::A;
    if (!std::strcmp(n, "B")) return Joypad::Key::B;
    if (!std::strcmp(n, "Select")) return Joypad::Key::Select;
    return Joypad::Key::Start;
}

}  // namespace

int run_debug(const DebugOptions& opts) {
    Cartridge cart;
    if (!cart.load(opts.rom_path)) {
        std::printf("failed to load %s\n", opts.rom_path.c_str());
        return 2;
    }

    MMU mmu;
    CPU cpu;
    PPU ppu;
    APU apu;
    Timer timer;
    Joypad joypad;

    mmu.set_cartridge(&cart);
    mmu.set_ppu(&ppu);
    mmu.set_apu(&apu);
    mmu.set_timer(&timer);
    mmu.set_joypad(&joypad);
    cpu.set_mmu(&mmu);
    ppu.set_mmu(&mmu);
    apu.set_mmu(&mmu);
    timer.set_mmu(&mmu);
    joypad.set_mmu(&mmu);

    // CGB mode detection: set hardware mode on all components
    HardwareMode mode = cart.hardware_mode();
    // Direct-boot init per Pan Docs Power_Up_Sequence (freebios, no boot ROM)
    cpu.reset(mode);
    mmu.set_hardware_mode(mode);
    ppu.set_hardware_mode(mode);
    // CPU starts in normal speed; KEY1 will toggle
    cpu.double_speed = false;
    cpu.clock_speed_hz = kDefaultDmgClockHz;

    // Skip the boot ROM: post-boot state, PC=0x0100 (see CPU::reset).
    mmu.boot_rom_enabled = false;

    u64 ran = 0;
    u64 frames = 0;
    std::vector<i16> wav;
    const u64 wav_target_frames = opts.wav_seconds * 60;
    if (opts.wav_seconds > 0) wav.reserve(opts.wav_seconds * 44100 * 2);
    // Blargg shells park in a 1-instruction infinite loop on exit (e.g. JR -2
    // after writing the result code to cart RAM $A000). Detect that as a
    // finished run instead of burning the whole budget. HALT does not count:
    // games halt every frame while waiting for VBlank. The 1M threshold is
    // ~60 frames of identical PC, which working games never do.
    u16 last_pc = 0;
    u64 same_pc = 0;
    bool parked = false;
    while (ran < opts.max_mcycles) {
        if (cpu.regs.pc == last_pc) {
            if (++same_pc == 1000000) {
                const u16 pc = cpu.regs.pc;
                const u8 op = mmu.read(pc);
                const bool self_loop =
                    (op == 0x18 && mmu.read(pc + 1) == 0xFE) ||  // JR -2
                    (op == 0xC3 && mmu.read(pc + 1) == (pc & 0xFF) &&
                     mmu.read(pc + 2) == (pc >> 8));             // JP $
                if (self_loop) {
                    parked = true;
                    break;
                }
                same_pc = 0;  // Not a trap; keep watching.
            }
        } else {
            last_pc = cpu.regs.pc;
            same_pc = 0;
        }
        u32 mc = cpu.step();
        timer.step(mc);
        ppu.step(mc);
        apu.step(mc);
        ran += mc;
        if (ppu.frame_ready()) {
            ppu.clear_frame_ready();
            frames++;
            for (const auto& ev : opts.script) {
                if (ev.frame == frames) {
                    joypad.set_key(key_from_name(ev.key.c_str()), ev.pressed);
                }
            }
            if (!opts.wav_path.empty()) apu.take_samples(wav);
            if (!opts.ppm_path.empty() && frames == opts.ppm_frames) {
                write_ppm(opts.ppm_path.c_str(), ppu.frame());
                std::printf("wrote %s after %llu frames\n", opts.ppm_path.c_str(),
                            (unsigned long long)frames);
                break;
            }
            if (!opts.wav_path.empty() && frames >= wav_target_frames) {
                write_wav(opts.wav_path.c_str(), wav);
                std::printf("wrote %s (%llu frames, %zu samples)\n", opts.wav_path.c_str(),
                            (unsigned long long)frames, wav.size());
                break;
            }
        }
        const std::string& log = mmu.serial_log;
        if (log.find("Passed") != std::string::npos) break;
        if (log.find("Failed") != std::string::npos) break;
    }

    std::printf("--- serial (%llu mcycles, %llu frames%s) ---\n%s\n--- end ---\n",
                (unsigned long long)ran, (unsigned long long)frames,
                parked ? ", parked at exit trap" : "", mmu.serial_log.c_str());

    // Test shells that exit silently (e.g. dmg_sound singles) leave a result
    // code at cart RAM $A000 (0 = pass) plus text at $A004.
    const u8 cart_result = cart.read_ram(0x0000);
    std::string cart_text;
    for (u16 a = 0x0004; a < 0x0044; a++) {
        const char c = static_cast<char>(cart.read_ram(a));
        if (c == '\0') break;
        cart_text.push_back(c);
    }
    if (parked) {
        std::printf("cart-result: %u %s\n", cart_result, cart_text.c_str());
    }

    const std::string& log = mmu.serial_log;
    if (log.find("Failed") != std::string::npos) return 1;
    if (log.find("Passed") != std::string::npos) return 0;
    if (!opts.ppm_path.empty() && frames >= opts.ppm_frames) return 0;
    if (!opts.wav_path.empty() && frames >= wav_target_frames) return 0;
    if (parked && cart_result != 0 && cart_result != 0xFF) {
        std::printf("failed test #%u\n", cart_result);
        return 1;
    }
    if (parked && cart_result == 0) return 0;
    return 2;
}

}  // namespace gb
