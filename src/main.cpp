#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "gb/gameboy.h"
#include "gba/core.h"
#include "gba/test_verdict.h"

#if !defined(GB4ME_RELEASE) || defined(GB4ME_KEEP_SELFTEST)
#include "selftest.h"
#endif

namespace
{

#if !defined(GB4ME_RELEASE) || defined(GB4ME_KEEP_SELFTEST)
void usage(const char *prog)
{
	std::printf("usage:\n");
	std::printf("  %s [rom.gb]              play in a window (ROM menu if omitted)\n", prog);
	std::printf("  %s --selftest [rom]      run the built-in GB/GBC/GBA hardware self test\n",
				prog);
	std::printf("  %s --list-roms <folder>  print playable ROMs in a folder\n", prog);
	std::printf("  %s --dump-gba <rom> <frames> <prefix>\n", prog);
	std::printf("                           headless: run <frames>, write <prefix>.ppm +\n");
	std::printf("                           <prefix>.txt (video regs, OAM, audio state)\n");
	std::printf("  %s --trace-gba <rom> <frames>\n", prog);
	std::printf("                           headless: run <frames>, report the first PC\n");
	std::printf("                           outside every legal GBA region (boot triage)\n");
	std::printf("  %s --blargg <rom> [frames]\n", prog);
	std::printf("                           headless: run a Blargg cpu_instrs test ROM\n");
	std::printf("                           until its serial output says Passed/Failed\n");
	std::printf("                           (default 3600 frames); exit 0/1\n");
	std::printf("  %s --dump-gb <rom> <frames> <prefix>\n", prog);
	std::printf("                           headless: run <frames>, write <prefix>.ppm\n");
	std::printf("                           (160x144) plus any serial log\n");
	std::printf("  %s --gba-suite <rom> [suite] [frames]\n", prog);
	std::printf("                           headless: run an mgba-emu/suite ROM through\n");
	std::printf("                           its menu into suite <suite> (default 0) until\n");
	std::printf("                           the debug log reports END: passed/total\n");
	std::printf("                           (default 3600 frames); exit 0/1\n");
	std::printf("  %s --gba-tests <rom> [frames]\n", prog);
	std::printf("                           headless: run a jsmolka/gba-tests ROM until\n");
	std::printf("                           its mode-4 screen reports a verdict (default\n");
	std::printf("                           600 frames); exit 0/1\n");
	std::printf("  optional: --input \"frame:key,...\" --hold N --snap N\n");
	std::printf("           injects key presses to advance past title/name-entry\n");
	std::printf("           screens; key in A,B,S,E,U,D,L,R,H,J (A,B,START,SELECT,UP,\n");
	std::printf("           DOWN,LEFT,RIGHT,L-Shoulder,R-Shoulder). --snap N writes a\n");
	std::printf("           frame every N frames as <prefix>_<frame>.ppm\n");
	std::printf(
		"format support: .gb (DMG) .gbc (CGB) .gba (GBA, built-in HLE BIOS — no firmware\n");
	std::printf("files are needed or requested)\n");
	std::printf("exit codes (--selftest): 0 = all checks passed, 1 = failures\n");
}
#endif // dev CLI

#if !defined(GB4ME_RELEASE) || defined(GB4ME_KEEP_SELFTEST)
// Headless GBA state dump (Phase 7 diagnostics): framebuffer plus the
// video/audio state needed to root-cause rendering and sound issues
// without a display.
//
// `script` optionally injects key presses so a game can be advanced past
// title/name-entry screens into gameplay that is otherwise unreachable
// headless (Golden Sun stops at name entry with no input, so its battle
// sprites could never be observed). Format: "frame:key,frame:key,..." with
// key in {A,B,START,SELECT,UP,DOWN,LEFT,RIGHT,L,R}. Each press is held for
// `holdFrames` frames.
// TEMPORARY diagnostic: run frames and report the first PC outside every
// legal GBA region, with the preceding PCs for context.
// Headless boot triage: run `frames` and report the first PC outside every
// legal GBA region (BIOS/IWRAM/EWRAM/IO/ROM), with the preceding PCs. A game
// that boots walks off into open bus and slides through 0x00000000 there; the
// context lines point at the instruction that jumped. Found the DMA
// completion-timing regression this way (see agents.md).
int trace_gba(const char *rom, int frames)
{
	gba::GameBoyAdvance gba;
	if (!gba.load(rom)) {
		std::printf("failed to load '%s'\n", rom);
		return 1;
	}
	auto legal = [](u32 pc) {
		return (pc < 0x00010000u) || (pc >= 0x02000000u && pc < 0x02010000u) ||
			   (pc >= 0x03000000u && pc < 0x03008000u) || (pc >= 0x04000000u && pc < 0x04010000u) ||
			   (pc >= 0x08000000u && pc < 0x0E000000u);
	};
	const int trace_from = 0;
	std::vector<std::string> ring;
	for (int f = 0; f < frames;) {
		const u32 pc = gba.cpu().pc();
		char buf[16];
		std::snprintf(buf, sizeof buf, "%c%08X",
					  gba.cpu().state() == gba::CpuState::Thumb ? 'T' : 'A', pc);
		ring.emplace_back(buf);
		if (ring.size() > 24)
			ring.erase(ring.begin());
		if (!legal(pc)) {
			std::printf("%s: first illegal PC %s after %d frames\n", rom, buf, f);
			for (const std::string &line : ring)
				std::printf("  %s\n", line.c_str());
			return 1;
		}
		if (f >= trace_from)
			std::printf("%c%08X r0=%08X r1=%08X r2=%08X r3=%08X sp=%08X lr=%08X\n",
						gba.cpu().state() == gba::CpuState::Thumb ? 'T' : 'A', pc, gba.cpu().reg(0),
						gba.cpu().reg(1), gba.cpu().reg(2), gba.cpu().reg(3), gba.cpu().reg(13),
						gba.cpu().reg(14));
		gba.step(1);
		if (gba.ppu().frameReady()) {
			gba.ppu().clearFrameReady();
			++f;
			const u64 c11 = gba.swiHistogram()[11];
			std::printf("frame %d cpuset=%llu tick=%llu pc=%08X\n", f, (unsigned long long) c11,
						(unsigned long long) gba.tick(), gba.cpu().pc());
		}
	}
	std::printf("%s: no illegal PC in %d frames (ends at %s)\n", rom, frames,
				ring.empty() ? "no instructions executed" : ring.back().c_str());
	return 0;
}

int dump_gba(const char *rom, int frames, const char *prefix, const char *script, int holdFrames,
			 int snap)
{
	gba::GameBoyAdvance gba;
	if (!gba.load(rom)) {
		std::printf("failed to load '%s'\n", rom);
		return 1;
	}
	// Parse the input script into frame -> key(s).
	std::vector<std::pair<int, std::vector<gba::GbaKeypad::Key>>> events;
	if (script != nullptr && script[0] != '\0') {
		std::string s(script);
		size_t pos = 0;
		while (pos < s.size()) {
			const size_t colon = s.find(':', pos);
			if (colon == std::string::npos)
				break;
			const size_t comma = s.find(',', colon);
			const std::string frameStr = s.substr(pos, colon - pos);
			const std::string keyStr = s.substr(
				colon + 1, (comma == std::string::npos) ? std::string::npos : comma - colon - 1);
			const int f = std::atoi(frameStr.c_str());
			std::vector<gba::GbaKeypad::Key> keys;
			for (const char *p2 = keyStr.c_str(); *p2; ++p2) {
				switch (*p2) {
				case 'a':
				case 'A':
					keys.push_back(gba::GbaKeypad::Key::A);
					break;
				case 'b':
				case 'B':
					keys.push_back(gba::GbaKeypad::Key::B);
					break;
				case 's':
				case 'S':
					keys.push_back(gba::GbaKeypad::Key::Start);
					break;
				case 'e':
				case 'E':
					keys.push_back(gba::GbaKeypad::Key::Select);
					break;
				case 'u':
				case 'U':
					keys.push_back(gba::GbaKeypad::Key::Up);
					break;
				case 'd':
				case 'D':
					keys.push_back(gba::GbaKeypad::Key::Down);
					break;
				case 'l':
				case 'L':
					keys.push_back(gba::GbaKeypad::Key::Left);
					break;
				case 'r':
				case 'R':
					keys.push_back(gba::GbaKeypad::Key::Right);
					break;
				case 'h':
				case 'H':
					keys.push_back(gba::GbaKeypad::Key::L);
					break;
				case 'j':
				case 'J':
					keys.push_back(gba::GbaKeypad::Key::R);
					break;
				default:
					break;
				}
			}
			if (!keys.empty())
				events.push_back({f, keys});
			if (comma == std::string::npos)
				break;
			pos = comma + 1;
		}
	}
	// Track which keys are still held so a press spans holdFrames.
	std::vector<gba::GbaKeypad::Key> held;
	for (int f = 0; f < frames; ++f) {
		for (const auto &e : events) {
			if (e.first != f)
				continue;
			for (auto k : e.second)
				held.push_back(k);
		}
		const u64 start = gba.tick();
		while (!gba.ppu().frameReady() && gba.tick() - start < 1200000)
			gba.step(4096);
		gba.ppu().clearFrameReady();
		// Release anything whose hold window has expired.
		if (f > 0 && (f % (holdFrames > 0 ? holdFrames : 1)) == 0 && !held.empty()) {
			for (auto k : held)
				gba.keypad().setKey(k, false);
			held.clear();
		}
		for (auto k : held)
			gba.keypad().setKey(k, true);
		// Optional periodic frame snapshots, so one long run can show a
		// frame-to-frame sequence without re-running from frame 0.
		if (snap > 0 && (f % snap) == 0) {
			char name[64];
			std::snprintf(name, sizeof(name), "%s_%06d", prefix, f);
			FILE *sf = std::fopen((std::string(name) + ".ppm").c_str(), "wb");
			if (sf != nullptr) {
				std::fprintf(sf, "P6\n240 160\n255\n");
				for (u32 px : gba.ppu().frame().pixels) {
					const u8 rgb[3] = {static_cast<u8>((px >> 16) & 0xFF),
									   static_cast<u8>((px >> 8) & 0xFF),
									   static_cast<u8>(px & 0xFF)};
					std::fwrite(rgb, 1, 3, sf);
				}
				std::fclose(sf);
			}
		}
	}
	std::string ppm = std::string(prefix) + ".ppm";
	FILE *o = std::fopen(ppm.c_str(), "wb");
	if (o == nullptr) {
		std::printf("failed to write '%s'\n", ppm.c_str());
		return 1;
	}
	std::fprintf(o, "P6\n240 160\n255\n");
	for (u32 px : gba.ppu().frame().pixels) {
		const u8 rgb[3] = {static_cast<u8>((px >> 16) & 0xFF), static_cast<u8>((px >> 8) & 0xFF),
						   static_cast<u8>(px & 0xFF)};
		std::fwrite(rgb, 1, 3, o);
	}
	std::fclose(o);
	std::string txt = std::string(prefix) + ".txt";
	o = std::fopen(txt.c_str(), "w");
	if (o == nullptr) {
		std::printf("failed to write '%s'\n", txt.c_str());
		return 1;
	}
	std::fprintf(o, "pc=%08X dispcnt=%04X\n", gba.cpu().pc(), gba.bus().read16(0x04000000));
	for (u32 b = 0; b < 4; ++b)
		std::fprintf(o, "bg%dcnt=%04X hofs=%04X vofs=%04X\n", b,
					 gba.bus().read16(0x04000008 + 2 * b), gba.ppu().scrollH(b),
					 gba.ppu().scrollV(b));
	std::fprintf(o, "winin=%04X winout=%04X bldcnt=%04X bldalpha=%04X\n",
				 gba.bus().read16(0x04000048), gba.bus().read16(0x0400004A),
				 gba.bus().read16(0x04000050), gba.bus().read16(0x04000052));
	std::fprintf(
		o, "bg2pa=%04X pb=%04X pc=%04X pd=%04X x=%08X y=%08X\n", gba.ppu().affineParam(0, 0),
		gba.ppu().affineParam(0, 1), gba.ppu().affineParam(0, 2), gba.ppu().affineParam(0, 3),
		static_cast<unsigned>(gba.ppu().affineX(0)), static_cast<unsigned>(gba.ppu().affineY(0)));
	std::fprintf(o, "sndh=%04X nr50=%02X nr51=%02X tm0c=%04X tm1c=%04X dma1h=%04X dma2h=%04X\n",
				 gba.bus().read16(0x04000082), gba.bus().read16(0x04000080),
				 gba.bus().read16(0x04000081), gba.bus().read16(0x04000102),
				 gba.bus().read16(0x04000106), gba.bus().read16(0x040000BE),
				 gba.bus().read16(0x040000CA));
	for (u32 i = 0; i < 128; ++i)
		std::fprintf(o, "oam%03u: %04X %04X %04X xxxx\n", i, gba.bus().read16(0x07000000 + 8 * i),
					 gba.bus().read16(0x07000002 + 8 * i), gba.bus().read16(0x07000004 + 8 * i));
	unsigned vrnz = 0, plnz = 0;
	for (u32 a = 0x06000000; a < 0x06018000; a += 32)
		if (gba.bus().read32(a) != 0)
			++vrnz;
	for (u32 a = 0x05000000; a < 0x05000400; a += 4)
		if (gba.bus().read16(a) != 0)
			++plnz;
	std::fprintf(o, "vram_nonzero32=%u pal_nonzero=%u\n", vrnz, plnz);
	std::fprintf(o, "vram_blocks:");
	for (u32 blk = 0; blk < 24; ++blk) {
		unsigned n = 0;
		for (u32 a = 0x06000000 + blk * 0x1000; a < 0x06001000 + blk * 0x1000; a += 32)
			if (gba.bus().read32(a) != 0)
				++n;
		std::fprintf(o, " %u", n);
	}
	std::fprintf(o, "\n");
	for (u32 b = 0; b < 4; ++b) {
		const u32 map = 0x06000000 + (((gba.bus().read16(0x04000008 + 2 * b) >> 8) & 0x1F) * 0x800);
		std::fprintf(o, "bg%u_map:", b);
		for (u32 k = 0; k < 16; ++k)
			std::fprintf(o, " %04X", gba.bus().read16(map + 2 * k));
		std::fprintf(o, "\n");
	}
	// Full-map nonzero histogram: which tile entries each BG map references
	// (reveals whether a layer's content exists but points at empty tiles).
	for (u32 b = 0; b < 4; ++b) {
		const u32 map = 0x06000000 + (((gba.bus().read16(0x04000008 + 2 * b) >> 8) & 0x1F) * 0x800);
		unsigned distinct = 0, nonzero = 0;
		bool seen[1024] = {false};
		for (u32 k = 0; k < 1024; ++k) {
			const u16 e = gba.bus().read16(map + 2 * k);
			if (e == 0)
				continue;
			++nonzero;
			if (!seen[e & 0x3FFu]) {
				seen[e & 0x3FFu] = true;
				++distinct;
			}
		}
		std::fprintf(o, "bg%u_cells: nonzero=%u distinct_tiles=%u\n", b, nonzero, distinct);
	}
	// Tile occupancy of BG char block 0 (0x06000000, 256 16-color tiles):
	// which tile slots hold any nonzero bytes (upload holes show here).
	std::fprintf(o, "chartiles:");
	for (u32 t = 0; t < 256; t += 16) {
		unsigned mask = 0;
		for (u32 k = 0; k < 16; ++k) {
			if (gba.bus().read32(0x06000000 + (t + k) * 32) != 0)
				mask |= 1u << k;
		}
		std::fprintf(o, " %04X", mask);
	}
	std::fprintf(o, "\n");
	// Logo tilemap window (title Pokemon logo lives at BG screen 9):
	// nonzero byte count tells whether the LZ77 upload landed.
	unsigned tm_nz = 0;
	for (u32 a = 0x06004800; a < 0x06004C00; ++a)
		if (gba.bus().read8(a) != 0)
			++tm_nz;
	std::fprintf(o, "logomap_nonzero=%u/1024\n", tm_nz);
	std::fclose(o);
	return 0;
}
#endif // dev CLI

#if !defined(GB4ME_RELEASE) || defined(GB4ME_KEEP_SELFTEST)
// Headless Blargg cpu_instrs runner: same bring-up as the frontend's
// direct-boot path (no boot ROM), stepped frame by frame with no pacing.
// Blargg tests report over the link cable (SB/SC); the MMU captures those
// bytes into serial_log, where "Passed"/"Failed" appears at the end.
// GB4ME_FORCE_DMG=1 runs even a CGB-flagged ROM on DMG hardware: several
// DMG-only test ROMs (Blargg oam_bug) are wrong under the CGB model.
const bool g_force_dmg = std::getenv("GB4ME_FORCE_DMG") != nullptr;

// Per-M-cycle hook shared by the headless GB harnesses: the CPU drives the
// timer/PPU/APU so register accesses land on hardware's M-cycle.
struct GbCycleHook {
	gb::Timer *timer;
	gb::PPU *ppu;
	gb::APU *apu;
	void onMcycle() const
	{
		timer->step(1);
		ppu->step(1);
		apu->step(1);
	}
};

static void gb_cycle_hook(void *ctx)
{
	static_cast<GbCycleHook *>(ctx)->onMcycle();
}

// Headless GB/CGB screen dump (diagnostics for timing-test triage, mirroring
// dump_gba): runs <frames>, writes <prefix>.ppm (160x144) plus any serial
// log the ROM produced. Uses run_blargg's direct-boot bring-up.
int dump_gb(const char *rom, int frames, const char *prefix)
{
	gb::Cartridge cart;
	if (!cart.load(rom)) {
		std::printf("not a loadable GB/GBC ROM: '%s' (%s)\n", rom,
					cart.last_error().empty() ? "unknown format" : cart.last_error().c_str());
		return 1;
	}
	if (g_force_dmg)
		cart.set_hardware_mode(HardwareMode::DMG);
	gb::MMU mmu;
	gb::CPU cpu;
	gb::PPU ppu;
	gb::APU apu;
	gb::Timer timer;
	gb::Joypad joypad;
	mmu.set_ppu(&ppu);
	mmu.set_apu(&apu);
	mmu.set_timer(&timer);
	mmu.set_joypad(&joypad);
	mmu.set_cartridge(&cart);
	cpu.set_mmu(&mmu);
	ppu.set_mmu(&mmu);
	apu.set_mmu(&mmu);
	timer.set_mmu(&mmu);
	joypad.set_mmu(&mmu);
	const HardwareMode mode = cart.hardware_mode();
	cpu.reset(mode);
	mmu.set_hardware_mode(mode);
	ppu.set_hardware_mode(mode);
	mmu.boot_rom_enabled = false;
	cpu.double_speed = false;
	cpu.clock_speed_hz = kDefaultDmgClockHz;
	GbCycleHook hook{&timer, &ppu, &apu};
	cpu.set_cycle_hook(&gb_cycle_hook, &hook);
	for (int f = 0; f < frames; ++f) {
		// The CPU's hook advances the components; this loop only paces time.
		for (u32 t = 0; t < 70224;)
			t += cpu.step() * 4;
	}
	const std::string ppm = std::string(prefix) + ".ppm";
	FILE *o = std::fopen(ppm.c_str(), "wb");
	if (o == nullptr) {
		std::printf("failed to write '%s'\n", ppm.c_str());
		return 1;
	}
	std::fprintf(o, "P6\n160 144\n255\n");
	for (u32 px : ppu.frame().pixels) {
		const u8 rgb[3] = {static_cast<u8>((px >> 16) & 0xFF), static_cast<u8>((px >> 8) & 0xFF),
						   static_cast<u8>(px & 0xFF)};
		std::fwrite(rgb, 1, 3, o);
	}
	std::fclose(o);
	std::printf("wrote %s (mode %d, serial %zu bytes)\n", ppm.c_str(), static_cast<int>(mode),
				mmu.serial_log.size());
	if (!mmu.serial_log.empty())
		std::printf("%s\n", mmu.serial_log.c_str());
	return 0;
}

int run_blargg(const char *rom, int max_frames)
{
	gb::Cartridge cart;
	if (!cart.load(rom)) {
		std::printf("not a loadable GB/GBC ROM: '%s' (%s)\n", rom,
					cart.last_error().empty() ? "unknown format" : cart.last_error().c_str());
		return 1;
	}
	if (g_force_dmg)
		cart.set_hardware_mode(HardwareMode::DMG);
	gb::MMU mmu;
	gb::CPU cpu;
	gb::PPU ppu;
	gb::APU apu;
	gb::Timer timer;
	gb::Joypad joypad;
	mmu.set_ppu(&ppu);
	mmu.set_apu(&apu);
	mmu.set_timer(&timer);
	mmu.set_joypad(&joypad);
	mmu.set_cartridge(&cart);
	cpu.set_mmu(&mmu);
	ppu.set_mmu(&mmu);
	apu.set_mmu(&mmu);
	timer.set_mmu(&mmu);
	joypad.set_mmu(&mmu);
	const HardwareMode mode = cart.hardware_mode();
	cpu.reset(mode);
	mmu.set_hardware_mode(mode);
	ppu.set_hardware_mode(mode);
	mmu.boot_rom_enabled = false;
	cpu.double_speed = false;
	cpu.clock_speed_hz = kDefaultDmgClockHz;
	GbCycleHook hook{&timer, &ppu, &apu};
	cpu.set_cycle_hook(&gb_cycle_hook, &hook);
	// Blargg's non-serial tests report through cartridge RAM instead ("output
	// to memory" in the test readmes): $A000 is the result code ($80 while
	// still running), $A001-$A003 carry the $DE,$B0,$61 signature and $A004
	// starts a zero-terminated text log. oam_bug, mem_timing, interrupt_time
	// and halt_bug never touch the serial port, so serial alone always times
	// out on them.
	auto ram_report = [&]() {
		if (cart.ram_size() == 0)
			return std::string();
		if (cart.read_ram(1) != 0xDE || cart.read_ram(2) != 0xB0 || cart.read_ram(3) != 0x61)
			return std::string();
		std::string text;
		for (u16 a = 4; a < 0x100; ++a) {
			const u8 c = cart.read_ram(a);
			if (c == 0)
				break;
			text.push_back(static_cast<char>(c));
		}
		return text;
	};
	for (int f = 0; f < max_frames; ++f) {
		// One frame = 70224 T-cycles; cpu.step() returns M-cycles.
		// The CPU's hook advances the components; this loop only paces time.
		for (u32 t = 0; t < 70224;)
			t += cpu.step() * 4;
		const std::string report = ram_report();
		if (!report.empty() && cart.read_ram(0) != 0x80) {
			const u8 code = cart.read_ram(0);
			std::printf("%s: %s (%d frames, result %u)\n%s\n", rom, code == 0 ? "Passed" : "FAILED",
						f + 1, code, report.c_str());
			return code == 0 ? 0 : 1;
		}
		// halt_bug and interrupt_time report only on screen: their console
		// loads the font so a BG tile index is the ASCII code, so the text can
		// be read straight out of the tile map.
		if (ppu.console_shows("Passed")) {
			std::printf("%s: Passed (%d frames, screen)\n", rom, f + 1);
			return 0;
		}
		if (ppu.console_shows("Failed")) {
			// Keep the detail: "Failed" alone says nothing about which subtest, so
			// print the cart-RAM log and the console text along with it.
			const std::string partial = ram_report();
			std::printf("%s: FAILED (%d frames, screen)\n%s%s%s%s", rom, f + 1,
						mmu.serial_log.c_str(), partial.empty() ? "" : "cart-RAM log:\n",
						partial.c_str(), ppu.console_text().c_str());
			return 1;
		}
		const std::string &log = mmu.serial_log;
		if (log.find("Passed") != std::string::npos) {
			std::printf("%s: Passed (%d frames)\n%s\n", rom, f + 1, log.c_str());
			return 0;
		}
		if (log.find("Failed") != std::string::npos) {
			std::printf("%s: FAILED (%d frames)\n%s\n", rom, f + 1, log.c_str());
			return 1;
		}
	}
	// Still running: show the partial text log so a hang is diagnosable.
	const std::string partial = ram_report();
	std::printf("%s: TIMEOUT after %d frames\n%s%s%s\n", rom, max_frames, mmu.serial_log.c_str(),
				partial.empty() ? "" : "cart-RAM log:\n", partial.c_str());
	return 1;
}

// mgba-emu/suite runner: the suite reports over the mGBA MMIO debug channel
// (0x4FFF600 string + 0x4FFF700 flags, NOT SIO), which GbaBus captures into
// debug_log. The suite boots into an interactive menu, so the runner drives
// it: DOWN presses pick suite `suite` (wrapping like the menu), A enters it,
// then the first "END: passed/total" line is the verdict. jsmolka/gba-tests
// instead draw verdict text into the framebuffer - a different runner.
int run_gba_suite(const char *rom, int suite, int max_frames)
{
	gba::GameBoyAdvance gba;
	if (!gba.load(rom)) {
		std::printf("not a loadable GBA ROM: '%s'\n", rom);
		return 1;
	}
	auto step_frame = [&]() {
		const u64 start = gba.tick();
		while (!gba.ppu().frameReady() && gba.tick() - start < 1200000)
			gba.step(4096);
		gba.ppu().clearFrameReady();
	};
	auto hold_key = [&](gba::GbaKeypad::Key k, int frames) {
		gba.keypad().setKey(k, true);
		for (int f = 0; f < frames; ++f)
			step_frame();
		gba.keypad().setKey(k, false);
		for (int f = 0; f < 3; ++f)
			step_frame();
	};
	step_frame();
	for (int f = 0; f < 30; ++f)
		step_frame(); // Suite menu boots.
	for (int i = 0; i < suite; ++i)
		hold_key(gba::GbaKeypad::Key::Down, 3);
	hold_key(gba::GbaKeypad::Key::A, 3);
	for (int f = 0; f < max_frames; ++f) {
		step_frame();
		// The boot banner contains no "END: " line, so searching from the
		// start is safe (same shape as run_blargg's serial scan).
		const std::string &log = gba.bus().debug_log;
		const size_t at = log.find("END: ");
		if (at != std::string::npos) {
			int passed = -1, total = -1;
			if (std::sscanf(log.c_str() + at, "END: %d/%d", &passed, &total) == 2 && total > 0) {
				if (passed == total) {
					std::printf("%s[%d]: Passed (%d frames) %d/%d\n%s\n", rom, suite, f + 1, passed,
								total, log.c_str());
					return 0;
				}
				std::printf("%s[%d]: FAILED (%d frames) %d/%d\n%s\n", rom, suite, f + 1, passed,
							total, log.c_str());
				return 1;
			}
		}
	}
	std::printf("%s[%d]: TIMEOUT after %d frames\n%s\n", rom, suite, max_frames,
				gba.bus().debug_log.c_str());
	return 1;
}

// jsmolka/gba-tests runner: those ROMs need no menu driving and report no
// log - they draw verdict text into mode-4 VRAM ("All tests passed" at
// (56,76), "Failed test NNN" at (60,76)), which parse_gba_test_verdict
// template-matches pixel-exactly. Mirrors run_gba_suite's shape/exit codes.
int run_gba_tests(const char *rom, int max_frames)
{
	gba::GameBoyAdvance gba;
	if (!gba.load(rom)) {
		std::printf("not a loadable GBA ROM: '%s'\n", rom);
		return 1;
	}
	for (int f = 0; f < max_frames; ++f) {
		const u64 start = gba.tick();
		while (!gba.ppu().frameReady() && gba.tick() - start < 1200000)
			gba.step(4096);
		gba.ppu().clearFrameReady();
		const auto &frame = gba.ppu().frame();
		const gba::GbaTestVerdict v = gba::parse_gba_test_verdict(
			frame.pixels.data(), gba::GbaPpu::kWidth, gba::GbaPpu::kHeight);
		if (v.kind == gba::GbaTestVerdictKind::Pass) {
			std::printf("%s: Passed (%d frames)\n", rom, f + 1);
			return 0;
		}
		if (v.kind == gba::GbaTestVerdictKind::Fail) {
			std::printf("%s: FAILED test %d (%d frames)\n%s\n", rom, v.failed_test, f + 1,
						gba.bus().debug_log.c_str());
			return 1;
		}
	}
	std::printf("%s: TIMEOUT after %d frames (no verdict on screen)\n", rom, max_frames);
	return 1;
}
#endif // dev CLI

} // namespace

