#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "gb/gameboy.h"
#include "gba/core.h"

#if !defined(GB4ME_RELEASE) || defined(GB4ME_KEEP_SELFTEST)
#include "gba/core.h"
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
	std::printf("  optional: --input \"frame:key,...\" --hold N --snap N\n");
	std::printf("           injects key presses to advance past title/name-entry\n");
	std::printf("           screens; key in A,B,S,E,U,D,L,R,H,J (A,B,START,SELECT,UP,\n");
	std::printf("           DOWN,LEFT,RIGHT,L-Shoulder,R-Shoulder). --snap N writes a\n");
	std::printf("           frame every N frames as <prefix>_<frame>.ppm\n");
	std::printf("format support: .gb (DMG) .gbc (CGB) .gba (GBA, built-in HLE BIOS — no firmware\n");
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
int dump_gba(const char *rom, int frames, const char *prefix, const char *script,
			 int holdFrames, int snap)
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
			const std::string keyStr = s.substr(colon + 1, (comma == std::string::npos)
																	  ? std::string::npos
																	  : comma - colon - 1);
			const int f = std::atoi(frameStr.c_str());
			std::vector<gba::GbaKeypad::Key> keys;
			for (const char *p2 = keyStr.c_str(); *p2; ++p2) {
				switch (*p2) {
				case 'a': case 'A': keys.push_back(gba::GbaKeypad::Key::A); break;
				case 'b': case 'B': keys.push_back(gba::GbaKeypad::Key::B); break;
				case 's': case 'S': keys.push_back(gba::GbaKeypad::Key::Start); break;
				case 'e': case 'E': keys.push_back(gba::GbaKeypad::Key::Select); break;
				case 'u': case 'U': keys.push_back(gba::GbaKeypad::Key::Up); break;
				case 'd': case 'D': keys.push_back(gba::GbaKeypad::Key::Down); break;
				case 'l': case 'L': keys.push_back(gba::GbaKeypad::Key::Left); break;
				case 'r': case 'R': keys.push_back(gba::GbaKeypad::Key::Right); break;
				case 'h': case 'H': keys.push_back(gba::GbaKeypad::Key::L); break;
				case 'j': case 'J': keys.push_back(gba::GbaKeypad::Key::R); break;
				default: break;
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
		std::fprintf(o, "bg%dcnt=%04X hofs=%04X vofs=%04X\n", b, gba.bus().read16(0x04000008 + 2 * b),
					gba.ppu().scrollH(b), gba.ppu().scrollV(b));
	std::fprintf(o, "winin=%04X winout=%04X bldcnt=%04X bldalpha=%04X\n",
				gba.bus().read16(0x04000048), gba.bus().read16(0x0400004A),
				gba.bus().read16(0x04000050), gba.bus().read16(0x04000052));
	std::fprintf(o, "bg2pa=%04X pb=%04X pc=%04X pd=%04X x=%08X y=%08X\n",
				gba.ppu().affineParam(0, 0), gba.ppu().affineParam(0, 1),
				gba.ppu().affineParam(0, 2), gba.ppu().affineParam(0, 3),
				static_cast<unsigned>(gba.ppu().affineX(0)),
				static_cast<unsigned>(gba.ppu().affineY(0)));
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
#else // developer build (or release+keep-selftest): full CLI and dumps
	bool selftest = false;
	std::string rom_path;
	std::string list_folder;
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
				} else if (!std::strcmp(argv[j], "--hold") &&
						   j + 1 < static_cast<size_t>(argc)) {
					dump_hold = std::atoi(argv[j + 1]);
					j += 2;
				} else if (!std::strcmp(argv[j], "--snap") &&
						   j + 1 < static_cast<size_t>(argc)) {
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

	if (!dump_rom.empty())
		return dump_gba(dump_rom.c_str(), dump_frames, dump_prefix.c_str(),
					 dump_input.empty() ? nullptr : dump_input.c_str(), dump_hold,
					 dump_snap);

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