int main(int argc, char **argv)
{
#if defined(GB4ME_RELEASE) && !defined(GB4ME_KEEP_SELFTEST)
	// Release build: the ROM path is the only argument, and nothing is ever
	// written to stdout/stderr. Unknown flags are ignored rather than
	// explained, so the emulator stays completely silent.
	std::string rom_path;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] != '-')
			rom_path = argv[i];
	}
	gb::GameBoy gb;
	if (!gb.initialize())
		return 1;
	if (!rom_path.empty())
		gb.load_rom(rom_path); // Falls back to the ROM menu on failure.
	gb.run();
	gb.shutdown();
	return 0;
#else  // developer build (or release+keep-selftest): full CLI and dumps
	bool selftest = false;
	std::string rom_path;
	std::string list_folder;
	std::string blargg_rom;
	int blargg_frames = 3600;
	std::string dumpgb_rom;
	int dumpgb_frames = 0;
	std::string dumpgb_prefix;
	std::string tests_rom;
	int tests_frames = 600;
	std::string suite_rom;
	int suite_index = 0;
	int suite_frames = 3600;
	std::string dump_rom;
	int dump_frames = 0;
	std::string dump_prefix;
	std::string dump_input;
	int dump_hold = 6;
	int dump_snap = 0;
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--selftest")) {
			selftest = true;
		} else if (!std::strcmp(argv[i], "--list-roms") && i + 1 < argc) {
			list_folder = argv[i + 1];
			i += 1;
		} else if (!std::strcmp(argv[i], "--blargg") && i + 1 < argc) {
			blargg_rom = argv[i + 1];
			if (i + 2 < argc && argv[i + 2][0] != '-') {
				blargg_frames = std::atoi(argv[i + 2]);
				if (blargg_frames <= 0)
					blargg_frames = 3600;
				i += 1;
			}
		} else if (!std::strcmp(argv[i], "--trace-gba") && i + 2 < argc) {
			return trace_gba(argv[i + 1], std::atoi(argv[i + 2]));
			return 0;
		} else if (!std::strcmp(argv[i], "--dump-gb") && i + 3 < argc) {
			dumpgb_rom = argv[i + 1];
			dumpgb_frames = std::atoi(argv[i + 2]);
			dumpgb_prefix = argv[i + 3];
			i += 3;
			i += 1;
		} else if (!std::strcmp(argv[i], "--gba-suite") && i + 1 < argc) {
			suite_rom = argv[i + 1];
			int consumed = 1;
			if (i + 2 < argc && argv[i + 2][0] != '-') {
				suite_index = std::atoi(argv[i + 2]);
				if (suite_index < 0)
					suite_index = 0;
				consumed += 1;
			}
			if (i + 3 < argc && argv[i + 3][0] != '-') {
				suite_frames = std::atoi(argv[i + 3]);
				if (suite_frames <= 0)
					suite_frames = 3600;
				consumed += 1;
			}
			i += consumed;
		} else if (!std::strcmp(argv[i], "--gba-tests") && i + 1 < argc) {
			tests_rom = argv[i + 1];
			if (i + 2 < argc && argv[i + 2][0] != '-') {
				tests_frames = std::atoi(argv[i + 2]);
				if (tests_frames <= 0)
					tests_frames = 600;
				i += 1;
			}
			i += 1;
		} else if (!std::strcmp(argv[i], "--dump-gba") && i + 3 < argc) {
			dump_rom = argv[i + 1];
			dump_frames = std::atoi(argv[i + 2]);
			dump_prefix = argv[i + 3];
			// Consume any trailing --input "..." / --hold N pairs. Done in
			// a nested loop because the outer loop's own increment would
			// otherwise step over the first option.
			for (size_t j = static_cast<size_t>(i) + 4; j < static_cast<size_t>(argc);) {
				if (!std::strcmp(argv[j], "--input") && j + 1 < static_cast<size_t>(argc)) {
					dump_input = argv[j + 1];
					j += 2;
				} else if (!std::strcmp(argv[j], "--hold") && j + 1 < static_cast<size_t>(argc)) {
					dump_hold = std::atoi(argv[j + 1]);
					j += 2;
				} else if (!std::strcmp(argv[j], "--snap") && j + 1 < static_cast<size_t>(argc)) {
					dump_snap = std::atoi(argv[j + 1]);
					j += 2;
				} else {
					break;
				}
			}
			i = argc; // Everything after the prefix is consumed above.
		} else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] != '-' && rom_path.empty()) {
			rom_path = argv[i];
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (selftest)
		return gb::run_selftest(rom_path);

	if (!blargg_rom.empty())
		return run_blargg(blargg_rom.c_str(), blargg_frames);

	if (!dumpgb_rom.empty())
		return dump_gb(dumpgb_rom.c_str(), dumpgb_frames, dumpgb_prefix.c_str());

	if (!suite_rom.empty())
		return run_gba_suite(suite_rom.c_str(), suite_index, suite_frames);

	if (!tests_rom.empty())
		return run_gba_tests(tests_rom.c_str(), tests_frames);

	if (!dump_rom.empty())
		return dump_gba(dump_rom.c_str(), dump_frames, dump_prefix.c_str(),
						dump_input.empty() ? nullptr : dump_input.c_str(), dump_hold, dump_snap);

	if (!list_folder.empty()) {
		for (const auto &e : gb::GameBoy::scan_rom_folder(list_folder)) {
			std::printf("%s | %s | %s\n", e.title.c_str(), e.path.c_str(),
						e.gba ? "GBA" : (e.cgb ? "CGB" : "DMG"));
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
#endif // release / dev main
}
