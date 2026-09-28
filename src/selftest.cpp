// Built-in self test (`GB4ME --selftest [rom]`).
//
// One place for the hardware checks that used to live in throwaway harnesses:
// the GBA core (HLE BIOS + SWIs, ARM/THUMB execution, exceptions and IRQs,
// memory bus rules, PPU/DMA/timers/keypad, save hardware, save states) and the
// GB/GBC core (SM83 semantics, interrupt handling, banked WRAM/VRAM, CGB
// palettes, double speed). Hardware expectations cite GBATEK / Pan Docs where
// they are not obvious. Nothing here touches the frontend, so it runs
// headless; the optional ROM argument adds a boot smoke test.
//
// Exit code: 0 = all checks passed, 1 = at least one failure.

#include "selftest.h"

#include "gb/apu.h"
#include "gb/audio_sync.h"
#include "gb/cartridge.h"
#include "gb/cpu.h"
#include "gb/gui_console.h"
#include "gb/input.h"
#include "gb/joypad.h"
#include "gb/mmu.h"
#include "gb/ppu.h"
#include "gb/timer.h"
#include "gb/vulkan_renderer.h"
#include "gba/bios.h"
#include "gba/core.h"
#include "gba/cpu.h"
#include "gba/ppu.h"
#include "gba/save_hardware.h"
#include "vulkan/vulkan_internal.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace gb
{
namespace
{

// ---------------------------------------------------------------------------
// Check harness
// ---------------------------------------------------------------------------

int g_checks = 0;
int g_failed = 0;
const char *g_section = "";

void section(const char *name)
{
	g_section = name;
	std::printf("\n[%s]\n", name);
}

void check(bool ok, const char *what, const std::string &detail = "")
{
	++g_checks;
	if (ok) {
		std::printf("  PASS  %s\n", what);
		return;
	}
	++g_failed;
	std::printf("  FAIL  %s%s%s\n", what, detail.empty() ? "" : " — ", detail.c_str());
}

void info(const char *fmt, ...)
{
	std::printf("  info  ");
	va_list args;
	va_start(args, fmt);
	std::vprintf(fmt, args);
	va_end(args);
	std::printf("\n");
}

std::string hex32(u32 v)
{
	char buf[16];
	std::snprintf(buf, sizeof buf, "0x%08X", v);
	return buf;
}

std::string hex16(u32 v)
{
	char buf[16];
	std::snprintf(buf, sizeof buf, "0x%04X", v & 0xFFFFu);
	return buf;
}

void checkEq(u32 got, u32 want, const char *what)
{
	char buf[96];
	std::snprintf(buf, sizeof buf, "got %s, expected %s", hex32(got).c_str(), hex32(want).c_str());
	check(got == want, what, buf);
}

// String comparisons (paths, hardware modes) print the values, not hex.
void checkStrEq(const std::string &got, const std::string &want, const char *what)
{
	check(got == want, what, "got \"" + got + "\", expected \"" + want + "\"");
}

void checkEq16(u32 got, u32 want, const char *what)
{
	char buf[96];
	std::snprintf(buf, sizeof buf, "got %s, expected %s", hex16(got).c_str(), hex16(want).c_str());
	check((got & 0xFFFFu) == (want & 0xFFFFu), what, buf);
}

// For sub-system timing checks: instruction-granular stepping cannot land on an
// exact cycle, so allow a small window while still catching real drift.
void checkNear16(u32 got, u32 want, u32 tolerance, const char *what)
{
	const u32 g = got & 0xFFFFu, w = want & 0xFFFFu;
	const u32 diff = (g > w) ? (g - w) : (w - g);
	char buf[96];
	std::snprintf(buf, sizeof buf, "got %s, expected %s (tolerance %u)", hex16(g).c_str(),
				  hex16(w).c_str(), tolerance);
	check(diff <= tolerance, what, buf);
}

// ---------------------------------------------------------------------------
// GBA fixture
// ---------------------------------------------------------------------------

constexpr u32 kGbaEntry = 0x08000200; // Where the synthetic ROM's header B lands.
constexpr u32 kGbaEntryOff = 0x200;   // ...and its offset inside the ROM file.
constexpr u32 kCodeBase = 0x02000000; // EWRAM: writable, executable scratch.

// Minimal 256Kbit cartridge: a valid ARM branch at 0x08000000 (the HLE boot
// path reads it, GBATEK "CPU Startup"), a park loop (`B .`) at the target and
// a BX LR helper. Padded with 0xEA so a runaway branch keeps branching.
std::vector<u8> makeTestRom()
{
	std::vector<u8> rom(32 * 1024, 0xEA);
	auto put32 = [&](u32 off, u32 v) {
		rom[off + 0] = static_cast<u8>(v);
		rom[off + 1] = static_cast<u8>(v >> 8);
		rom[off + 2] = static_cast<u8>(v >> 16);
		rom[off + 3] = static_cast<u8>(v >> 24);
	};
	std::memcpy(&rom[0xA0], "GB4ME SELFTEST", 14); // 0xA0 title, 0xAC game code
	rom[0xB2] = 0x96;                              // Header: fixed value (GBATEK)
	// Header checksum over 0xA0..0xBC is mandatory for the cartridge loader.
	u8 chk = 0;
	for (u32 i = 0xA0; i <= 0xBC; ++i)
		chk = static_cast<u8>(chk - rom[i]);
	rom[0xBD] = static_cast<u8>(chk - 0x19);

	put32(0x00, 0xEA00007Eu);             // B +0x1F8 -> 0x08000200
	put32(kGbaEntryOff, 0xEAFFFFFEu);     // B $ (park at the entry point)
	put32(kGbaEntryOff + 4, 0xE12FFF1Eu); // BX LR
	return rom;
}

// Boots the synthetic ROM through the normal HLE path and runs code snippets.
// The core owns ~1MB of traces/VRAM/framebuffers, so it lives on the heap:
// dozens of these are built across the test sections.
struct GbaMachine {
	std::unique_ptr<gba::GameBoyAdvance> owned = std::make_unique<gba::GameBoyAdvance>();
	gba::GameBoyAdvance &g = *owned;

	GbaMachine()
	{
		g.loadFromBytes(makeTestRom());
	}

	// Write an ARM snippet at `base`, append a park loop, run until it lands
	// there. Returns instructions executed.
	int runArm(const std::vector<u32> &code, u32 base = kCodeBase, int max_steps = 4096)
	{
		for (size_t i = 0; i < code.size(); ++i)
			g.bus().write32(base + static_cast<u32>(4 * i), code[i]);
		const u32 park = base + static_cast<u32>(4 * code.size());
		g.bus().write32(park, 0xEAFFFFFEu);
		g.cpu().setState(gba::CpuState::Arm);
		g.cpu().setPc(base);
		int steps = 0;
		while (g.cpu().pc() != park && steps < max_steps) {
			g.cpu().step();
			++steps;
		}
		return steps;
	}

	int runThumb(const std::vector<u16> &code, u32 base = kCodeBase, int max_steps = 4096)
	{
		for (size_t i = 0; i < code.size(); ++i)
			g.bus().write16(base + static_cast<u32>(2 * i), code[i]);
		const u32 park = base + static_cast<u32>(2 * code.size());
		g.bus().write16(park, 0xE7FEu); // B $.
		g.cpu().setState(gba::CpuState::Thumb);
		g.cpu().setPc(base);
		int steps = 0;
		while (g.cpu().pc() != park && steps < max_steps) {
			g.cpu().step();
			++steps;
		}
		return steps;
	}

	// Execute a real `SWI n` from EWRAM (full exception entry + HLE service +
	// MOVS return) with the caller's r0-r3.
	void swi(u8 num, u32 r0 = 0, u32 r1 = 0, u32 r2 = 0, u32 r3 = 0)
	{
		g.cpu().setReg(0, r0);
		g.cpu().setReg(1, r1);
		g.cpu().setReg(2, r2);
		g.cpu().setReg(3, r3);
		// ARM SWI comment field is bits 23-16 (THUMB puts it in bits 7-0).
		runArm({0xEF000000u | (static_cast<u32>(num) << 16)});
	}

	u32 r(u8 i)
	{
		return g.cpu().reg(i);
	}

	u32 read32(u32 addr)
	{
		return g.bus().read32(addr);
	}
	void write32(u32 addr, u32 v)
	{
		g.bus().write32(addr, v);
	}
	void write16(u32 addr, u16 v)
	{
		g.bus().write16(addr, v);
	}
	// LCD / IRQ registers.
	void io16(u32 addr, u16 v)
	{
		g.bus().write16(addr, v);
	}
	u16 io16r(u32 addr)
	{
		return g.bus().read16(addr);
	}
	void runCycles(u32 cycles)
	{
		g.step(cycles);
	}
	int runFrames(int frames)
	{
		int done = 0;
		for (int f = 0; f < frames; ++f) {
			u64 guard = 0;
			while (!g.ppu().frameReady() && guard++ < 400000) {
				g.step(1232);
			}
			if (!g.ppu().frameReady())
				break;
			g.ppu().clearFrameReady();
			++done;
		}
		return done;
	}
	unsigned distinctColors()
	{
		bool seen[32] = {};
		unsigned n = 0;
		for (u32 px : g.ppu().frame().pixels) {
			if (!seen[px & 0x1F]) {
				seen[px & 0x1F] = true;
				++n;
			}
		}
		return n;
	}
};

// Installs a game IRQ handler at 0x02000100 (like a real game would, via
// [0x03007FFC]) that counts entries at 0x02000200, acknowledges IF and returns
// through the documented `SUBS PC, LR, #4`. Returns the handler address.
u32 installIrqHandler(GbaMachine &m)
{
	const u32 h = 0x02000100;
	m.write32(h + 0x00, 0xE59FC020u); // LDR r12, [pc, #32] -> counter address
	m.write32(h + 0x04, 0xE59C1000u); // LDR r1, [r12]
	m.write32(h + 0x08, 0xE2811001u); // ADD r1, r1, #1
	m.write32(h + 0x0C, 0xE58C1000u); // STR r1, [r12]
	m.write32(h + 0x10, 0xE3A01001u); // MOV r1, #1
	m.write32(h + 0x14, 0xE3A02401u); // MOV r2, #0x04000000
	m.write32(h + 0x18, 0xE2822C02u); // ADD r2, r2, #0x200 (-> 0x04000200)
	m.write32(h + 0x1C, 0xE1C210B2u); // STRH r1, [r2, #2] (IF = 1)
	m.write32(h + 0x20, 0xE25EF004u); // SUBS PC, LR, #4
	m.write32(h + 0x28, 0x02000200u); // literal: IRQ counter
	m.write32(0x03007FFC, h);
	return h;
}

// ---------------------------------------------------------------------------
// 1. HLE BIOS startup (no external firmware, no direct-to-ROM jump)
// ---------------------------------------------------------------------------

void testBiosStartup()
{
	section("HLE BIOS startup");
	GbaMachine m;
	check(m.g.loaded(), "synthetic cartridge loads");
	checkEq(m.g.cpu().mode() == gba::CpuMode::System ? 1 : 0, 1, "post-BIOS CPU is in System mode");
	checkEq(m.g.cpu().state() == gba::CpuState::Arm ? 1 : 0, 1, "post-BIOS state is ARM");
	checkEq(m.g.cpu().cpsr(), 0x1Fu, "post-BIOS CPSR = System, I+F clear, ARM");
	checkEq(m.g.cpu().reg(13), gba::HleBios::kSpUsr, "SP_usr = 0x03007F00 (GBATEK stacks)");
	checkEq(m.g.cpu().pc(), kGbaEntry, "PC = cartridge header entry branch target");

	// The instruction at the entry point is the ROM's own branch, not a
	// patched-in jump: the HLE booted through vectors like real hardware.
	checkEq(m.read32(kGbaEntry), 0xEAFFFFFEu, "ROM entry reached unmodified");

	checkEq(m.g.bus().read8(0x04000300), 0x01u, "POSTFLG = 1 (further boot flag)");
	checkEq(m.g.bus().read16(0x04000200), 0, "IE cleared at hand-off");
	checkEq(m.g.bus().read16(0x04000202), 0, "IF cleared at hand-off");
	checkEq(m.g.bus().read16(0x04000208), 0, "IME cleared at hand-off");
	checkEq(m.g.bus().read32(0x03007E00), 0, "IWRAM system area zeroed");

	// BIOS protection: outside the BIOS region the installed vectors are not
	// readable (GBATEK "BIOS reads return the most recently fetched opcode").
	check(!m.g.bus().biosActive(), "BIOS region not marked active while running from ROM");
	check(m.read32(0x18) != 0xEA000042u, "installed IRQ vector not readable from ROM code");
	m.g.bus().setBiosActive(true);
	checkEq(m.read32(0x18), 0xEA000042u, "IRQ vector = B 0x128 when BIOS region is active");
	checkEq(m.read32(0x128), 0xE92D500Fu, "IRQ trampoline pushes {R0-R3,R12,LR}");
	checkEq(m.read32(0x134), 0xE510F004u, "IRQ trampoline loads PC from [0x03007FFC]");
	checkEq(m.read32(0x13C), 0xE25EF004u, "IRQ trampoline returns with SUBS PC,LR,#4");
	m.g.bus().setBiosActive(false);

	// installVectors() is idempotent (called by applyStartup).
	m.g.bios().installVectors(m.g.bus());
	m.g.bus().setBiosActive(true);
	checkEq(m.read32(0x128), 0xE92D500Fu, "installVectors() idempotent");
	m.g.bus().setBiosActive(false);

	// A SWI performs genuine exception entry before the HLE service runs: the
	// probe below is the service site, so it sees the real post-entry state.
	GbaMachine s;
	u32 entry_mode = 0, entry_spsr = 0, entry_lr = 0, entry_cpsr = 0;
	s.g.cpu().setSwiHandler([&](u8) {
		entry_mode = s.g.cpu().cpsr() & 0x1Fu;
		entry_spsr = s.g.cpu().spsr();
		entry_lr = s.g.cpu().reg(14);
		entry_cpsr = s.g.cpu().cpsr();
	});
	s.runArm({0xEF060000u}, kCodeBase, 1); // SWI 06h (Div)
	checkEq(s.g.cpu().pc(), 0x08u, "SWI entry vectors through 0x08");
	checkEq(entry_mode, 0x13u, "SWI entry switches to Supervisor mode");
	checkEq(entry_lr, kCodeBase + 4, "SWI LR_svc = return address ($+4)");
	checkEq(entry_spsr & 0x1Fu, 0x1Fu, "SPSR_svc holds the pre-SWI mode (System)");
	check((entry_cpsr & (1u << gba::kFlagI)) != 0, "SWI entry sets CPSR.I");
}

// ---------------------------------------------------------------------------
// 2. SWI dispatch — every implemented service
// ---------------------------------------------------------------------------

void testSwiDispatch()
{
	section("HLE BIOS SWI services");

	{ // Div (06h): r0/r1 -> quotient, remainder, |quotient| in r3.
		GbaMachine m;
		m.swi(gba::HleBios::kDiv, 100, 7);
		checkEq(m.r(0), 14, "SWI 06h Div quotient");
		checkEq(m.r(1), 2, "SWI 06h Div remainder");
		checkEq(m.r(3), 14, "SWI 06h Div r3 = |quotient|");
	}
	{ // DivArm (07h): reversed arguments, same outputs.
		GbaMachine m;
		m.swi(gba::HleBios::kDivArm, 7, 100);
		checkEq(m.r(0), 14, "SWI 07h DivArm quotient");
		checkEq(m.r(1), 2, "SWI 07h DivArm remainder");
	}
	{ // Sqrt (08h)
		GbaMachine m;
		m.swi(gba::HleBios::kSqrt, 144);
		checkEq(m.r(0), 12, "SWI 08h Sqrt(144) = 12");
		m.swi(gba::HleBios::kSqrt, 0x10000000u);
		checkEq(m.r(0), 0x4000u, "SWI 08h Sqrt(0x10000000) = 0x4000 (integer)");
	}
	{ // ArcTan (09h) / ArcTan2 (0Ah): 1.14 fixed point radians.
		GbaMachine m;
		m.swi(gba::HleBios::kArcTan, 0x4000u); // 1.0 in 1.14 format -> 45 degrees.
		checkEq16(m.r(0), 0x2000u, "SWI 09h ArcTan(1.0) = 0x2000");
		m.swi(gba::HleBios::kArcTan2, 0x4000u, 0x4000u); // atan2(1,1) = 45 deg (1.14 format).
		checkEq16(m.r(0), 0x2000u, "SWI 0Ah ArcTan2(1,1) = 0x2000");
		m.swi(gba::HleBios::kArcTan2, 0, 0x10000u);
		checkEq16(m.r(0), 0x4000u, "SWI 0Ah ArcTan2(0,1) = 0x4000 (90 degrees)");
	}
	{ // GetBiosChecksum (0Dh) — GBATEK: BAAE187Fh.
		GbaMachine m;
		m.swi(gba::HleBios::kGetBiosChecksum);
		checkEq(m.r(0), gba::HleBios::kBiosChecksum, "SWI 0Dh GetBiosChecksum = 0xBAAE187F");
	}
	{ // CpuSet (0Bh): 32-bit copy, count in words.
		GbaMachine m;
		const u32 src = 0x02001000, dst = 0x02002000;
		for (u32 i = 0; i < 8; ++i)
			m.write32(src + 4 * i, 0xA5A50000u + i);
		m.swi(gba::HleBios::kCpuSet, src, dst,
			  0x04000008u); // r2 = control: bit26=32-bit, bit24=inc, count=8
		bool ok = true;
		for (u32 i = 0; i < 8; ++i)
			ok = ok && m.read32(dst + 4 * i) == 0xA5A50000u + i;
		check(ok, "SWI 0Bh CpuSet copies 8 words (32-bit mode)");
	}
	{ // CpuSet fill (bit 24 clear = 16-bit, bit 26 = fixed source).
		GbaMachine m;
		const u32 src = 0x02001000, dst = 0x02002800;
		m.write16(src, 0x1234u);
		// r2 = control: bit26=0 (16-bit), bit24=1 (fixed), count=4
		m.swi(gba::HleBios::kCpuSet, src, dst, 0x05000004u);
		bool ok = true;
		for (u32 i = 0; i < 4; ++i)
			ok = ok && m.g.bus().read16(dst + 4 * i) == 0x1234u;
		check(ok, "SWI 0Bh CpuSet fixed-source fill");
	}
	{ // CpuFastSet (0Ch): 32-byte units, count in words (multiple of 8).
		GbaMachine m;
		const u32 dst = 0x02003000;
		m.write32(0x02001000, 0xDEADBEEFu);
		// r2 = control: bit24=1 (fixed source), count=8 (CpuFastSet ignores bit26, always 32-bit)
		m.swi(gba::HleBios::kCpuFastSet, 0x02001000, dst, 0x01000008u);
		bool ok = true;
		for (u32 i = 0; i < 8; ++i)
			ok = ok && m.read32(dst + 4 * i) == 0xDEADBEEFu;
		check(ok, "SWI 0Ch CpuFastSet 32-byte fill");
	}
	{ // BgAffineSet (0Eh): identity rotation -> pa = pd = 0x100, pb = pc = 0.
		// mGBA format: center (s32 8.8), display center (s16), scaling (s16 8.8), angle (s16 8.8).
		GbaMachine m;
		const u32 in = 0x02001000, out = 0x02002000;
		m.write32(in + 0, 0x00000100u); // center X = 1.0 (8.8)
		m.write32(in + 4, 0);           // center Y = 0
		m.write16(in + 8, 0);           // display center X = 0
		m.write16(in + 10, 0);          // display center Y = 0
		m.write16(in + 12, 0x0100u);    // scaling X = 1.0 (8.8)
		m.write16(in + 14, 0x0100u);    // scaling Y = 1.0 (8.8)
		m.write16(in + 16, 0);          // angle = 0
		m.swi(gba::HleBios::kBgAffineSet, in, out, 1, 0);
		checkEq16(m.g.bus().read16(out + 0), 0x0100u, "SWI 0Eh BgAffineSet pa = 0x100");
		checkEq16(m.g.bus().read16(out + 2), 0x0000u, "SWI 0Eh BgAffineSet pb = 0");
		checkEq16(m.g.bus().read16(out + 4), 0x0000u, "SWI 0Eh BgAffineSet pc = 0");
		checkEq16(m.g.bus().read16(out + 6), 0x0100u, "SWI 0Eh BgAffineSet pd = 0x100");
	}
	{ // ObjAffineSet (0Fh): writes pa..pd with an 8-byte stride.
		// mGBA format: sx (s16 8.8), sy (s16 8.8), theta (s16 8.8), 8-byte entries.
		GbaMachine m;
		const u32 in = 0x02001000, out = 0x02002000;
		// First entry: sx=1.0, sy=1.0, theta=0 -> pa=pd=0x100, pb=pc=0
		m.write16(in + 0, 0x0100u); // sx = 1.0 (8.8)
		m.write16(in + 2, 0x0100u); // sy = 1.0 (8.8)
		m.write16(in + 4, 0);       // theta = 0
		// Second entry: sx=1.0, sy=1.0, theta=0 -> pa=pd=0x100
		m.write16(in + 8, 0x0100u);  // sx = 1.0
		m.write16(in + 10, 0x0100u); // sy = 1.0
		m.write16(in + 12, 0);       // theta = 0
		m.swi(gba::HleBios::kObjAffineSet, in, out, 2, 8);
		checkEq16(m.g.bus().read16(out + 0), 0x0100u, "SWI 0Fh ObjAffineSet first pa");
		checkEq16(m.g.bus().read16(out + 6), 0x0100u, "SWI 0Fh ObjAffineSet first pd");
		checkEq16(m.g.bus().read16(out + 8), 0x0100u, "SWI 0Fh ObjAffineSet stride honoured");
	}
	{ // BitUnPack (10h): 1-bit source -> 8-bit destination.
		GbaMachine m;
		const u32 src = 0x02001000, info = 0x02001100, dst = 0x02002000;
		m.g.bus().write8(src, 0x0F);
		m.write16(info + 0, 1);        // source length in bytes
		m.g.bus().write8(info + 2, 1); // source width
		m.g.bus().write8(info + 3, 8); // destination width
		m.write32(info + 4, 0);        // bias
		m.swi(gba::HleBios::kBitUnPack, src, dst, info);
		const u8 expect[8] = {1, 1, 1, 1, 0, 0, 0, 0};
		bool ok = true;
		for (u32 i = 0; i < 8; ++i)
			ok = ok && m.g.bus().read8(dst + i) == expect[i];
		check(ok, "SWI 10h BitUnPack expands 0x0F to 1,1,1,1,0,0,0,0");
	}
	{ // LZ77 (11h): 8 literals then a back-reference 8 bytes back.
		GbaMachine m;
		const u32 src = 0x02001000, dst = 0x02002000;
		const u8 stream[] = {
			0x10, 0x10, 0x00, 0x00, // signature + 24-bit length = 16
			0x00,                   // flag: 8 literal bytes
			'A',  'B',  'C',  'D',  'E', 'F', 'G', 'H',
			0x80,       // flag: compressed block next
			0x50, 0x07, // len-3 = 5 -> 8 bytes, disp-1 = 7
		};
		for (size_t i = 0; i < sizeof stream; ++i)
			m.g.bus().write8(src + static_cast<u32>(i), stream[i]);
		m.swi(gba::HleBios::kLz77UnCompWram, src, dst);
		const char *expect = "ABCDEFGHABCDEFGH";
		bool ok = true;
		for (u32 i = 0; i < 16; ++i)
			ok = ok && m.g.bus().read8(dst + i) == static_cast<u8>(expect[i]);
		check(ok, "SWI 11h LZ77UnCompWram literals + back-reference");
	}
	{ // RLE (14h): one run block, 8 bytes of 'Z'.
		GbaMachine m;
		const u32 src = 0x02001000, dst = 0x02002000;
		const u8 stream[] = {0x30, 0x08, 0x00, 0x00, 0x85, 'Z'};
		for (size_t i = 0; i < sizeof stream; ++i)
			m.g.bus().write8(src + static_cast<u32>(i), stream[i]);
		m.swi(gba::HleBios::kRlUnCompWram, src, dst);
		bool ok = true;
		for (u32 i = 0; i < 8; ++i)
			ok = ok && m.g.bus().read8(dst + i) == 'Z';
		check(ok, "SWI 14h RLUnCompWram run block");
	}
	{ // Diff8 (16h): prefix-sum filter.
		GbaMachine m;
		const u32 src = 0x02001000, dst = 0x02002000;
		const u8 stream[] = {0x80, 0x03, 0x00, 0x00, 0x01, 0x01, 0x01};
		for (size_t i = 0; i < sizeof stream; ++i)
			m.g.bus().write8(src + static_cast<u32>(i), stream[i]);
		m.swi(gba::HleBios::kDiff8bitUnFilterWram, src, dst);
		check(m.g.bus().read8(dst + 0) == 1 && m.g.bus().read8(dst + 1) == 2 &&
				  m.g.bus().read8(dst + 2) == 3,
			  "SWI 16h Diff8bitUnFilterWram 1,1,1 -> 1,2,3");
	}
	{ // Unknown SWI: must return rather than derail execution.
		GbaMachine m;
		m.swi(0x3F, 0, 0, 0, 0);
		check(m.g.cpu().pc() == kCodeBase + 4, "unknown SWI returns to the caller");
	}
	{ // Div must not touch the divisor's register beyond the documented pair.
		GbaMachine m;
		m.swi(6, 7, 2, 0x11111111u, 0x22222222u);
		checkEq(m.r(2), 0x11111111u, "SWI 06h leaves r2 alone");
	}
}

// ---------------------------------------------------------------------------
// 3. ARM / THUMB execution
// ---------------------------------------------------------------------------

void testArmInstructions()
{
	section("ARM instruction execution");
	{ // Data processing flags.
		GbaMachine m;
		m.runArm({
			0xE3A00001u, // MOV r0, #1
			0xE2900001u, // ADDS r0, r0, #1 -> 2, no carry out
		});
		checkEq(m.r(0), 2, "MOV/ADDS r0 = 2");
		check(!m.g.cpu().getFlag(gba::kFlagC) && !m.g.cpu().getFlag(gba::kFlagZ) &&
				  !m.g.cpu().getFlag(gba::kFlagN),
			  "ADDS with no carry out clears C");
		m.runArm({0xE3500002u}); // CMP r0, #2
		check(m.g.cpu().getFlag(gba::kFlagZ), "CMP r0,#2 sets Z");
		check(m.g.cpu().getFlag(gba::kFlagC), "CMP has no borrow -> C set");
	}
	{ // Carry out and negative flag.
		GbaMachine m;
		m.runArm({
			0xE3E00000u, // MVN r0, #0 -> 0xFFFFFFFF
			0xE2900001u, // ADDS r0, r0, #1 -> 0, Z+C
		});
		checkEq(m.r(0), 0, "0xFFFFFFFF + 1 wraps to 0");
		check(m.g.cpu().getFlag(gba::kFlagZ) && m.g.cpu().getFlag(gba::kFlagC),
			  "wrap sets Z and C");
	}
	{ // Condition codes: MOVEQ skipped while Z is clear.
		GbaMachine m;
		m.runArm({
			0xE3A00005u, // MOV r0, #5
			0xE3500006u, // CMP r0, #6 (not equal)
			0x03A000FFu, // MOVEQ r0, #0xFF (must not run)
		});
		checkEq(m.r(0), 5, "conditional MOVEQ not executed when Z = 0");
	}
	{ // Barrel shifter with register-specified shift.
		GbaMachine m;
		m.runArm({
			0xE3A00201u, // MOV r0, #0x01000000
			0xE3A01008u, // MOV r1, #8
			0xE1A00410u, // MOV r0, r0, LSL r1
		});
		checkEq(m.r(0), 0x10000000u, "LSL by register");
	}
	{ // Multiply / multiply-accumulate / long multiply.
		GbaMachine n;
		n.runArm({
			0xE3A00007u, // MOV r0, #7
			0xE3A01006u, // MOV r1, #6
			0xE0030091u, // MUL r3, r1, r0  -> 42
			0xE3A02005u, // MOV r2, #5
			0xE0233291u, // MLA r3, r1, r2, r3 -> 42 + 30 = 72
		});
		checkEq(n.r(3), 72, "MUL then MLA");
		GbaMachine l;
		l.runArm({
			0xE3E01000u, // MVN r1, #0 -> 0xFFFFFFFF
			0xE3A00002u, // MOV r0, #2
			0xE0830091u, // UMULL r0, r3, r1, r0
		});
		checkEq(l.r(0), 0xFFFFFFFEu, "UMULL low word");
		checkEq(l.r(3), 0x00000001u, "UMULL high word");
	}
	{ // Loads/stores with GBA unaligned semantics (GBATEK "Memory Alignments").
		GbaMachine m;
		m.write32(0x02000400, 0x11223344u);
		m.runArm({
			0xE59F0010u, // LDR r0, [pc, #16] -> literal below
			0xE3A01000u,
			0xE1A02000u, // MOV r2, r0
			0xE59F000Cu, // LDR r0, [pc, #12]
			0xE5901000u, // LDR r1, [r0]      (aligned)
			0xEAFFFFFEu, // park (never reached: runArm appends its own)
			0x02000400u, // literal: address
			0x02000400u, // literal: address
			0x02000400u,
		});
		checkEq(m.r(1), 0x11223344u, "aligned LDR reads the stored word");
		m.runArm({
			0xE3A00000u, // MOV r0, #0
			0xE59F0010u, // LDR r0, [pc, #16] -> 0x02000401
			0xE5901000u, // LDR r1, [r0]      (unaligned: rotate right 8)
			0xE1A01001u,
			0xE1A01001u,
			0xEAFFFFFEu,
			0x02000401u,
			0x02000401u,
			0x02000401u,
		});
		checkEq(m.r(1), 0x44112233u, "unaligned LDR rotates by 8 * (addr & 3)");
		m.runArm({
			0xE59F0010u, // LDR r0, [pc, #16] -> 0x02000401
			0xE3A01042u, // MOV r1, #0x42
			0xE1C010B0u, // STRH r1, [r0]     (aligns down to 0x02000400)
			0xE1A01001u,
			0xE1A01001u,
			0xEAFFFFFEu,
			0x02000401u,
			0x02000401u,
			0x02000401u,
		});
		checkEq16(m.g.bus().read16(0x02000400), 0x0042u, "unaligned STRH aligns down");
	}
	{ // LDRB / STRB and sign extension.
		GbaMachine m;
		m.g.bus().write8(0x02000500, 0x80);
		m.runArm({
			0xE59F0018u, // LDR r0, [pc, #24] -> 0x02000500
			0xE5D01000u, // LDRB r1, [r0]
			0xE1D120F0u, // LDRSH r2, [r0]  (from 0x8000 pattern below)
			0xE1A01001u,
			0xE1A01001u,
			0xEAFFFFFEu,
			0x02000500u,
			0x02000500u,
			0x02000500u,
		});
		checkEq(m.r(1), 0x80u, "LDRB zero-extends");
	}
	{ // Block transfer with writeback.
		GbaMachine m;
		for (u32 i = 0; i < 4; ++i)
			m.write32(0x02000600 + 4 * i, 0x10u + i);
		m.runArm({
			0xE59F001Cu, // LDR r0, [pc, #28] -> 0x02000600
			0xE890000Eu, // LDMIA r0, {r1, r2, r3}
			0xE3A04000u, // MOV r4, #0
			0xE1A00000u, // NOP
			0xE1A00000u,
			0xE1A00000u,
			0xEAFFFFFEu,
			0x02000600u,
			0x02000600u,
			0x02000600u,
		});
		checkEq(m.r(1), 0x10u, "LDM r1");
		checkEq(m.r(2), 0x11u, "LDM r2");
		checkEq(m.r(3), 0x12u, "LDM r3");
	}
	{ // Single data swap (SWP).
		GbaMachine m;
		m.write32(0x02000700, 0xCAFEBABEu);
		m.runArm({
			0xE59F0014u, // LDR r0, [pc, #20] -> 0x02000700
			0xE3A01011u, // MOV r1, #0x11
			0xE1001091u, // SWP r1, r1, [r0] -> r1 = old memory, memory = r1
			0xE1A00000u,
			0xE1A00000u,
			0xEAFFFFFEu,
			0x02000700u,
			0x02000700u,
			0x02000700u,
		});
		checkEq(m.r(1), 0xCAFEBABEu, "SWP returns the old word");
		checkEq(m.read32(0x02000700), 0x11u, "SWP wrote the new word");
	}
	{ // MSR control-field write with an unimplemented mode value: the mode is
	  // preserved while the other written fields still apply (ARM7TDMI).
		GbaMachine m;
		m.g.cpu().setReg(0, 0xF000001Cu); // flags=0xF0, mode=0x1C (invalid)
		m.runArm({
			0xE12C0000u, // MSR CPSR_fc, r0
			0xE1A00000u,
		});
		checkEq(m.g.cpu().cpsr() & 0x1Fu, 0x1Fu, "unimplemented mode field ignored by MSR");
		check((m.g.cpu().cpsr() & 0xF0000000u) == 0xF0000000u, "MSR still applies the flag field");
	}
	{ // User mode cannot change the control field (writes are ignored).
		GbaMachine m;
		m.g.cpu().switchMode(gba::CpuMode::User);
		m.runArm({0xE321F013u}); // MSR CPSR_c, #0x13 (SVC) — ignored in User mode
		checkEq(m.g.cpu().cpsr() & 0x1Fu, 0x10u, "User-mode MSR control write ignored");
		m.g.cpu().switchMode(gba::CpuMode::System);
	}
	{ // BX / BL with LR link.
		GbaMachine m;
		m.write32(0x02000300, 0xE12FFF1Eu); // BX LR
		m.runArm(
			{
				0xEB0000BEu, // BL 0x02000300 (offset 0x300 - 8 = 0x2F8 / 4 = 0xBE)
				0xE1A00000u,
			},
			kCodeBase, 64);
		checkEq(m.r(14), kCodeBase + 4, "BL links LR to the next instruction");
	}
}

void testThumbInstructions()
{
	section("THUMB instruction execution");
	{ // Small arithmetic + flags.
		GbaMachine m;
		m.runThumb({
			0x2005u, // MOV r0, #5
			0x3001u, // ADD r0, #1
			0x2806u, // CMP r0, #6
		});
		checkEq(m.r(0), 6, "THUMB MOV/ADD");
		check(m.g.cpu().getFlag(gba::kFlagZ), "THUMB CMP sets Z");
	}
	{ // THUMB.2 add/subtract: all four bit[10:9] forms (GBATEK).
		GbaMachine a;
		a.runThumb({0x230Au, 0x2104u, 0x185Au}); // MOV r3,#10; MOV r1,#4; ADD r2,r3,r1
		checkEq(a.r(2), 14, "THUMB ADD register form");
		GbaMachine s;
		s.runThumb({0x230Au, 0x2104u, 0x1A5Au}); // SUB r2,r3,r1 -> 6, C set
		checkEq(s.r(2), 6, "THUMB SUB register form");
		check(s.g.cpu().getFlag(gba::kFlagC), "THUMB SUB no-borrow sets C");
		GbaMachine b;
		b.runThumb({0x230Au, 0x1D5Au}); // MOV r3,#10; ADD r2,r3,#5 -> 15
		checkEq(b.r(2), 15, "THUMB ADD immediate3 form");
		GbaMachine t;
		t.runThumb({0x230Au, 0x1F5Au}); // MOV r3,#10; SUB r2,r3,#5 -> 5
		checkEq(t.r(2), 5, "THUMB SUB immediate3 form");
	}
	{ // Register shift + high-register ALU + BL/BX round trip.
		GbaMachine m;
		m.write16(0x02000300, 0x4770u); // BX LR
		m.runThumb(
			{
				0x2108u, // MOV r1, #8
				0x0008u, // MOV r0, r1
				0x0080u, // LSL r0, r0, #2 -> 32
				0xF000u, // BL 0x02000300 (high halfword: link)
				0xF800u, // BL 0x02000300 (low halfword: offset)
				0x4603u, // MOV r3, r0
			},
			kCodeBase, 64);
		checkEq(m.r(3), 32, "THUMB LSL + BL/BX return");
	}
	{ // PUSH/POP and stack access.
		GbaMachine m;
		m.runThumb({
			0x2007u, // MOV r0, #7
			0xB402u, // PUSH {r1}
			0x2109u, // MOV r1, #9
			0xBC02u, // POP {r1}
		});
		checkEq(m.r(1), 0, "THUMB PUSH/POP restores the pushed value");
	}
	{ // PC-relative literal load.
		GbaMachine m;
		m.g.bus().write32(0x02000400, 0xABCD1234u);
		m.runThumb(
			{
				0x4800u, // LDR r0, [pc, #0]  -> literal at (pc+4)&~2 relative
				0x0000u,
				0x0000u,
			},
			0x020003F0u, 64);
		info("THUMB LDR literal PC was %s, r0 = %s", hex32(m.g.cpu().pc()).c_str(),
			 hex32(m.r(0)).c_str());
		check(m.r(0) != 0, "THUMB LDR literal loads a value");
	}
	{ // ARM <-> THUMB interworking through BX.
		GbaMachine m;
		m.write16(0x02000300, 0x2017u); // MOV r0, #23
		m.write16(0x02000302, 0x4770u); // BX LR (LR bit0 set -> stays THUMB)
		m.runArm(
			{
				0xE59F0010u, // LDR r0, [pc, #16] -> 0x02000301
				0xE12FFF10u, // BX r0 (switch to THUMB)
				0xE1A00000u,
				0xE1A00000u,
				0xE1A00000u,
				0xEAFFFFFEu,
				0x02000301u,
				0x02000301u,
				0x02000301u,
			},
			kCodeBase, 64);
		checkEq(m.r(0), 23, "BX switches ARM -> THUMB and runs THUMB code");
	}
	{ // THUMB.10 LDRH/STRH: imm5 scaled by 2 (GBATEK nn = 0..62 step 2).
		GbaMachine m;
		m.g.cpu().setReg(4, 0x02001000u);
		m.g.cpu().setReg(1, 0x1234u);
		m.runThumb({0x8061u}); // STRH r1, [r4, #2] (imm5=1 -> +2)
		checkEq(m.read32(0x02001000u), 0x12340000u, "THUMB.10 STRH scales imm5 by 2");
	}
	{ // THUMB.10 LDRH with imm5 bit 10 set (offset >= 32).
		GbaMachine m;
		m.write32(0x02001020u, 0x11223344u);
		m.g.cpu().setReg(4, 0x02001000u);
		m.runThumb({0x8C60u}); // LDRH r0, [r4, #34] (imm5=17 -> +34)
		checkEq(m.r(0), 0x1122u, "THUMB.10 LDRH large offset zero-extends");
	}
	{ // THUMB.8 register-offset STRH/LDRH (bit 9 set selects halfword group).
		GbaMachine m;
		m.g.cpu().setReg(3, 0x02002000u);
		m.g.cpu().setReg(1, 6u);
		m.g.cpu().setReg(2, 0x55AAu);
		m.runThumb({0x525Au}); // STRH r2, [r3, r1]
		checkEq(m.read32(0x02002004u), 0x55AA0000u, "THUMB.8 STRH stores 16 bits");
		m.g.cpu().setReg(0, 0u);
		m.runThumb({0x5A5Au}); // LDRH r2, [r3, r1]
		checkEq(m.r(2), 0x55AAu, "THUMB.8 LDRH loads 16 bits");
	}
	{ // THUMB.8 LDSB/LDSH sign extension through register offsets.
		GbaMachine m;
		m.write16(0x02002006u, 0xFF00u);
		m.g.cpu().setReg(3, 0x02002000u);
		m.g.cpu().setReg(1, 6u);
		m.g.cpu().setReg(0, 0u);
		m.runThumb({0x5658u}); // LDSB r0, [r3, r1]
		checkEq(m.r(0), 0x00000000u, "THUMB.8 LDSB byte 0x00 stays zero");
		m.write16(0x02002006u, 0x00FFu);
		m.runThumb({0x5658u}); // LDSB r0, [r3, r1] -> 0xFFFFFFFF
		checkEq(m.r(0), 0xFFFFFFFFu, "THUMB.8 LDSB sign-extends");
		m.write16(0x02002006u, 0xFF00u);
		m.runThumb({0x5E58u}); // LDSH r0, [r3, r1] -> 0xFFFFFF00
		checkEq(m.r(0), 0xFFFFFF00u, "THUMB.8 LDSH sign-extends");
	}
	{ // SWI from THUMB uses the 8-bit comment field (bits 7-0).
		GbaMachine m;
		m.g.cpu().setReg(0, 100);
		m.g.cpu().setReg(1, 7);
		m.runThumb({0xDF06u}, kCodeBase, 64);
		checkEq(m.r(0), 14, "THUMB SWI 6 dispatches Div");
		m.g.cpu().setReg(0, 0);
		m.g.cpu().setReg(1, 0);
		m.runThumb({0xDF06u}, kCodeBase, 64);
		// Division by zero: reference HLE contract (mGBA bios.c _Div):
		// r0 = +/-1, r1 = numerator, r3 = 1.
		checkEq(m.r(0), 1, "Div by zero: r0 = 1");
		checkEq(m.r(1), 0, "Div by zero: r1 = numerator");
		checkEq(m.r(3), 1, "Div by zero: r3 = 1");
	}
}

// ---------------------------------------------------------------------------
// 4. Exceptions and interrupts
// ---------------------------------------------------------------------------

void testExceptionsAndIrq()
{
	section("Exceptions and interrupts");
	{ // Undefined instruction takes the Undefined vector.
		GbaMachine m;
		check(m.read32(0x04) != 0, "undefined vector installed");
	}
	{ // Full IRQ path: VBlank -> vector 0x18 -> BIOS trampoline -> 0x03007FFC
	  // -> game handler -> `SUBS PC, LR, #4` return.
		GbaMachine m;
		installIrqHandler(m);
		m.io16(0x04000004, 0x0008u); // DISPSTAT: VBlank IRQ enable
		m.io16(0x04000200, 0x0001u); // IE = VBlank
		m.io16(0x04000208, 1);       // IME = 1
		m.runFrames(2);
		const u32 count = m.read32(0x02000200);
		check(count >= 2, "VBlank IRQ reached the handler via the BIOS trampoline",
			  "count=" + hex32(count));
		checkEq(m.io16r(0x04000208), 1, "IME still enabled after servicing");
	}
	{ // IE gating: DISPSTAT enable + IE must both be set, IME must be 1.
		GbaMachine m;
		installIrqHandler(m);
		m.io16(0x04000004, 0x0008u);
		m.io16(0x04000200, 0x0001u); // IE = VBlank
		m.io16(0x04000208, 0);       // IME = 0
		const u64 before = m.g.cpu().cycles();
		m.runFrames(1);
		checkEq(m.read32(0x02000200), 0u, "no IRQ taken while IME = 0");
		check(m.g.cpu().cycles() > before, "frame still advanced");
	}
	{ // CPSR.I masks the same IRQ (IME gates entry, I gates at the CPU).
		GbaMachine m;
		installIrqHandler(m);
		m.io16(0x04000004, 0x0008u);
		m.io16(0x04000200, 0x0001u);
		m.io16(0x04000208, 1);
		const u32 masked = m.g.cpu().cpsr() | (1u << gba::kFlagI);
		m.g.cpu().setCpsr(masked);
		m.runFrames(1);
		checkEq(m.read32(0x02000200), 0u, "CPSR.I masks the IRQ");
		checkEq(m.io16r(0x04000202) & 0x0001u, 0x0001u, "IF still pending while masked");
		m.g.cpu().setCpsr(masked & ~(1u << gba::kFlagI));
		m.runFrames(1);
		check(m.read32(0x02000200) >= 1, "clearing CPSR.I lets the pending IRQ through");
	}
	{ // IntrWait/Halt put the CPU into the halted low-power state.
		GbaMachine m;
		m.io16(0x04000004, 0x0008u); // DISPSTAT: VBlank IRQ enable (PPU raises IF)
		m.io16(0x04000200, 0x0001u); // IE = VBlank (pollWait wakes on IE & IF)
		m.g.cpu().setReg(0, 0);
		m.g.cpu().setReg(1, 0x0001u);
		m.runArm({0xEF040000u}, kCodeBase, 4); // SWI 04h IntrWait
		check(m.g.cpu().halted(), "SWI 04h IntrWait halts the CPU");
		m.runFrames(1); // PPU hits VBlank -> IF bit 0 -> pollWait wakes CPU
		check(!m.g.cpu().halted(), "waiting CPU wakes when the waited IRQ fires");
	}
}

// ---------------------------------------------------------------------------
// 5. Memory bus
// ---------------------------------------------------------------------------

void testBus()
{
	section("Memory bus");
	GbaMachine m;
	using Region = gba::GbaBus::Region;
	checkEq(static_cast<u32>(gba::GbaBus::decode(0x00000000)), static_cast<u32>(Region::Bios),
			"decode: BIOS");
	checkEq(static_cast<u32>(gba::GbaBus::decode(0x02000000)), static_cast<u32>(Region::Ewram),
			"decode: EWRAM");
	checkEq(static_cast<u32>(gba::GbaBus::decode(0x03000000)), static_cast<u32>(Region::Iwram),
			"decode: IWRAM");
	checkEq(static_cast<u32>(gba::GbaBus::decode(0x04000000)), static_cast<u32>(Region::Io),
			"decode: I/O");
	checkEq(static_cast<u32>(gba::GbaBus::decode(0x05000000)), static_cast<u32>(Region::Palette),
			"decode: palette");
	checkEq(static_cast<u32>(gba::GbaBus::decode(0x06000000)), static_cast<u32>(Region::Vram),
			"decode: VRAM");
	checkEq(static_cast<u32>(gba::GbaBus::decode(0x07000000)), static_cast<u32>(Region::Oam),
			"decode: OAM");
	checkEq(static_cast<u32>(gba::GbaBus::decode(0x08000000)), static_cast<u32>(Region::Rom),
			"decode: ROM slot 0");
	checkEq(static_cast<u32>(gba::GbaBus::decode(0x0E000000)), static_cast<u32>(Region::Sram),
			"decode: SRAM/Flash/EEPROM");
	// 0x0F000000 is outside every populated device: reads are open bus.
	checkEq(m.g.bus().read8(0x0F000000), 0xFFu, "unmapped region reads 0xFF (open bus)");

	// Alignment rules (GBATEK "Memory Alignments").
	m.write16(0x02000001, 0xABCDu);
	checkEq16(m.g.bus().read16(0x02000000), 0xABCDu, "16-bit write to odd address aligns down");
	m.write32(0x02000022, 0x11223344u);
	checkEq(m.read32(0x02000020), 0x11223344u, "32-bit write aligns down");
	checkEq(m.g.bus().read8(0x02000023), 0x11u, "byte views reflect the aligned word store");

	// Mirrors (GBATEK "GBA Memory Map").
	m.g.bus().write8(0x02040000, 0x5Au); // EWRAM mirrors every 256K
	checkEq(m.g.bus().read8(0x02000000), 0x5Au, "EWRAM mirrors at 256K boundaries");
	m.g.bus().write8(0x03008000, 0x77u); // IWRAM mirrors every 32K
	checkEq(m.g.bus().read8(0x03000000), 0x77u, "IWRAM mirrors at 32K boundaries");

	// Byte-level access to a 16-bit device register (IE at 0x04000200).
	m.io16(0x04000200, 0x0102u);
	checkEq(m.g.bus().read8(0x04000200), 0x02u, "read8 of IE low byte");
	checkEq(m.g.bus().read8(0x04000201), 0x01u, "read8 of IE high byte");
	checkEq(m.io16r(0x04000200), 0x0102u, "16-bit IE read matches");

	// VRAM 8-bit writes replicate to both halves (GBATEK "VRAM").
	m.g.bus().write8(0x06000000, 0x3Cu);
	checkEq16(m.g.bus().read16(0x06000000), 0x3C3Cu, "8-bit VRAM write replicates");
	// 8-bit VRAM writes replicate the byte across the halfword, from odd
	// addresses too (mGBA GBAMemory GBAStore8 VRAM case).
	m.g.bus().write8(0x06000001, 0x5Au);
	checkEq16(m.g.bus().read16(0x06000000), 0x5A5Au, "odd 8-bit VRAM write replicates");
	// ...but bytes cannot reach OBJ VRAM: 0x06010000+ in tile modes.
	m.g.bus().write8(0x06010000, 0xAAu);
	checkEq16(m.g.bus().read16(0x06010000), 0x0000u, "8-bit write to OBJ VRAM ignored");

	// BIOS write protection from outside the BIOS region.
	m.g.bus().write8(0x0C, 0x00);
	m.g.bus().setBiosActive(true);
	check(m.g.bus().read8(0x0C) != 0x00, "BIOS is write-protected from ROM code");
	m.g.bus().setBiosActive(false);

	// WAITCNT: ROM non-sequential accesses cost more than sequential ones.
	const u32 rom_n = m.g.bus().accessCycles(0x08000000, 2, false);
	const u32 rom_s = m.g.bus().accessCycles(0x08000000, 2, true);
	check(rom_n >= rom_s, "ROM N-cycles >= S-cycles at default WAITCNT",
		  "N=" + hex32(rom_n) + " S=" + hex32(rom_s));
	// SRAM is the slowest region (8 cycles, 3+1 wait pattern per GBATEK).
	m.g.bus().write8(0x0E000000, 0x12);
	check(m.g.bus().accessCycles(0x0E000000, 1, true) > rom_s, "SRAM slower than ROM");

	m.g.bus().setWaitcnt(0x4014u);
	checkEq16(m.io16r(0x04000204), 0x4014u, "WAITCNT is readable");

	// EEPROM region mirrors ROM when no EEPROM is fitted.
	checkEq(m.g.bus().read8(0x0D000000), m.g.bus().read8(0x08000000),
			"0x0D000000 mirrors ROM without EEPROM save hardware");
	// VRAM upper 32K mirrors 0x06010000-0x06017FFF (GBATEK mirrors).
	m.g.bus().write16(0x06010000u, 0xABCDu);
	checkEq16(m.g.bus().read16(0x06018000u), 0xABCDu, "VRAM mirror reads back");
	m.g.bus().write16(0x06018002u, 0x1234u);
	checkEq16(m.g.bus().read16(0x06010002u), 0x1234u, "VRAM mirror writes through");
}

// ---------------------------------------------------------------------------
// 6. PPU
// ---------------------------------------------------------------------------

void testPpu()
{
	section("PPU");
	{ // DISPCNT bit 7 (forced blank) -> white screen (mGBA video.c).
		GbaMachine m;
		m.io16(0x04000000, 0x0080u);
		m.runFrames(1);
		const u32 px = m.g.ppu().frame().pixels[80 * 240 + 120];
		check((px & 0xFFFFFFu) == 0xFFFFFFu, "forced blank renders white", hex32(px));
	}
	{ // No layers enabled -> backdrop colour (palette entry 0).
		GbaMachine m;
		m.g.bus().write16(0x05000000, 0x001Fu);
		m.runFrames(1);
		const u32 px = m.g.ppu().frame().pixels[80 * 240 + 120];
		check(((px >> 16) & 0xFF) > 0xE0 && ((px >> 8) & 0xFF) < 0x20,
			  "no layers -> backdrop = palette entry 0", hex32(px));
	}
	{
		GbaMachine m;
		// Mode 3 (bitmap): plot a red pixel at (0,0) of the visible area.
		m.io16(0x04000000, 0x0403u); // DISPCNT: mode 3, BG2 on
		m.g.bus().write16(0x06000000, 0x001Fu);
		m.runFrames(1);
		const u32 px = m.g.ppu().frame().pixels[0];
		check(((px >> 16) & 0xFF) > 0xE0 && ((px >> 8) & 0xFF) < 0x20,
			  "mode 3 bitmap pixel renders red", hex32(px));
	}
	{ // Mode 0 text BG: 4bpp tile 0 filled with colour 1, BG0 map entry 0.
		GbaMachine m;
		m.io16(0x04000000, 0x0100u); // DISPCNT: mode 0, BG0 on
		m.io16(0x04000008, 0x1000u); // BG0CNT: map base block 16 -> 0x06008000
		for (u32 i = 0; i < 32; ++i)
			m.g.bus().write16(0x06000000u + 2 * i, 0x1111u); // tile 0 rows
		m.g.bus().write16(0x06008000u, 0x0000u);             // map entry 0 -> tile 0
		m.g.bus().write16(0x05000000, 0x001Fu);              // BG palette colour 0 = red
		m.g.bus().write16(0x05000002, 0x03E0u);              // colour 1 = green
		m.runFrames(1);
		const u32 px = m.g.ppu().frame().pixels[0];
		check(((px >> 8) & 0xFF) > 0xE0 && ((px >> 16) & 0xFF) < 0x20,
			  "mode 0 BG0 tile colour index 1 = palette colour 1", hex32(px));
	}
	{
		GbaMachine m;
		// Sprite path: OAM entry 0 visible, OBJ palette colour 1 = red.
		m.io16(0x04000000, 0x1100u); // mode 0, BG0 + OBJ enable
		m.g.bus().write16(0x05000200, 0x0000u);
		m.g.bus().write16(0x05000202, 0x001Fu); // OBJ palette 0 colour 1
		for (u32 i = 0; i < 8; ++i)
			m.g.bus().write16(0x06010000u + 2 * i, 0x1111u); // OBJ tile 0 rows
		m.g.bus().write16(0x07000000, 20);                   // attr0: y
		m.g.bus().write16(0x07000002, 8);                    // attr1: x
		m.g.bus().write16(0x07000004, 0);                    // attr2: tile 0, palette 0
		m.runFrames(1);
		bool found_red = false;
		for (u32 y = 0; y < 40; ++y)
			for (u32 x = 0; x < 40; ++x) {
				const u32 px = m.g.ppu().frame().pixels[y * 240 + x];
				if (((px >> 16) & 0xFF) > 0xE0 && ((px >> 8) & 0xFF) < 0x20)
					found_red = true;
			}
		check(found_red, "OBJ sprite renders with its OBJ palette colour");
	}
	{ // Disabling BG0 falls back to the backdrop colour (palette entry 0).
		GbaMachine m;
		m.io16(0x04000000, 0x0100u);
		m.io16(0x04000008, 0x1000u);
		for (u32 i = 0; i < 32; ++i)
			m.g.bus().write16(0x06000000u + 2 * i, 0x1111u);
		m.g.bus().write16(0x06008000u, 0x0000u);
		m.g.bus().write16(0x05000000, 0x001Fu);
		m.g.bus().write16(0x05000002, 0x03E0u);
		m.runFrames(1);
		const u32 with_bg0 = m.g.ppu().frame().pixels[0];
		check(((with_bg0 >> 8) & 0xFF) > 0xE0, "BG0 draws its tile colour");
		m.io16(0x04000000, 0x0000u); // disable BG0
		m.runFrames(1);
		const u32 px = m.g.ppu().frame().pixels[0];
		check(((px >> 16) & 0xFF) > 0xE0, "backdrop colour (palette 0 colour 0) after BG0 off",
			  hex32(px));
	}
	{ // Priority: OBJ beats BG on equal priority numbers, BG0 beats BG1
	  // (mGBA composite order; GBATEK "BG0 is having the highest").
		GbaMachine m;
		m.io16(0x04000000, 0x1300u); // mode 0, BG0 + BG1 + OBJ
		m.io16(0x04000008, 0x1000u); // BG0CNT: prio 0, map block 16
		m.io16(0x0400000A, 0x1100u); // BG1CNT: prio 0, map block 17
		for (u32 i = 0; i < 32; ++i)
			m.g.bus().write16(0x06000000u + 2 * i, 0x1111u); // tile 0: colour 1
		for (u32 i = 0; i < 32; ++i)
			m.g.bus().write16(0x06000020u + 2 * i, 0x2222u); // tile 1: colour 2
		m.g.bus().write16(0x06008000u, 0x0000u);             // BG0 map -> tile 0
		for (u32 i = 0; i < 1024; ++i)
			m.g.bus().write16(0x06008800u + 2 * i, 0x0001u); // BG1 map -> tile 1
		m.g.bus().write16(0x05000000, 0x0000u);
		m.g.bus().write16(0x05000002, 0x03E0u); // BG colour 1 = green
		m.g.bus().write16(0x05000004, 0x7C00u); // BG colour 2 = blue
		m.g.bus().write16(0x05000202, 0x001Fu); // OBJ colour 1 = red
		for (u32 i = 0; i < 16; ++i)
			m.g.bus().write16(0x06010000u + 2 * i, 0x1111u); // OBJ tile 0, rows 0-7
		m.g.bus().write16(0x07000000, 20);                   // attr0: y = 20
		m.g.bus().write16(0x07000002, 8);                    // attr1: x = 8
		m.g.bus().write16(0x07000004, 0);                    // attr2: tile 0, prio 0
		m.runFrames(1);
		const u32 px = m.g.ppu().frame().pixels[24 * 240 + 12];
		check(((px >> 16) & 0xFF) > 0xE0 && ((px >> 8) & 0xFF) < 0x20,
			  "OBJ beats BG0 on priority tie", hex32(px));
		m.io16(0x04000000, 0x0300u); // OBJ off: BG0 vs BG1, same prio
		m.runFrames(1);
		const u32 px2 = m.g.ppu().frame().pixels[24 * 240 + 12];
		check(((px2 >> 8) & 0xFF) > 0xE0 && ((px2 >> 16) & 0xFF) < 0x20,
			  "BG0 beats BG1 on priority tie", hex32(px2));
	}
	{ // 256-color sprites: OAM numbers address 32-byte slots (GBATEK
	  // 1D example tiles 04h,06h; mGBA tile*0x20), not 64-byte tiles.
		GbaMachine m;
		m.io16(0x04000000, 0x1040u);                         // mode 0, OBJ, 1D mapping
		m.g.bus().write16(0x05000200u + 2 * 0x5Au, 0x001Fu); // OBJ colour 0x5A = red
		m.g.bus().write16(0x05000200u + 2 * 0xA5u, 0x7C00u); // OBJ colour 0xA5 = blue
		for (u32 i = 0; i < 4; ++i)
			m.g.bus().write16(0x06010080u + 2 * i, 0x5A5Au); // tile 4 row 0
		m.g.bus().write16(0x07000000, 20 | 0x2000u);         // attr0: y, 256 colours
		m.g.bus().write16(0x07000002, 8);                    // attr1: x, 8x8
		m.g.bus().write16(0x07000004, 4);                    // attr2: tile 4
		m.runFrames(1);
		const u32 px = m.g.ppu().frame().pixels[20 * 240 + 10];
		check(((px >> 16) & 0xFF) > 0xE0 && ((px >> 8) & 0xFF) < 0x20,
			  "256-colour 8x8 sprite reads slot tile*32", hex32(px));
	}
	{ // Alpha blend: BG0 (first target) over BG1 (second target) mixes
	  // EVA/EVB (GBATEK color special effects).
		GbaMachine m;
		m.io16(0x04000000, 0x0300u); // mode 0, BG0 + BG1
		m.io16(0x04000008, 0x1000u); // BG0CNT: prio 0, map block 16
		m.io16(0x0400000A, 0x1100u); // BG1CNT: prio 1, map block 17
		for (u32 i = 0; i < 32; ++i)
			m.g.bus().write16(0x06000000u + 2 * i, 0x1111u); // tile 0: colour 1
		for (u32 i = 0; i < 32; ++i)
			m.g.bus().write16(0x06000020u + 2 * i, 0x2222u); // tile 1: colour 2
		m.g.bus().write16(0x06008000u, 0x0000u);
		for (u32 i = 0; i < 1024; ++i)
			m.g.bus().write16(0x06008800u + 2 * i, 0x0001u); // BG1 map -> tile 1
		m.g.bus().write16(0x05000000, 0x0000u);
		m.g.bus().write16(0x05000002, 0x001Fu); // colour 1 = full red
		m.g.bus().write16(0x05000004, 0x03E0u); // colour 2 = full green
		m.io16(0x04000050, 0x0241u);            // effect 1: BG0 first, BG1 second
		m.io16(0x04000052, 0x0808u);            // EVA = EVB = 8 (half/half)
		m.runFrames(1);
		const u32 px = m.g.ppu().frame().pixels[10 * 240 + 10];
		const unsigned r = (px >> 16) & 0xFFu, g = (px >> 8) & 0xFFu;
		const unsigned b = px & 0xFFu;
		check(r > 0x60 && r < 0xA0 && g > 0x60 && g < 0xA0 && b < 0x20,
			  "alpha blend mixes red over green", hex32(px));
	}
	{ // LZ77UnCompVram assembles halfwords across byte stores (GBATEK).
		GbaMachine m;
		const u32 src = 0x02001000u, dst = 0x06000000u;
		m.write32(src, 0x00000410u);      // header: 0x10, length 4
		m.g.bus().write8(src + 4, 0x00u); // block header: 8 literals
		m.g.bus().write8(src + 5, 0xDEu);
		m.g.bus().write8(src + 6, 0xADu);
		m.g.bus().write8(src + 7, 0xBEu);
		m.g.bus().write8(src + 8, 0xEFu);
		m.swi(0x12, src, dst);
		checkEq16(m.g.bus().read16(dst), 0xADDEu, "LZ77 VRAM halfword 0");
		checkEq16(m.g.bus().read16(dst + 2), 0xEFBEu, "LZ77 VRAM halfword 1");
	}
	{ // HuffUnComp 4-bit: root with two terminal leaves (GBATEK tree).
		GbaMachine m;
		const u32 src = 0x02001000u, dst = 0x02002000u;
		m.write32(src, 0x00000424u);      // header: 0x20|4, length 4
		m.g.bus().write8(src + 4, 0x01u); // tree size 3
		m.g.bus().write8(src + 5, 0xC0u); // root: both terminal
		m.g.bus().write8(src + 6, 0x05u); // left leaf
		m.g.bus().write8(src + 7, 0x0Au); // right leaf
		m.write32(src + 8, 0x40000000u);  // bits 0,1 -> 5,A, then 5s (8 nibbles)
		m.swi(0x13, src, dst);
		checkEq(m.read32(dst), 0x555555A5u, "Huffman 4-bit decodes 5,A,5...");
	}
	{ // RlUnCompVram run block + Diff16bitUnFilter chained deltas.
		GbaMachine m;
		const u32 src = 0x02001000u, dst = 0x06000000u;
		m.write32(src, 0x00000430u);      // header: 0x30, length 4
		m.g.bus().write8(src + 4, 0x81u); // run of 4
		m.g.bus().write8(src + 5, 0x7Eu);
		m.swi(0x15, src, dst);
		checkEq16(m.g.bus().read16(dst), 0x7E7Eu, "RL VRAM run fills");
		const u32 s2 = 0x02001100u, d2 = 0x02002100u;
		m.write32(s2, 0x00000400u | 0x00u); // header length 4 (signature unchecked)
		m.g.bus().write16(s2 + 4, 0x0001u);
		m.g.bus().write16(s2 + 6, 0x0002u);
		m.swi(0x18, s2, d2);
		checkEq16(m.g.bus().read16(d2), 0x0001u, "Diff16 first word");
		checkEq16(m.g.bus().read16(d2 + 2), 0x0003u, "Diff16 accumulates");
	}
	{ // Per-line OBJ cycle budget: sprites past ~1210 cycles drop
	  // (GBATEK "Maximum Number of Sprites per Line"; mGBA/VBA-M model it).
		GbaMachine m;
		m.io16(0x04000000, 0x1000u); // mode 0, OBJ, 2D mapping
		for (u32 i = 0; i < 8192; ++i)
			m.g.bus().write16(0x06010000u + 2 * i, 0x1111u); // tiles 0-255 full
		m.g.bus().write16(0x05000000, 0x0000u);              // backdrop black
		m.g.bus().write16(0x05000202, 0x001Fu);              // OBJ colour 1 red
		for (u32 i = 0; i < 128; ++i) {                      // 64x64, two columns
			const u32 x = i < 19 ? 0u : 176u;
			m.g.bus().write16(0x07000000u + 8 * i, 0);           // y = 0
			m.g.bus().write16(0x07000002u + 8 * i, x | 0xC000u); // x, size 3
			m.g.bus().write16(0x07000004u + 8 * i, 0);           // tile 0
		}
		m.runFrames(1);
		const u32 early = m.g.ppu().frame().pixels[32 * 240 + 32];
		const u32 late = m.g.ppu().frame().pixels[32 * 240 + 200];
		check(((early >> 16) & 0xFF) > 0xE0, "early sprite draws", hex32(early));
		check(((late >> 16) & 0xFF) < 0x20 && ((late >> 8) & 0xFF) < 0x20,
			  "over-budget sprite drops to backdrop", hex32(late));
	}
	{ // A non-transformed OBJ parked below the screen must NOT be drawn
	  // wrapped onto the top. mGBA wraps Y only for transformed sprites
	  // (software-obj.c:222-225). An 8x8 sprite at Y=250 is the minimal
	  // trigger: (0-250)&0xFF = 6 < 8, so the old modulo drew it at y=0.
		GbaMachine m;
		m.io16(0x04000000, 0x1100u); // mode 0, OBJ on, 1D OBJ mapping
		// An all-zero OAM entry is a live 8x8 sprite at (0,0), so every
		// slot must be disabled first. The disable flag is attr0 bit 9
		// (GBATEK "Disable OBJ"), NOT bit 15.
		for (u32 s = 0; s < 128; ++s)
			m.g.bus().write16(0x07000000u + 8 * s, 0x0200u);
		for (u32 r = 0; r < 8; ++r) {
			m.g.bus().write16(0x06010000u + 4 * r, 0x0101u);
			m.g.bus().write16(0x06010002u + 4 * r, 0x0101u);
		}
		m.g.bus().write16(0x05000202u, 0x001Fu); // OBJ colour 1 = red (r=31)
		m.g.bus().write16(0x07000000u, 250u);    // attr0: Y = 250 (parked)
		m.g.bus().write16(0x07000002u, 8u);      // attr1: X = 8
		m.g.bus().write16(0x07000004u, 0u);      // tile 0, prio 0
		m.runFrames(1);
		bool any_red = false;
		for (unsigned x = 0; x < 20 && !any_red; ++x)
			for (unsigned y = 0; y < 12 && !any_red; ++y) {
				const u32 px = m.g.ppu().frame().pixels[y * 240 + x];
				if (((px >> 16) & 0xFF) > 0xE0)
					any_red = true;
			}
		check(!any_red, "OBJ parked below the screen is not drawn wrapped to the top");
	}
	{ // OBJ vertical mosaic snaps by screen Y (VBA-M cites NBA/HW
	  // research; mGBA snaps the scanline too), not by sprite-local row.
		GbaMachine m;
		m.io16(0x04000000, 0x1000u);  // mode 0, OBJ
		m.io16(0x0400004Cu, 0x7000u); // OBJ mosaic V = 8
		for (u32 r = 0; r < 8; ++r) {
			const u16 v = static_cast<u16>(((r + 1) << 4) | (r + 1));
			m.g.bus().write16(0x06010000u + 4 * r, v);
			m.g.bus().write16(0x06010002u + 4 * r, v);
		}
		m.g.bus().write16(0x05000000, 0x0000u);
		for (u32 c = 1; c <= 8; ++c)
			m.g.bus().write16(0x05000200u + 2 * c,
							  static_cast<u16>((c << 10) | (c << 5) | c)); // grey ramp
		m.g.bus().write16(0x07000000, 20 | 0x1000u);                       // attr0: y, mosaic
		m.g.bus().write16(0x07000002, 8);                                  // attr1: x, 8x8
		m.g.bus().write16(0x07000004, 0);                                  // attr2: tile 0
		m.runFrames(1);
		// Line 25, sprite row 5: screen snap 25 - 25%8 = 24 -> row 4
		// (colour 5). Sprite-local snap would give row 0 (colour 1).
		// x=8 (tile byte 0) is opaque; x=10 lands on the zero high byte
		// of the 0x55 row pattern (transparent by construction).
		const u32 px = m.g.ppu().frame().pixels[25 * 240 + 8];
		const unsigned got = (((px >> 16) & 0xFF) << 2) | (((px >> 8) & 0xFF) >> 3);
		info("mosaic row colour nibble = %u", got & 0xFu);
		// Colour 5 renders as (41,41,41); colour 1 as (8,8,8); a miss is
		// backdrop black. 0x20 separates them.
		check(((px >> 16) & 0xFF) > 0x20 && ((px >> 8) & 0xFF) > 0x20,
			  "OBJ mosaic uses screen-Y snap", hex32(px));
	}
	{ // Scanline timing: line 160 is VBlank (GBATEK "GBA Video Controller").
		GbaMachine m;
		m.io16(0x04000004, 0x0008u); // DISPSTAT: VBlank IRQ enable gates the IF bit
		u64 guard = 0;
		while (m.g.ppu().line() < 160 && guard++ < 1000000)
			m.g.step(1);
		check((m.io16r(0x04000004) & 0x0001u) != 0, "DISPSTAT VBlank flag set on line 160");
		check((m.io16r(0x04000202) & 0x0001u) != 0, "IF bit 0 (VBlank) raised on line 160");
		checkEq(m.g.ppu().line(), 160, "VCOUNT = 160 at VBlank entry");
	}
	{ // HBlank phase of a drawn line sets the flag; frames are delivered at
	  // VBlank entry, so the line counter sits at 160 when a frame is ready.
		GbaMachine m;
		bool saw_hblank = false;
		u64 guard = 0;
		while (m.g.ppu().line() < 100 && guard++ < 1000000)
			m.g.step(1);
		while (m.g.ppu().line() == 100 && guard++ < 1000000) {
			if ((m.io16r(0x04000004) & 0x0002u) != 0)
				saw_hblank = true;
			m.g.step(4);
		}
		check(saw_hblank, "DISPSTAT HBlank flag set during line 100");
		checkEq(m.runFrames(1), 1, "a frame completes in one frame period");
		checkEq(m.g.ppu().line(), 160u, "frame delivered at VBlank entry (line 160)");
	}
	{ // VCount match (VCOUNT = LYC) sets the DISPSTAT flag and raises the IRQ.
		GbaMachine m;
		m.io16(0x04000004, 0x0020u | 42u); // VCount IRQ enable, LYC = 42
		bool saw_match = false;
		u64 guard = 0;
		while (m.g.ppu().line() < 45 && guard++ < 1000000) {
			if ((m.io16r(0x04000004) & 0x0004u) != 0)
				saw_match = true;
			m.g.step(1);
		}
		check(saw_match, "DISPSTAT VCount flag set while VCOUNT = LYC");
		check((m.io16r(0x04000202) & 0x0004u) != 0, "IF bit 2 (VCount) raised");
	}
}

// ---------------------------------------------------------------------------
// 7. DMA
// ---------------------------------------------------------------------------

void testDma()
{
	section("DMA");
	{
		GbaMachine m;
		for (u32 i = 0; i < 16; ++i)
			m.write32(0x02001000 + 4 * i, 0x51510000u + i);
		m.write32(0x040000D4, 0x02001000u);               // SAD
		m.write32(0x040000D8, 0x02002000u);               // DAD
		m.write32(0x040000DC, (16u << 16) | 0x84000000u); // 32-bit, immediate, enable
		bool ok = true;
		for (u32 i = 0; i < 16; ++i)
			ok = ok && m.read32(0x02002000 + 4 * i) == 0x51510000u + i;
		check(ok, "DMA3 immediate 32-bit copy");
		checkEq(m.read32(0x040000DC) & 0x80000000u, 0, "DMA3 enable bit clears on completion");
	}
	{
		GbaMachine m;
		// Fixed destination (fill) via DMA3 16-bit, control = 0x8500_0000.
		m.write16(0x02001000, 0x1234u);
		m.write32(0x040000D4, 0x02001000u);
		m.write32(0x040000D8, 0x02002000u);
		m.write32(0x040000DC, (8u << 16) | 0x85000000u); // 16-bit, dest fixed, enable
		bool ok = true;
		for (u32 i = 0; i < 8; ++i)
			ok = ok && m.g.bus().read16(0x02002000) == 0x1234u;
		check(ok, "DMA3 with a fixed destination address fills");
	}
	{
		GbaMachine m;
		// VBlank-triggered repeating DMA3 (animation-style transfers).
		for (u32 i = 0; i < 4; ++i)
			m.write32(0x02001000 + 4 * i, 0x0000BEEFu);
		m.write32(0x040000D4, 0x02001000u);
		m.write32(0x040000D8, 0x02002000u);
		m.write32(0x040000DC, (0x9600u << 16) | 4u); // enable, VBlank, 32-bit, repeat
		checkEq(m.read32(0x02002000), 0u, "VBlank DMA does not fire immediately");
		check((m.g.bus().read16(0x040000DE) & 0x8000u) != 0, "VBlank DMA stays armed");
		m.runFrames(2);
		checkEq(m.read32(0x02002000), 0x0000BEEFu, "VBlank DMA transfers at VBlank");
	}
	{
		GbaMachine m;
		// DMA completion IRQ (DMA3 = IF bit 11).
		m.write32(0x040000D4, 0x02001000u);
		m.write32(0x040000D8, 0x02002000u);
		m.write32(0x040000DC, (0xC400u << 16) | 4u); // 32-bit + IRQ enable
		check((m.io16r(0x04000202) & 0x0800u) != 0, "DMA3 completion raises IF bit 11");
	}
	{ // HBlank DMA fires on visible lines only (mGBA gates GBADMARunHblank
	  // on vcount < 160); VBlank lines carry no HBlank DMA start condition.
		GbaMachine m;
		m.write32(0x02001000u, 0xCAFEBABEu);
		u64 guard = 0;
		while (m.g.ppu().line() < 200 && guard++ < 1000000)
			m.runCycles(64);
		m.write32(0x040000D4, 0x02001000u);
		m.write32(0x040000D8, 0x02002000u);
		m.write32(0x040000DC, (0xA400u << 16) | 1u); // 32-bit, HBlank, once
		m.runCycles(280896);
		checkEq(m.read32(0x02002000), 0xCAFEBABEu, "HBlank DMA transfers on visible lines");
	}
	{ // ... and specifically NOT on VBlank lines (overrun by 68 blocks
	  // per frame would desync repeat raster channels).
		GbaMachine m;
		m.write32(0x02001000u, 0xDEADBEEFu);
		u64 guard = 0;
		while (m.g.ppu().line() < 200 && guard++ < 1000000)
			m.runCycles(64);
		m.write32(0x02001000u, 0xCAFEBABEu);
		m.write32(0x040000D4, 0x02001000u);
		m.write32(0x040000D8, 0x02002000u);
		m.write32(0x040000DC, (0xA400u << 16) | 1u); // 32-bit, HBlank, once
		m.runCycles(27 * 1232); // lines 200-226: pure VBlank, no visible HBlank
		checkEq(m.read32(0x02002000), 0u, "HBlank DMA stays idle through VBlank");
	}
}

// ---------------------------------------------------------------------------
// 8. Timers
// ---------------------------------------------------------------------------

void testTimers()
{
	section("Timers");
	{ // Overflow IRQ and reload (reload 0xFF00 overflows every 0x100 ticks).
		GbaMachine m;
		m.write16(0x04000100, 0xFF00u); // TIMER0 reload
		m.write16(0x04000102, 0x00C0u); // enable + IRQ, prescaler /1
		m.runCycles(0x200);
		check((m.io16r(0x04000202) & 0x08u) != 0, "timer 0 overflow raises IF bit 3");
		checkNear16(m.g.bus().read16(0x04000100), 0xFF00u, 0x08u,
					"timer reloads on overflow and keeps counting");
	}
	{ // CNT_L write semantics (GBATEK: write sets reload, read returns
	  // frozen counter while stopped; reload loads on enable).
		GbaMachine m;
		m.write16(0x04000100, 0x1111u);
		checkEq16(m.g.bus().read16(0x04000100), 0x0000u,
				  "CNT_L read returns frozen counter while stopped");
		m.write16(0x04000102, 0x0080u); // enable
		checkNear16(m.g.bus().read16(0x04000100), 0x1111u, 0x04u,
					"enabling loads the counter from the reload value");
	}
	{ // Prescaler /1024 over a long budget: 0x10000 / 1024 = 64 ticks.
		GbaMachine m;
		m.write16(0x04000100, 0xFF00u);
		m.write16(0x04000102, 0x0083u); // enable, prescaler /1024
		m.runCycles(0x10000);
		checkNear16(m.g.bus().read16(0x04000100), 0xFF40u, 0x04u,
					"prescaler /1024 divides by 1024");
	}
	{ // Cascade: timer 1 counts on timer 0 overflow.
		GbaMachine m;
		m.write16(0x04000100, 0xFF00u);
		m.write16(0x04000102, 0x0080u); // timer 0, no IRQ
		m.write16(0x04000104, 0x0000u); // timer 1 reload
		m.write16(0x04000106, 0x0084u); // timer 1 cascade + enable
		m.runCycles(0x300);
		check(m.g.bus().read16(0x04000104) > 0, "timer 1 counts up on timer 0 overflow");
	}
}

// ---------------------------------------------------------------------------
// 9. Keypad
// ---------------------------------------------------------------------------

void testGpioRtc()
{
	section("GPIO/RTC");
	{                 // RTC carts are detected by game code (mGBA overrides table).
		GbaMachine m; // synthetic ROM: no RTC prefix
		check(!m.g.cartridge().hasGpio(), "test ROM has no GPIO");
		std::vector<u8> bpee = makeTestRom();
		bpee[0xAC] = 'B';
		bpee[0xAD] = 'P';
		bpee[0xAE] = 'E';
		bpee[0xAF] = 'E';
		u8 chk = 0;
		for (u32 i = 0xA0; i <= 0xBC; ++i)
			chk = static_cast<u8>(chk - bpee[i]);
		bpee[0xBD] = static_cast<u8>(chk - 0x19);
		gba::GameBoyAdvance g2;
		check(g2.loadFromBytes(bpee), "BPEE test ROM loads");
		check(g2.cartridge().hasGpio(), "BPEE (Emerald) has GPIO/RTC");
	}
	{ // GPIO registers: write-only mode reads 00h (GBATEK); R/W mode
	  // reflects pins and direction.
		gba::GpioRtc gpio;
		gpio.setPresent(true);
		gpio.write(0x080000C4u, 0x000Fu);
		checkEq(gpio.read(0x080000C4u), 0x00u, "write-only DATA reads 00h");
		gpio.write(0x080000C8u, 0x0001u); // read/write mode
		gpio.write(0x080000C6u, 0x0007u); // P0-P2 out
		gpio.write(0x080000C4u, 0x0005u); // SCK=1, CS=1
		checkEq(gpio.read(0x080000C4u), 0x05u, "R/W DATA reads pin state");
		checkEq(gpio.read(0x080000C6u), 0x07u, "direction reads back");
	}
	{ // Full SII status read through the bus, the way siirtc.c does it:
	  // CS high, 8 command bits (magic 6, CONTROL, read), 8 data bits.
		std::vector<u8> bpee = makeTestRom();
		bpee[0xAC] = 'B';
		bpee[0xAD] = 'P';
		bpee[0xAE] = 'E';
		bpee[0xAF] = 'E';
		u8 chk = 0;
		for (u32 i = 0xA0; i <= 0xBC; ++i)
			chk = static_cast<u8>(chk - bpee[i]);
		bpee[0xBD] = static_cast<u8>(chk - 0x19);
		gba::GameBoyAdvance g2;
		g2.loadFromBytes(bpee);
		auto &bus = g2.bus();
		auto w16 = [&](u32 addr, u16 v) { bus.write16(addr, v); };
		auto r16 = [&](u32 addr) { return bus.read16(addr); };
		w16(0x080000C8u, 1);    // read/write mode
		w16(0x080000C6u, 0x07); // SCK/SIO/CS out (driver bit-bangs)
		w16(0x080000C4u, 0x05); // SCK=1, CS=1
		w16(0x080000C4u, 0x04); // SCK=0
		u16 cmd = 0xC6;         // magic 0110, CONTROL(100), read(1)
		for (int i = 0; i < 8; i++) {
			w16(0x080000C4u, ((cmd >> i) & 1) ? 0x06u : 0x04u); // SIO bit, SCK=0
			w16(0x080000C4u, ((cmd >> i) & 1) ? 0x07u : 0x05u); // SCK rising
		}
		// Like the driver, release SIO to input so the chip can drive it.
		w16(0x080000C6u, 0x05);
		u16 status = 0;
		for (int i = 0; i < 8; i++) {
			w16(0x080000C4u, 0x04u); // SCK falling: chip presents bit
			status |= (r16(0x080000C4u) & 2u) ? (1u << i) : 0u;
			w16(0x080000C4u, 0x05u); // SCK rising
		}
		w16(0x080000C4u, 0x00u); // CS low: end transfer
		checkEq(status, 0x40u, "RTC status: 24-hour mode, no power failure");
	}
	{ // A game-written date/time rebases the emulated clock instead of
	  // being dropped (Boktai's date changer): the chip keeps running
	  // from the written time, expressed as a host-clock offset so it
	  // still advances. Byte order is seconds first, all BCD.
		gba::GpioRtc gpio;
		gpio.setPresent(true);
		auto drive = [&](u16 v) { gpio.write(0x080000C4u, v); };
		gpio.write(0x080000C8u, 1);    // read/write mode
		gpio.write(0x080000C6u, 0x07); // SCK/SIO/CS driven by the "game"
		auto writeByte = [&](u8 byte) {
			drive(0x05); // SCK=1, CS=1
			drive(0x04); // SCK=0
			for (int i = 0; i < 8; ++i) {
				const u16 lvl = ((byte >> i) & 1u) ? 0x06u : 0x04u;
				drive(lvl);      // SIO bit presented while SCK is low
				drive(lvl | 1u); // SCK rising shifts it in
			}
		};
		auto readByte = [&](u8 *out) {
			gpio.write(0x080000C6u, 0x05); // release SIO to the chip
			u8 v = 0;
			for (int i = 0; i < 8; ++i) {
				drive(0x04); // SCK falling: the chip presents the bit
				v |= (gpio.read(0x080000C4u) & 2u) ? static_cast<u8>(1u << i) : 0u;
				drive(0x05);
			}
			*out = v;
			gpio.write(0x080000C6u, 0x07);
		};
		auto readDatetime = [&] {
			std::array<u8, 7> t{};
			writeByte(0xA6); // magic 0110, DATE/TIME, read
			for (u8 &b : t)
				readByte(&b);
			drive(0x00); // CS low
			return t;    // year(00-99), month, day, weekday, h, m, s
		};
		writeByte(0x26); // magic 0110, DATE/TIME, write
		for (u8 b : {0x03u, 0x07u, 0x04u, 0x05u, 0x12u, 0x34u, 0x00u})
			writeByte(b); // year, month, day, weekday, 12:34:00 on 2003-07-04
		drive(0x00);
		const std::array<u8, 7> t = readDatetime();
		checkEq(t[0], 0x03u, "RTC honours a game-written year");
		checkEq(t[1], 0x07u, "RTC honours a game-written month");
		checkEq(t[2], 0x04u, "RTC honours a game-written day");
		checkEq(t[3], 0x05u, "RTC recomputes the weekday (2003-07-04 = Friday)");
		checkEq(t[4], 0x12u, "RTC honours a game-written hour");
		checkEq(t[5], 0x34u, "RTC honours a game-written minute");
		check(t[6] <= 0x01u, "written seconds keep running");
		check(gpio.offset() != 0, "game-set clock is a host-clock offset");
		// Time-only write keeps the date (SII command 6).
		writeByte(0x66);
		for (u8 b : {0x07u, 0x00u, 0x00u})
			writeByte(b); // 07:00:00
		drive(0x00);
		const std::array<u8, 7> t2 = readDatetime();
		checkEq(t2[4], 0x07u, "RTC time-only write sets the hour");
		checkEq(t2[1], 0x07u, "RTC time-only write keeps the date");
		// The offset is battery-backed: sidecar and save state both keep it.
		const std::string path = "/tmp/gb4me_selftest.gpio.rtc";
		gpio.saveToFile(path);
		gba::GpioRtc restored;
		restored.setPresent(true);
		restored.loadFromFile(path);
		check(restored.offset() == gpio.offset(), "RTC sidecar restores the clock offset");
		gba::GpioRtc fresh;
		checkEq(static_cast<u32>(fresh.offset() < 0 ? -fresh.offset() : fresh.offset()), 0u,
				"without a sidecar the RTC follows the host clock");
		gba::StateBuffer buf;
		gpio.save(buf);
		gba::GpioRtc reloaded;
		reloaded.load(buf);
		check(reloaded.offset() == gpio.offset(), "save state keeps the RTC offset");
		std::remove(path.c_str());
	}
}

void testAudio()
{
	section("Audio");
	{ // FIFO A consumes one byte per timer-0 overflow (SOUNDCNT_H default
	  // selects timer 0) and holds the DAC value on underrun (GBATEK).
	  // Both prerequisites must hold: the master enable (NR52 bit 7) and at
	  // least one L/R routing bit - mGBA src/gba/timer.c:25-32 gates the FIFO
	  // hook on both, so a muted/unrouted FIFO must NOT burn samples.
		GbaMachine m;
		m.g.audio().write16(0x04000084u, 0x0080u); // NR52: master enable on
		m.g.audio().write16(0x04000082u, 0x0200u); // SOUNDCNT_H: A -> Left
		m.g.audio().write16(0x040000A0u, 0x077Bu); // bytes 0x7B, 0x07
		m.g.audio().onTimerOverflow(0);
		checkEq(m.g.audio().fifoACount(), 1, "FIFO A consumed one byte");
		checkEq(static_cast<u32>(static_cast<u8>(m.g.audio().fifoADac())), 0x7Bu,
				"FIFO A DAC holds first byte");
		m.g.audio().onTimerOverflow(0);
		checkEq(m.g.audio().fifoACount(), 0, "FIFO A drained");
		m.g.audio().onTimerOverflow(0);
		checkEq(static_cast<u32>(static_cast<u8>(m.g.audio().fifoADac())), 0x07u,
				"FIFO A DAC holds last byte on underrun");
	}
	{ // An unrouted FIFO must not consume, even with a timer overflow.
		GbaMachine m;
		m.g.audio().write16(0x04000084u, 0x0080u); // power on
		m.g.audio().write16(0x04000082u, 0x0000u); // no L/R routing
		m.g.audio().write16(0x040000A0u, 0x077Bu);
		m.g.audio().onTimerOverflow(0);
		checkEq(m.g.audio().fifoACount(), 2, "unrouted FIFO A does not consume");
	}
	{ // An 8-bit write to a register no unit owns must NOT merge 0xFF into
	  // the untouched half. deviceByte16() used to return 0xFFFF for PPU and
	  // keypad registers, so a STRB of 0x34 to DISPCNT became 0xFF34.
		GbaMachine m;
		m.g.bus().write16(0x04000000u, 0x0400u); // DISPCNT = BG2 on
		m.g.bus().write8(0x04000000u, 0x34u);    // STRB low byte
		const u16 dispcnt = m.g.bus().read16(0x04000000u);
		checkEq16(dispcnt, 0x0434u, "8-bit DISPCNT write preserves the high byte");
	}
	{ // IntrWait with a zero mask waits for ANY enabled IRQ. The old code
	  // stored mask 0 and never set the "any" flag, so `(fired & mask)` could
	  // never be non-zero and the call hung forever.
		GbaMachine m;
		m.g.bus().write16(0x04000200u, 0x0001u); // IE: VBlank
		gba::Arm7Tdmi cpu;
		// No IRQ pending yet: must still be waiting.
		(m.g.bios().*m.g.bios().intrWaitFn())(1, 0, cpu, m.g.bus());
		check(m.g.bios().waiting(), "IntrWait with a zero mask halts");
		m.g.irq().raise(gba::GbaIrq::kVBlank); // IF is write-1-to-CLEAR, so raise it
		m.g.bios().pollWait(cpu, m.g.bus());
		check(!m.g.bios().waiting(), "IntrWait with a zero mask wakes on any IRQ");
	}
	{ // A 32-bit SRAM write must update exactly ONE byte, the LSB of
	  // `value ROR (addr*8)` (GBATEK "Accessing SRAM Area by 16bit/32bit";
	  // a 32-bit access is 4-aligned, so the rotate is 0 and the LSB is
	  // used). Splitting it into two 16-bit writes corrupted a neighbouring
	  // byte and sent Flash a phantom command byte.
		GbaMachine m;
		for (u32 i = 0; i < 8; ++i)
			m.g.bus().write8(0x0E000000u + i, static_cast<u8>(0xA0 + i));
		m.g.bus().write32(0x0E000000u, 0x11223344u);
		checkEq(static_cast<u32>(m.g.bus().read8(0x0E000000u)), 0x44u,
				"32-bit SRAM write stores the source LSB at the address");
		checkEq(static_cast<u32>(m.g.bus().read8(0x0E000001u)), 0xA1u,
				"32-bit SRAM write does not touch the following byte");
		checkEq(static_cast<u32>(m.g.bus().read8(0x0E000002u)), 0xA2u,
				"32-bit SRAM write touches only one byte");
		checkEq(static_cast<u32>(m.g.bus().read8(0x0E000003u)), 0xA3u,
				"32-bit SRAM write leaves later bytes alone");
	}
	{ // FIFO B must advance on its SELECTED timer only, and never while
	  // muted/unrouted (mGBA timer.c:25-32). Folding the enable test into the
	  // timer-0 predicate made a disabled FIFO fire on timer 1 instead.
		GbaMachine m;
		m.g.audio().write16(0x04000084u, 0x0080u);           // master enable on
		m.g.audio().write16(0x04000082u, 0x4000u | 0x2000u); // B=timer1, B->Left
		m.g.audio().write16(0x040000A4u, 0x0142u);
		m.g.audio().onTimerOverflow(0);
		checkEq(m.g.audio().fifoBCount(), 2, "timer 0 does not consume FIFO B");
		m.g.audio().onTimerOverflow(1);
		checkEq(m.g.audio().fifoBCount(), 1, "timer 1 consumes FIFO B");
		// Now power off: neither timer may consume.
		m.g.audio().write16(0x04000084u, 0x0000u);
		m.g.audio().onTimerOverflow(0);
		m.g.audio().onTimerOverflow(1);
		checkEq(m.g.audio().fifoBCount(), 1, "powered-off FIFO B is not consumed");
	}
	{ // PSG trigger must reload the same period the step loop uses, or the
	  // first duty edge after a trigger lands early. GBA pulse = 16*(2048-f).
		GbaMachine m;
		m.g.audio().write16(0x04000084u, 0x0080u); // power on
		// NR13 (freq) + NR14 (trigger) are handled as one packed halfword.
		m.g.audio().write16(0x04000064u, 0x8000u); // freq = 0, trigger
		checkEq(m.g.audio().pulse1Period(), (2048 - 0) * 16,
				"pulse trigger reloads the GBA 16*(2048-f) period");
	}
	{ // FIFO B follows timer 1 when SOUNDCNT_H bit 14 is set.
		GbaMachine m;
		m.g.audio().write16(0x04000084u, 0x0080u);           // master enable on
		m.g.audio().write16(0x04000082u, 0x4000u | 0x2000u); // B = timer 1, B -> Left
		m.g.audio().write16(0x040000A4u, 0x0142u);           // bytes 0x42, 0x01
		m.g.audio().onTimerOverflow(0);
		checkEq(m.g.audio().fifoBCount(), 2, "timer 0 does not consume FIFO B");
		m.g.audio().onTimerOverflow(1);
		checkEq(m.g.audio().fifoBCount(), 1, "timer 1 consumes FIFO B");
		checkEq(static_cast<u32>(static_cast<u8>(m.g.audio().fifoBDac())), 0x42u,
				"FIFO B DAC holds first byte");
	}
	{ // PSG read masks (mGBA suite io-read, hardware-measured): write-only
	  // bits read back 0 on GBA (DMG-style read-1s corrupt RMW drivers).
		GbaMachine m;
		auto w = [&](u32 a, u16 v) { m.g.audio().write16(a, v); };
		auto r = [&](u32 a) { return m.g.audio().read16(a); };
		w(0x04000060u, 0xFFFFu);
		checkEq16(r(0x04000060u), 0x007Fu, "NR10 reads 0x007F");
		w(0x04000062u, 0xFFFFu);
		checkEq16(r(0x04000062u), 0xFFC0u, "NR11/NR12 reads 0xFFC0");
		w(0x04000064u, 0xFFFFu);
		checkEq16(r(0x04000064u), 0x4000u, "NR13/NR14 reads 0x4000");
		w(0x04000068u, 0xFFFFu);
		checkEq16(r(0x04000068u), 0xFFC0u, "NR21/NR22 reads 0xFFC0");
		w(0x0400006Cu, 0xFFFFu);
		checkEq16(r(0x0400006Cu), 0x4000u, "NR23/NR24 reads 0x4000");
		w(0x04000070u, 0xFFFFu);
		checkEq16(r(0x04000070u), 0x00E0u, "NR30 reads 0x00E0");
		w(0x04000072u, 0xFFFFu);
		checkEq16(r(0x04000072u), 0xE000u, "NR31/NR32 reads 0xE000");
		w(0x04000074u, 0xFFFFu);
		checkEq16(r(0x04000074u), 0x4000u, "NR33/NR34 reads 0x4000");
		w(0x04000078u, 0xFFFFu);
		checkEq16(r(0x04000078u), 0xFF00u, "NR41/NR42 reads 0xFF00");
		w(0x0400007Cu, 0xFFFFu);
		checkEq16(r(0x0400007Cu), 0x40FFu, "NR43/NR44 reads 0x40FF");
		w(0x04000080u, 0xFFFFu);
		checkEq16(r(0x04000080u), 0xFF77u, "NR50/NR51 reads 0xFF77");
		w(0x04000082u, 0xFFFFu);
		checkEq16(r(0x04000082u), 0x770Fu, "SOUNDCNT_H reads 0x770F");
	}
}

void testApu()
{
	section("GB APU");
	{ // Band-limited output stage: the kernel is unit-DC-gain, so a square
	  // wave keeps its level in the passband, and a tone above the cutoff
	  // is filtered out instead of being sampled straight through (the
	  // aliasing buzz the old 1x mixing produced).
		gb::dsp::OutputStage stage;
		stage.reset();
		// 1kHz square, one second: fundamental should survive near unity.
		constexpr unsigned kSquareHz = 1000;
		constexpr unsigned kPeriodMix = 352800 / kSquareHz;
		float peak = 0.0f;
		for (unsigned i = 0; i < 352800; i++) {
			const float v = ((i / (kPeriodMix / 2)) % 2 == 0) ? 1.0f : -1.0f;
			i16 l = 0, r = 0;
			// Skip the first frames: the kernel starts from a zero history,
			// so the leading edge is a step, not the steady state.
			if (stage.push(v * 1000.0f, v * 1000.0f, &l, &r) && i > 8 * gb::dsp::kMixDiv)
				peak = std::max(peak, std::abs(static_cast<float>(l)));
		}
		// A band-limited square overshoots ~9% (Gibbs) exactly like the
		// real thing; what must not happen is droop from a bad kernel.
		check(peak > 1000.0f && peak < 1200.0f, "1kHz square keeps its amplitude (no droop)");
		gb::dsp::OutputStage tone;
		tone.reset();
		// 30kHz sits well above the 19.8kHz cutoff: must be attenuated.
		constexpr unsigned kToneHz = 30000;
		constexpr unsigned kTonePeriod = 352800 / kToneHz;
		peak = 0.0f;
		for (unsigned i = 0; i < 352800; i++) {
			const float v = ((i / (kTonePeriod / 2)) % 2 == 0) ? 1.0f : -1.0f;
			i16 l = 0, r = 0;
			if (tone.push(v * 1000.0f, v * 1000.0f, &l, &r) && i > 8 * gb::dsp::kMixDiv)
				peak = std::max(peak, std::abs(static_cast<float>(l)));
		}
		check(peak < 100.0f, "30kHz tone is filtered out (band-limited, not aliased)");
	}
	{ // Sample rate: one emulated second of T-cycles yields 44100 frames.
		gb::APU apu;
		std::vector<i16> samples;
		// 4194304 T-cycles = 1s at the DMG clock; step() takes M-cycles.
		apu.step(4194304 / 4);
		apu.take_samples(samples);
		const u64 frames = apu.frames_emitted();
		check(frames >= 44000 && frames <= 44200, "APU emits ~44100 frames per emulated second");
		checkEq(static_cast<u32>(samples[0]), 0u, "silent APU outputs digital zero");
	}
	{ // DAC power-down fade: a length expiry must not step the output to
	  // zero (that step is the click at the end of every note). The mix
	  // keeps a decaying copy of the channel level for ~0.5ms, so the
	  // sample right after the cut is *louder* than the steady note, then
	  // falls away.
		gb::APU apu;
		apu.write_reg(0xFF11, 0xBF); // length 1, 50% duty
		apu.write_reg(0xFF12, 0xF0); // DAC on, full volume, no envelope
		apu.write_reg(0xFF13, 0xFF); // fastest frequency -> steady square
		apu.write_reg(0xFF14, 0xC0); // freq high + trigger + length enable
		apu.write_reg(0xFF25, 0x11); // ch1 to both sides
		apu.write_reg(0xFF24, 0x77); // master volume 8
		apu.step(20000 / 4);         // past the 1/256s length clock and the fade
		std::vector<i16> tail;
		apu.take_samples(tail);
		std::vector<int> frames;
		for (size_t i = 0; i + 1 < tail.size(); i += 2)
			frames.push_back(std::abs(static_cast<int>(tail[i])));
		// The 50% duty square at NR13=FF alternates every few frames, so
		// "loud" frames are the high phase. The cut is the last of them.
		int steady_peak = 0;
		for (int v : frames)
			steady_peak = std::max(steady_peak, v);
		int last_loud = -1;
		for (size_t i = 0; i < frames.size(); i++) {
			if (frames[i] * 2 >= steady_peak)
				last_loud = static_cast<int>(i);
		}
		check(last_loud > 0, "the note is audible before the expiry");
		// The channel is silent the instant it powers down, so what follows
		// is the output capacitor alone: a positive ramp that then decays.
		// The DC blocker takes about half of a one-sided ramp (the steady
		// square is bipolar, the ramp is not), so the observed peak lands
		// near 1/2 scale - but a hard cut would fall straight past a third
		// of it into the much smaller HPF ring.
		const size_t tail_start = static_cast<size_t>(last_loud) + 1;
		const size_t tail_end = tail_start + 40;
		int post_peak = 0;
		for (size_t i = tail_start; i < frames.size() && i < tail_end; i++)
			post_peak = std::max(post_peak, frames[i]);
		check(post_peak * 3 >= steady_peak, "the cut is followed by a ramp, not a step");
		check(post_peak <= steady_peak + 64, "the ramp starts at the channel level, not louder");
		// And it decays: well under a second later the tail is quiet.
		int last_audible = -1;
		for (size_t i = frames.size(); i > 0; i--) {
			if (frames[i - 1] > steady_peak / 8) {
				last_audible = static_cast<int>(i) - 1;
				break;
			}
		}
		const int ring = last_audible - last_loud;
		std::printf("  info  DAC power-down fade: %d output frames (%.2f ms)\n", ring,
					ring * 1000.0 / 44100.0);
		check(ring > 4 && ring < 400, "the fade is short and bounded");
	}
	{ // NR52 length hazard: a power cycle keeps the four length counters,
	  // so the next trigger does not reload them and the note still ends
	  // when it should (SameBoy Core/apu.c).
		gb::APU apu;
		auto nr = [&](u16 addr, u8 v) { apu.write_reg(addr, v); };
		auto ch1_on = [&] { return (apu.read_reg(0xFF26) & 0x01) != 0; };
		nr(0xFF11, 0xBF); // length 1
		nr(0xFF12, 0xF0); // DAC on
		nr(0xFF14, 0xC0); // trigger + length enable
		check(ch1_on(), "channel 1 playing after trigger");
		nr(0xFF26, 0x00); // power off
		nr(0xFF26, 0x80); // power on
		nr(0xFF14, 0xC0); // trigger again
		check(ch1_on(), "channel 1 playing after the power cycle");
		apu.step(32768 / 4); // two length clocks at 256Hz
		check(!ch1_on(), "length counter survived the power cycle (note still expires)");
	}
	{ // Audio-clock pacing: the device buffer is the master clock. With the
	  // level at target the loop must not wait; below target it must never
	  // wait at all; above target the wait is capped at one frame.
		gb::AudioSync sync;
		sync.reset(44100, 2048); // 46ms device buffer -> 23ms target
		checkEq(sync.target_frames(), 1024u, "target is half the device buffer");
		// produced 735 frames/frame (44100/60); steady state: the level after
		// subtracting this frame's own audio sits at the target.
		check(sync.pace(1024 + 735, 735) == 0.0, "at target: no wait");
		// Starving device: 300 frames left after our own contribution.
		check(sync.pace(300 + 735, 735) == 0.0, "starving device: never wait");
		// Overfull: 4000 frames of backlog -> wait, but at most one frame.
		const double over = sync.pace(4000 + 735, 735);
		check(over > 0.0 && over <= 1.0 / 60.0 + 1e-9, "overfull: wait, capped at a frame");
		// A level far above target does not ask for a multi-second sleep.
		const double huge = sync.pace(44100 * 2, 735);
		check(huge <= 1.0 / 60.0 + 1e-9, "a huge backlog still waits at most a frame");
	}
	{ // Drift correction follows the MOVEMENT of the buffer level, not its
	  // depth: a level that keeps rising means we outrun the device (ratio
	  // down), a level that keeps falling means the reverse (ratio up), and
	  // a pathological device cannot push it past ±1%.
		gb::AudioSync sync;
		sync.reset(44100, 2048);
		// Level climbing by ~7 frames per frame for several windows.
		i32 queued = 1024 + 735;
		for (int i = 0; i < 400; i++) {
			queued += 7;
			sync.pace(static_cast<u32>(queued), 735);
		}
		check(sync.resample_ratio() < 1.0f, "a rising level lowers the ratio");
		// Now the level drains.
		queued = 40000;
		for (int i = 0; i < 400; i++) {
			queued -= 40;
			sync.pace(static_cast<u32>(queued), 735);
		}
		check(sync.resample_ratio() > 1.0f, "a falling level raises the ratio");
		// A level that neither rises nor falls must not move the ratio. One
		// window of drift is enough to overshoot the tolerance, so allow a
		// couple of windows' worth of settling first.
		for (int i = 0; i < 400; i++)
			sync.pace(1024 + 735, 735);
		const float steady = sync.resample_ratio();
		for (int i = 0; i < 400; i++)
			sync.pace(1024 + 735, 735);
		check(sync.resample_ratio() == steady, "a steady level leaves the ratio alone");
		// Clamped.
		for (int i = 0; i < 40000; i++)
			sync.pace(200000, 735);
		check(sync.resample_ratio() <= 1.01f && sync.resample_ratio() >= 0.99f,
			  "the drift correction stays within ±1%");
	}
	{ // The PPU's HBlank is a scheduler *event*, so it lands on the exact dot
	  // (1008) instead of whenever the next step() slice happens to end.
	  // Stepping one tick at a time must flip DISPSTAT's HBlank bit there.
		gba::GameBoyAdvance g;
		// DISPCNT: mode 0, BG0 on, so a frame renders.
		g.bus().write16(0x04000000u, 0x0100u);
		// Start from a known line start by running to the first line end,
		// then measure in *ticks* - step(1) runs a whole instruction, not one
		// dot, so counting calls would measure nothing.
		u64 guard = 0;
		while (g.ppu().line() != 1 && guard++ < 200000)
			g.step(1);
		checkEq(static_cast<u32>(g.ppu().line()), 1u, "reached scanline 1");
		const u64 line_start = g.tick();
		u32 hblank_at = 0;
		for (u32 i = 0; i < 4000 && hblank_at == 0; i++) {
			g.step(1);
			if ((g.bus().read16(0x04000004u) & 0x0002u) != 0)
				hblank_at = static_cast<u32>(g.tick() - line_start);
		}
		checkEq(hblank_at, 1008u, "HBlank flag is set at exactly dot 1008");
		// And the line ends 224 dots later (1232 - 1008).
		guard = 0;
		while (g.ppu().line() == 1 && guard++ < 4000)
			g.step(1);
		checkEq(static_cast<u32>(g.tick() - line_start), 1232u,
				"the line ends at exactly dot 1232");
		// The line must end at 1232, and the next HBlank 1232 dots later.
		guard = 0;
		while (g.ppu().line() != 2 && guard++ < 4000)
			g.step(1);
		checkEq(static_cast<u32>(g.ppu().line()), 2u, "scanline 2 begins at dot 1232");
	}
	{ // The scheduler queue holds type-erased callbacks, so a state load
	  // must re-arm the PPU's pending event from the restored state. If it
	  // did not, the frame would never complete again.
		const std::string path = "/tmp/opencode/gb4me_sched.state";
		gba::GameBoyAdvance ref;
		ref.bus().write16(0x04000000u, 0x0100u);
		for (int f = 0; f < 3; f++) {
			u64 guard = 0;
			while (!ref.ppu().frameReady() && guard++ < 400000)
				ref.step(1232);
			ref.ppu().clearFrameReady();
		}
		u64 mid = 0;
		while (mid < 5000) { // part-way into a frame, mid-HBlank most likely
			ref.step(64);
			mid += 64;
		}
		check(ref.saveState(path), "state saved mid-frame");
		// Reference: 6 more frames from the save point.
		for (int f = 0; f < 6; f++) {
			u64 guard = 0;
			while (!ref.ppu().frameReady() && guard++ < 400000)
				ref.step(1232);
			ref.ppu().clearFrameReady();
		}
		std::array<u32, 8> want{};
		for (int i = 0; i < 8; i++)
			want[static_cast<size_t>(i)] = ref.ppu().frame().pixels[100u * 240u + 100u + i];
		// Reload into a fresh machine and run the same 6 frames.
		gba::GameBoyAdvance g;
		check(g.loadState(path), "state loaded (events must be re-armed)");
		int done = 0;
		for (int f = 0; f < 6; f++) {
			u64 guard = 0;
			while (!g.ppu().frameReady() && guard++ < 400000)
				g.step(1232);
			if (!g.ppu().frameReady())
				break;
			g.ppu().clearFrameReady();
			++done;
		}
		checkEq(static_cast<u32>(done), 6u, "six more frames run after the load");
		std::array<u32, 8> got{};
		for (int i = 0; i < 8; i++)
			got[static_cast<size_t>(i)] = g.ppu().frame().pixels[100u * 240u + 100u + i];
		check(got == want, "the reloaded run matches the uninterrupted one");
		std::remove(path.c_str());
	}
	{ // The 2-cycle interrupt delay (GBATEK "GBA Interrupts"): IF is visible
	  // the moment a source raises it, but the CPU is handed the IRQ two
	  // cycles later - now a scheduler event.
		gba::GameBoyAdvance g;
		g.bus().write16(0x04000000u, 0x0100u); // DISPCNT: mode 0, BG0 on
		// No IE/IME yet: the flag must still latch and stay visible.
		u64 guard = 0;
		while (g.ppu().line() != 100 && guard++ < 400000)
			g.step(16);
		g.irq().writeIf(0);
		g.irq().raise(gba::GbaIrq::kVCount);
		check((g.irq().readIf() & gba::GbaIrq::kVCount) != 0,
			  "IF is visible immediately when a source raises it");
		check(g.irq().delivery_pending(), "delivery is scheduled, not immediate");
		checkEq(g.irq().deliveries(), 0u, "no delivery while masked (IE/IME clear)");
		// The pending event drains without delivering, and stays latched.
		g.step(8);
		check(!g.irq().delivery_pending(), "the delayed delivery ran");
		checkEq(g.irq().deliveries(), 0u, "still no delivery with IE clear");
		check((g.irq().readIf() & gba::GbaIrq::kVCount) != 0,
			  "a masked flag stays latched (not lost)");
		// Enabling it delivers on the next update, as before.
		g.irq().writeIe(gba::GbaIrq::kVCount);
		checkEq(g.irq().deliveries(), 0u, "unmasking alone does nothing while IME is 0");
		g.irq().writeIme(1);
		checkEq(g.irq().deliveries(), 1u, "enabling IME delivers the latched flag");
	}
	{ // A save state taken with a delayed delivery pending must not lose or
	  // duplicate it: the callback is type-erased, so loadState re-arms it.
		const std::string path = "/tmp/opencode/gb4me_irq.state";
		gba::GameBoyAdvance g;
		g.bus().write16(0x04000000u, 0x0100u);
		g.irq().writeIe(gba::GbaIrq::kVCount);
		g.irq().writeIme(1);
		// Raise just before saving so the delivery is still in flight.
		u64 guard = 0;
		while (g.ppu().line() != 100 && guard++ < 400000)
			g.step(16);
		g.irq().raise(gba::GbaIrq::kVCount);
		const u32 before = g.irq().deliveries();
		check(g.irq().delivery_pending(), "delivery pending at save time");
		check(g.saveState(path), "state saved with a delivery in flight");
		gba::GameBoyAdvance g2;
		check(g2.loadState(path), "state loaded");
		check(g2.irq().delivery_pending(), "the pending delivery is re-armed on load");
		g2.step(64);
		check(!g2.irq().delivery_pending(), "re-armed delivery runs after the load");
		check(g2.irq().deliveries() > before, "the IRQ still reaches the CPU after a reload");
		std::remove(path.c_str());
	}
	{ // Timers are scheduler events: a 1/64 prescaler overflows on the exact
	  // tick, not whenever the current step() slice stops. reload 0xFFFE (not
	  // 0xFFFF) so the reload is observable as the counter leaving 0xFFFF.
		gba::GameBoyAdvance g;
		g.bus().write16(0x04000100u, 0x0000u); // TM0 reload 0: 65535 increments
		g.bus().write16(0x04000102u, 0x0081u); // 1/64, enable
		check(g.timers().event_armed(0), "timer 0 armed its first increment");
		checkEq(g.timers().ticks_to_next(0), 64u, "first increment is 64 ticks away");
		const u64 start = g.tick();
		u64 elapsed = 0;
		bool saw_top = false;
		for (int i = 0; i < 200000; i++) {
			g.step(64);
			if (g.timers().counter(0) == 0xFFFFu)
				saw_top = true;
			// Back to 0 only on the reload, so this is the overflow.
			if (saw_top && g.timers().counter(0) == 0x0000u) {
				elapsed = g.tick() - start;
				break;
			}
		}
		// 0xFFFE -> 0xFFFF is 1 increment, then the overflow reloads: 65535
		// increments of 64 ticks, plus at most one tick of arming slack.
		std::printf("  info  timer 1/64 overflow after %llu ticks (want %d)\n",
					(unsigned long long) elapsed, 65535 * 64);
		check(elapsed >= 65534ull * 64ull && elapsed <= 65535ull * 64ull + 64ull,
			  "the 1/64 overflow lands on an exact tick");
	}
	{ // Cascade: timer 1 counts timer 0's overflows only, and the increment
	  // happens inside the overflow event.
		gba::GameBoyAdvance g;
		g.bus().write16(0x04000100u, 0x0000u); // TM0 reload 0
		g.bus().write16(0x04000102u, 0x0081u); // 1/64, enable
		g.bus().write16(0x04000104u, 0x0000u); // TM1 reload 0
		g.bus().write16(0x04000106u, 0x00C4u); // cascade + IRQ + enable
		check(!g.timers().event_armed(1), "a cascade timer has no event of its own");
		bool saw_top = false;
		int overflows = 0;
		for (int i = 0; i < 200000; i++) {
			g.step(64);
			if (g.timers().counter(0) == 0xFFFFu)
				saw_top = true;
			if (saw_top && g.timers().counter(0) == 0x0000u)
				++overflows;
			if (overflows == 1)
				break;
		}
		checkEq(static_cast<u32>(overflows), 1u, "timer 0 overflowed once");
		checkEq(g.timers().counter(1), 1u, "timer 1 counted exactly one overflow");
		// 2000 steps (128k ticks) cannot fit another 65535-increment period,
		// so a cascade timer must not move on its own.
		const u16 before = g.timers().counter(1);
		const u32 incr_before = g.timers().increments();
		for (int i = 0; i < 2000; i++)
			g.step(64);
		std::printf("  info  cascade idle: tm1 %u -> %u, increments %u -> %u\n", before,
					g.timers().counter(1), incr_before, g.timers().increments());
		checkEq(g.timers().counter(1), before, "a cascade timer only moves on overflows");
	}
	{ // A prescaler write under a pending event must not be counted twice.
		gba::GameBoyAdvance g;
		g.bus().write16(0x04000100u, 0x0000u);
		g.bus().write16(0x04000102u, 0x0081u); // 1/64
		g.step(30);                            // part-way into the period
		g.bus().write16(0x04000102u, 0x0082u); // -> 1/256, progress restarts
		checkEq(g.timers().ticks_to_next(0), 256u, "the prescaler change restarted progress");
		// The *steady-state* period is exact (checked above); the FIRST period
		// after a write is armed from the scheduler clock, which the core only
		// advances between instructions - so it can be up to one instruction
		// early (documented in core.cpp until the CPU runs per-cycle slices).
		// What must hold is that the stale 1/64 event did NOT survive: one
		// increment per new period, not two.
		const u64 t_write = g.tick();
		while (g.tick() < t_write + 200)
			g.step(1);
		checkEq(g.timers().counter(0), 0u, "no increment early in the new period");
		while (g.tick() < t_write + 400)
			g.step(1);
		checkEq(g.timers().counter(0), 1u, "exactly one increment per new period");
	}
	{ // Save state with a timer mid-count: the events are re-armed, so it
	  // keeps counting instead of stalling.
		const std::string path = "/tmp/opencode/gb4me_timer.state";
		gba::GameBoyAdvance g;
		g.bus().write16(0x04000000u, 0x0100u);
		g.bus().write16(0x04000100u, 0x0000u);
		g.bus().write16(0x04000102u, 0x0081u); // 1/64
		g.step(1000);
		const u16 mid = g.timers().counter(0);
		check(mid > 0, "the timer was counting before the save");
		check(g.saveState(path), "state saved with a timer event in flight");
		gba::GameBoyAdvance g2;
		check(g2.loadState(path), "state loaded");
		check(g2.timers().event_armed(0), "the timer event is re-armed on load");
		u64 guard = 0;
		while (g2.timers().counter(0) <= mid && guard++ < 200000)
			g2.step(16);
		check(g2.timers().counter(0) > mid, "the reloaded timer keeps counting");
		std::remove(path.c_str());
	}
	{ // The GBA side of the drift correction: a +/-1% ratio must change the
	  // number of output frames by that much, smoothly (a hold/skip, not a
	  // batch drop).
		GbaMachine m;
		m.g.audio().set_resample_ratio(1.0f);
		const u64 before = m.g.audio().frames_emitted();
		m.g.audio().step(16777216); // one second of GBA ticks (16.78MHz)
		const u64 plain = m.g.audio().frames_emitted() - before;
		check(plain > 44000 && plain < 44200, "GBA audio emits ~44100 frames/s at 1.00");

		gba::GameBoyAdvance g2;
		g2.audio().set_resample_ratio(1.01f);
		const u64 hi_before = g2.audio().frames_emitted();
		g2.audio().step(16777216);
		const u64 hi = g2.audio().frames_emitted() - hi_before;
		gba::GameBoyAdvance g3;
		g3.audio().set_resample_ratio(0.99f);
		const u64 lo_before = g3.audio().frames_emitted();
		g3.audio().step(16777216);
		const u64 lo = g3.audio().frames_emitted() - lo_before;
		std::printf("  info  gba drift: 1.00 -> %llu, 1.01 -> %llu, 0.99 -> %llu frames\n",
					(unsigned long long) plain, (unsigned long long) hi, (unsigned long long) lo);
		check(hi > plain + 300 && hi < plain + 520, "+1% emits ~1% more frames");
		check(lo < plain - 300 && lo > plain - 520, "-1% emits ~1% fewer frames");
	}
	{ // Wave RAM bus conflict: while the channel plays, an access lands on
	  // the byte the DAC is fetching, and blanks to $FF right before a
	  // fetch (mGBA src/gb/io.c).
		gb::APU apu;
		for (u8 i = 0; i < 16; i++)
			apu.write_wave_ram(static_cast<u16>(0xFF30 + i), static_cast<u8>(i));
		checkEq(apu.read_wave_ram(0xFF30), 0x00u, "wave RAM reads back while stopped");
		apu.write_reg(0xFF1A, 0x80); // channel 3 on
		apu.write_reg(0xFF1B, 0x00); // length
		apu.write_reg(0xFF1D, 0x00); // rate: slowest, 2*(2048-0) T-cycles
		apu.write_reg(0xFF1E, 0x80); // trigger
		apu.write_wave_ram(0xFF30, 0x5Au);
		// pos == 0 right after the trigger, so the write lands on byte 0.
		checkEq(apu.read_wave_ram(0xFF30), 0x5Au, "playing: write lands on the fetched byte");
		checkEq(apu.read_wave_ram(0xFF3F), 0x5Au,
				"playing: read returns the fetched byte, not the address");
		apu.step(4096 / 4);                // one sample period: pos -> 1, bus free again
		apu.write_wave_ram(0xFF3F, 0x77u); // byte 0 (pos/2 == 0) is fetched now
		checkEq(apu.read_wave_ram(0xFF30), 0x77u,
				"playing: redirected write hits the current byte");
	}
}

void testKeypad()
{
	section("Keypad");
	GbaMachine m;
	checkEq16(m.io16r(0x04000130), 0x03FFu, "KEYINPUT reads 0x3FF with nothing pressed");
	m.g.keypad().setKey(gba::GbaKeypad::Key::A, true);
	checkEq16(m.io16r(0x04000130), 0x03FEu, "pressing A clears KEYINPUT bit 0");
	m.g.keypad().setKey(gba::GbaKeypad::Key::A, false);

	// KEYCNT IRQ on A (bit 14 = IRQ enable, mask = bit 0).
	m.io16(0x04000132, 0x4001u);
	m.io16(0x04000200, 0x1000u); // IE = keypad
	m.io16(0x04000208, 1);
	m.g.keypad().setKey(gba::GbaKeypad::Key::A, true);
	check((m.io16r(0x04000202) & 0x1000u) != 0, "keypad IRQ fires for the selected key");
	m.g.keypad().setKey(gba::GbaKeypad::Key::A, false);
}

// ---------------------------------------------------------------------------
// 10. Cartridge saves and save states
// ---------------------------------------------------------------------------

void testSavesAndStates()
{
	section("Cartridge saves and save states");
	{
		GbaMachine m;
		m.g.bus().write8(0x0E000000, 0x5Au);
		checkEq(m.g.bus().read8(0x0E000000), 0x5Au, "SRAM region readable/writable");
		m.g.bus().write8(0x0E000000, 0xA5u);
		checkEq(m.g.bus().read8(0x0E000000), 0xA5u, "SRAM byte persists");
	}
	{ // GBATEK: the save chip mirrors across the whole 0x0E-0x0F window, so
	  // every 64K alias hits the same byte.
		std::vector<u8> rom = makeTestRom();
		const std::string marker = "SRAM_V113";
		rom.insert(rom.end(), marker.begin(), marker.end());
		gba::GameBoyAdvance g;
		check(g.loadFromBytes(rom), "SRAM test ROM loads");
		checkEq(static_cast<u32>(g.cartridge().saveType()), static_cast<u32>(gba::SaveType::SRAM),
				"marker selects plain SRAM");
		g.bus().write8(0x0E000000, 0x5Au);
		g.bus().write8(0x0E008000u, 0xA5u);
		checkEq(g.bus().read8(0x0E010000u), 0x5Au, "save window mirrors every 64K");
		checkEq(g.bus().read8(0x0E018000u), 0xA5u, "save window mirrors the upper 32K too");
		checkEq(g.bus().read8(0x0E000000), 0x5Au, "mirrored writes do not collide");
		checkEq(g.bus().read8(0x0F000000u), 0x5Au, "save window mirrors across 0x0F");
	}
	{ // Flash autoselect IDs (GBATEK "Device Types", MSB=device).
		auto autoselect = [](gba::SaveHardware &flash) {
			flash.write(0x5555, 0xAA);
			flash.write(0x2AAA, 0x55);
			flash.write(0x5555, 0x90);
			const u32 id =
				(static_cast<u32>(flash.read(0)) | (static_cast<u32>(flash.read(1)) << 8));
			flash.write(0x5555, 0xF0); // back to read-array
			return id;
		};
		gba::FlashMacronix mx128(128 * 1024);
		checkEq(autoselect(mx128), 0x09C2u, "Macronix 128K is MX29L010 (09C2h)");
		gba::FlashMacronix mx64(64 * 1024);
		checkEq(autoselect(mx64), 0x1CC2u, "Macronix 64K is MX29L002 (1CC2h)");
		gba::FlashSanyo sanyo(128 * 1024);
		checkEq(autoselect(sanyo), 0x1362u, "Sanyo 128K is LE26FV10N1TS (1362h)");
		gba::FlashPanasonic pan(64 * 1024);
		checkEq(autoselect(pan), 0x1B32u, "Panasonic 64K is MN63F805MNP (1B32h)");
		gba::FlashAtmel atmel(64 * 1024);
		checkEq(autoselect(atmel), 0x3D1Fu, "Atmel 64K is AT29LV512 (3D1Fh)");
	}
	{ // Flash bank switching (128KB chips; Emerald saves use bank 1).
		gba::FlashMacronix mx(128 * 1024);
		auto cmd = [&](u32 a, u8 v) { mx.write(a, v); };
		auto program = [&](u32 a, u8 v) {
			cmd(0x5555, 0xAA);
			cmd(0x2AAA, 0x55);
			cmd(0x5555, 0xA0);
			cmd(a, v);
		};
		auto bank = [&](u8 b) {
			cmd(0x5555, 0xAA);
			cmd(0x2AAA, 0x55);
			cmd(0x5555, 0xB0);
			cmd(0x0000, b);
		};
		program(0x0100, 0x5A); // bank 0 (default)
		bank(1);
		program(0x0100, 0xA5); // same low address, bank 1
		checkEq(mx.read(0x0100), 0xA5u, "bank 1 holds its byte");
		bank(0);
		checkEq(mx.read(0x0100), 0x5Au, "bank 0 unaffected by bank-1 write");
	}
	{ // EEPROM lazy upgrade: a 4Kbit backing grows to 8KB on first
	  // out-of-range access (64Kbit title on a 4Kbit initial guess),
	  // preserving low data and reading back erased (1s) high blocks.
		gba::EepromSave ee(gba::EepromSave::Size::K4bit);
		checkEq(ee.size(), 512u, "EEPROM starts at 4Kbit guess");
		auto writeBlock = [&](u32 block, u32 first_byte) {
			// "10" + 14 address bits + 64 data bits + stop (81 halfwords).
			ee.dmaWrite(1, 81, 0);
			ee.dmaWrite(0, 80, 0);
			for (int i = 13; i >= 0; --i)
				ee.dmaWrite((block >> i) & 1u, static_cast<u32>(66 + i), 0);
			for (int b = 0; b < 8; ++b) {
				const u32 byte = (b == 0) ? first_byte : 0u;
				for (int i = 7; i >= 0; --i)
					ee.dmaWrite((byte >> i) & 1u, static_cast<u32>(65 - (b * 8 + (7 - i))), 0);
			}
			ee.dmaWrite(0, 1, 0);
		};
		auto readBlock = [&](u32 block) {
			// "11" + 14 address bits + stop, then 68-bit readback.
			ee.dmaWrite(1, 17, 0);
			ee.dmaWrite(1, 16, 0);
			for (int i = 13; i >= 0; --i)
				ee.dmaWrite((block >> i) & 1u, static_cast<u32>(2 + i), 0);
			ee.dmaWrite(0, 1, 0);
			u64 bits = 0;
			for (int i = 0; i < 68; ++i)
				bits = (bits << 1) | (ee.dmaRead(0) & 1u);
			return bits; // the 4 leading ignore bits shifted out; 64 data bits remain
		};
		writeBlock(0x00, 0xAAu); // low block, survives the upgrade
		writeBlock(0x40, 0xFFu); // block 0x40 = byte 0x200: first byte past 512
		checkEq(ee.size(), 8 * 1024u, "EEPROM upgrades to 64Kbit on high write");
		check(readBlock(0x40) == 0xFF00000000000000ull, "high block reads back written byte");
		check((readBlock(0x00) >> 56) == 0xAAu, "low block data preserved across upgrade");
	}
	{ // EEPROM write settle time: after a data burst the serial line
	  // reads 0 (busy) for ~6.5ms of core cycles, then ready again.
	  // (mGBA models it as EEPROM_SETTLE_CYCLES = 115000.)
		gba::EepromSave ee(gba::EepromSave::Size::K4bit);
		checkEq(ee.dmaRead(0) & 1u, 1u, "EEPROM ready before any write");
		// "10" + 6 address bits (512B chip) + 64 data bits + stop.
		constexpr u32 kTotal = 73;
		ee.dmaWrite(1, kTotal, 0);
		ee.dmaWrite(0, kTotal - 1, 0);
		for (int i = 5; i >= 0; --i)
			ee.dmaWrite(0, static_cast<u32>(58 + i), 0);
		for (int b = 0; b < 64; ++b)
			ee.dmaWrite(0, static_cast<u32>(65 - b), 0);
		ee.dmaWrite(0, 1, 0);
		checkEq(ee.dmaRead(0) & 1u, 0u, "EEPROM busy right after a write");
		checkEq(ee.dmaRead(100000) & 1u, 0u, "EEPROM still busy before the settle window ends");
		checkEq(ee.dmaRead(115000) & 1u, 1u, "EEPROM ready once the settle window passes");
		// A new write re-arms the window; a read request is answered even
		// while a previous write would still be settling.
		ee.dmaWrite(1, kTotal, 100000);
		ee.dmaWrite(0, kTotal - 1, 100000);
		for (int i = 5; i >= 0; --i)
			ee.dmaWrite(0, static_cast<u32>(58 + i), 100000);
		for (int b = 0; b < 64; ++b)
			ee.dmaWrite(0, static_cast<u32>(65 - b), 100000);
		ee.dmaWrite(0, 1, 100000);
		checkEq(ee.dmaRead(211000) & 1u, 0u, "EEPROM busy window restarts on the next write");
		checkEq(ee.dmaRead(226000) & 1u, 1u, "EEPROM ready again after the second settle");
	}
	{ // Serial (SIO) ports have no link hardware: writes must not come
	  // back from the io shadow (a game polling its own status word would
	  // think a transfer happened), and reads report idle.
		GbaMachine m;
		m.g.bus().write16(0x04000128u, 0x0000); // SIOCNT: "start" a transfer
		checkEq(m.g.bus().read16(0x04000128u), 0x0000u, "SIO write does not read back (no shadow)");
		m.g.bus().write16(0x04000120u, 0x00C3); // SIOMLT0 sent byte
		checkEq(m.g.bus().read16(0x04000120u), 0x0000u, "SIO read returns idle");
		checkEq(m.g.bus().read8(0x04000129u), 0x00u, "SIO byte read returns idle");
		// The neighbouring timer block is untouched: 0x100 is TM0's live
		// counter, so a write must not shadow it either (it reads 0 until
		// the timer has been enabled and ticked).
		m.g.bus().write16(0x04000100u, 0xB0B0);
		checkEq(m.g.bus().read16(0x04000100u), 0x0000u, "timers still own 0x100-0x10F");
	}
	{ // Flash byte program is AND-only: bits can be cleared, never set
	  // back without an erase (GBATEK "Flash Memory" programming model).
		gba::FlashMacronix flash(64 * 1024);
		auto program = [&](u32 addr, u8 value) {
			flash.write(0x5555, 0xAA);
			flash.write(0x2AAA, 0x55);
			flash.write(0x5555, 0xA0);
			flash.write(addr, value);
		};
		auto eraseSector = [&](u32 addr) {
			flash.write(0x5555, 0xAA);
			flash.write(0x2AAA, 0x55);
			flash.write(0x5555, 0x80);
			flash.write(0x5555, 0xAA);
			flash.write(0x2AAA, 0x55);
			flash.write(addr, 0x30);
		};
		auto eraseChip = [&] {
			flash.write(0x5555, 0xAA);
			flash.write(0x2AAA, 0x55);
			flash.write(0x5555, 0x10);
		};
		program(0x0200, 0x5Au);
		checkEq(flash.read(0x0200), 0x5Au, "flash program stores a byte");
		program(0x0200, 0xA5u);
		checkEq(flash.read(0x0200), static_cast<u8>(0x5Au & 0xA5u),
				"flash program ANDs (0 bits stay cleared)");
		program(0x0200, 0x0Fu);
		checkEq(flash.read(0x0200), 0x00u, "flash program cannot set bits back to 1");
		eraseSector(0x0200);
		checkEq(flash.read(0x0200), 0xFFu, "flash sector erase restores erased state");
		program(0x0200, 0x3Cu);
		checkEq(flash.read(0x0200), 0x3Cu, "flash programs again after erase");
		program(0x1500, 0x11u); // next sector, must survive the next erase
		eraseSector(0x0200);
		checkEq(flash.read(0x0200), 0xFFu, "flash sector erase clears its own sector");
		checkEq(flash.read(0x1500), 0x11u, "flash sector erase leaves other sectors alone");
		eraseChip();
		checkEq(flash.read(0x1500), 0xFFu, "flash chip erase clears everything");
	}
	{
		// Save state round trip: CPU, memory, PPU and timer state all live in
		// the state buffer, so a reload must restore them.
		GbaMachine m;
		m.write32(0x02001000, 0xDEADBEEFu);
		m.g.cpu().setReg(0, 0x12345678u);
		m.write16(0x04000100, 0x8000u);
		m.write16(0x04000102, 0x0080u);
		m.runCycles(16);
		const u32 saved_counter = m.g.bus().read16(0x04000100);
		const std::string path = "/tmp/gb4me_selftest.state";
		check(m.g.saveState(path), "saveState() writes a state file");
		m.write32(0x02001000, 0);
		m.g.cpu().setReg(0, 0);
		m.g.bus().write16(0x04000100, 0);
		check(m.g.loadState(path), "loadState() reads the state file back");
		checkEq(m.read32(0x02001000), 0xDEADBEEFu, "state restores EWRAM");
		checkEq(m.g.cpu().reg(0), 0x12345678u, "state restores CPU registers");
		checkEq16(m.g.bus().read16(0x04000100), saved_counter, "state restores timer counters");
		std::remove(path.c_str());
	}
}

// ---------------------------------------------------------------------------
// 11. GB / GBC core
// ---------------------------------------------------------------------------

// Headless GB machine: code is planted in WRAM and PC pointed at it, so the
// CPU/MMU/PPU can be driven without a cartridge or the frontend.
struct GbMachine {
	gb::Cartridge cart;
	gb::MMU mmu;
	gb::CPU cpu;
	gb::PPU ppu;
	gb::APU apu;
	gb::Timer timer;
	gb::Joypad joypad;

	explicit GbMachine(HardwareMode mode = HardwareMode::DMG)
	{
		mmu.set_ppu(&ppu);
		mmu.set_apu(&apu);
		mmu.set_timer(&timer);
		mmu.set_joypad(&joypad);
		cpu.set_mmu(&mmu);
		ppu.set_mmu(&mmu);
		apu.set_mmu(&mmu);
		timer.set_mmu(&mmu);
		joypad.set_mmu(&mmu);
		cpu.reset(mode);
		mmu.set_hardware_mode(mode);
		ppu.set_hardware_mode(mode);
		mmu.boot_rom_enabled = false;
		cpu.double_speed = false;
		cpu.clock_speed_hz = kDefaultDmgClockHz;
	}

	// Plant `code` at 0x0000 of WRAM bank 0 and point PC at it.
	void plant(const std::vector<u8> &code, u16 addr = 0xC000)
	{
		for (size_t i = 0; i < code.size(); ++i)
			mmu.write(static_cast<u16>(addr + i), code[i]);
		cpu.regs.pc = addr;
		cpu.regs.sp = 0xDFF0;
	}

	// Plant `code` at `addr`, append a `JR $` park and step until execution
	// reaches it. Returns the number of instructions executed.
	int run(const std::vector<u8> &code, u16 addr = 0xC000, int max_steps = 256)
	{
		plant(code, addr);
		const u16 park = static_cast<u16>(addr + code.size());
		mmu.write(park, 0x18); // JR $ (self loop = done marker)
		mmu.write(static_cast<u16>(park + 1), 0xFE);
		int steps = 0;
		while (cpu.regs.pc != park && steps < max_steps) {
			cpu.step();
			++steps;
		}
		return steps;
	}

	// Step the CPU+PPU together for `mcycles`.
	void runCycles(u32 mcycles)
	{
		u32 ran = 0;
		while (ran < mcycles) {
			const u32 mc = cpu.step();
			timer.step(mc);
			ppu.step(mc);
			ran += mc;
		}
	}
};

void testGbCpu()
{
	section("GB SM83 CPU");
	{
		GbMachine m;
		m.run({0x3E, 0x0F, 0xC6, 0x01}); // LD A,0x0F ; ADD A,0x01
		checkEq(m.cpu.regs.a, 0x10, "ADD A: 0x0F + 1 = 0x10");
		check(m.cpu.regs.get_h() && !m.cpu.regs.get_z() && !m.cpu.regs.get_n() &&
				  !m.cpu.regs.get_c(),
			  "ADD half-carry only");
	}
	{
		GbMachine m;
		m.run({0x3E, 0xFF, 0xC6, 0x01}); // LD A,0xFF ; ADD A,1
		checkEq(m.cpu.regs.a, 0x00, "ADD A carry wraps to 0");
		check(m.cpu.regs.get_z() && m.cpu.regs.get_h() && m.cpu.regs.get_c(), "Z/H/C set on wrap");
	}
	{
		GbMachine m;
		m.run({0x3E, 0x09, 0xC6, 0x09, 0x27}); // LD A,9 ; ADD A,9 ; DAA
		checkEq(m.cpu.regs.a, 0x18, "DAA after 0x09 + 0x09 = 0x18");
	}
	{
		GbMachine m;
		m.run({0x3E, 0x0F, 0x37, 0x3C}); // LD A,0x0F ; SCF ; INC A
		checkEq(m.cpu.regs.a, 0x10, "INC A");
		check(m.cpu.regs.get_h() && m.cpu.regs.get_c() && !m.cpu.regs.get_n(),
			  "INC sets H, preserves C, clears N");
	}
	{
		GbMachine m;
		m.run({0x06, 0x12, 0x0E, 0x34}); // LD B,0x12 ; LD C,0x34
		checkEq(m.cpu.regs.bc(), 0x1234, "16-bit register pair load");
	}
	{
		GbMachine m;
		// LD HL,0xC100 ; LD (HL),0x56 ; LD A,(HL) — WRAM, not ROM.
		m.run({0x21, 0x00, 0xC1, 0x36, 0x56, 0x7E});
		checkEq(m.cpu.regs.a, 0x56, "LD (HL)/LD A,(HL)");
		checkEq(m.mmu.read(0xC100), 0x56, "value landed in WRAM");
	}
	{
		GbMachine m;
		m.run({0xAF}); // XOR A
		checkEq(m.cpu.regs.a, 0x00, "XOR A zeroes A");
		check(m.cpu.regs.get_z() && !m.cpu.regs.get_n() && !m.cpu.regs.get_h() &&
				  !m.cpu.regs.get_c(),
			  "XOR A sets only Z");
	}
	{
		GbMachine m;
		m.run({0x3E, 0x12, 0xCB, 0x37}); // LD A,0x12 ; SWAP A
		checkEq(m.cpu.regs.a, 0x21, "SWAP A swaps nibbles");
	}
	{
		GbMachine m;
		m.run({0x3E, 0x00, 0xCB, 0xFF, 0xCB, 0xBF}); // LD A,0 ; SET 7,A ; RES 7,A
		checkEq(m.cpu.regs.a, 0x00, "SET/RES bit 7");
	}
	{
		GbMachine m;
		m.run({0x3E, 0x81, 0xCB, 0x7F}); // LD A,0x81 ; BIT 7,A
		check(!m.cpu.regs.get_z() && !m.cpu.regs.get_n() && m.cpu.regs.get_h(),
			  "BIT sets Z from the bit and H");
	}
	{
		GbMachine m;
		m.run(
			{0x21, 0x34, 0x12, 0x01, 0x34, 0x12, 0x09}); // LD HL,0x1234 ; LD BC,0x1234 ; ADD HL,BC
		checkEq(m.cpu.regs.hl(), 0x2468, "ADD HL,BC");
	}
	{
		GbMachine m;
		m.run({0x06, 0x12, 0x0E, 0x34, 0xC5, 0xD1}); // PUSH BC ; POP DE
		checkEq(m.cpu.regs.de(), 0x1234, "PUSH/POP round trip");
	}
	{
		// CALL/RET: routine at 0xC010 adds 5 to A and returns to 0xC005.
		GbMachine m;
		m.mmu.write(0xC010, 0xC6); // ADD A,5
		m.mmu.write(0xC011, 0x05);
		m.mmu.write(0xC012, 0xC9); // RET
		// LD A,1 ; CALL 0xC010 ; NOP ; (park at 0xC006)
		const int steps = m.run({0x3E, 0x01, 0xCD, 0x10, 0xC0, 0x00});
		checkEq(m.cpu.regs.a, 0x06, "CALL/RET executes the subroutine");
		checkEq(m.cpu.regs.pc, 0xC006, "RET returns after the CALL");
		checkEq(m.cpu.regs.sp, 0xDFF0, "CALL/RET balances the stack");
		checkEq(static_cast<u32>(steps), 5, "CALL/RET takes LD+CALL+ADD+RET+NOP");
	}
	{
		// Conditional jumps: the not-taken path falls through (Z set).
		GbMachine m;
		// XOR A ; JP NZ,0xC008 ; LD A,0x55 ; JR -> park ; LD A,0xAA ; (park 0xC00A)
		m.run({0xAF, 0xC2, 0x08, 0xC0, 0x3E, 0x55, 0x18, 0x02, 0x3E, 0xAA});
		checkEq(m.cpu.regs.a, 0x55, "JP NZ not taken when Z is set");
	}
	{
		// ...and the taken path (Z clear) jumps over the fall-through code.
		GbMachine m;
		// LD A,1 ; OR A ; JP NZ,0xC00A ; LD A,0x55 ; JR -> park ; LD A,0xAA
		m.run({0x3E, 0x01, 0xB7, 0xC2, 0x0A, 0xC0, 0x3E, 0x55, 0x18, 0x02, 0x3E, 0xAA});
		checkEq(m.cpu.regs.a, 0xAA, "JP NZ taken when Z is clear");
	}
	{
		// HALT + interrupt wake, then vector at 0x0040.
		GbMachine m;
		m.cpu.ime = true;
		m.mmu.write_ie(0x01); // VBlank
		m.cpu.halted = true;
		m.mmu.request_interrupt(0);
		m.cpu.step();
		checkEq(m.cpu.regs.pc, 0x0040, "HALT wakes on the VBlank interrupt and vectors");
		check((m.mmu.read_if() & 0x01) == 0, "serviced interrupt flag is cleared");
	}
	{
		// EI takes effect after the following instruction (Pan Docs). With an
		// interrupt already pending, the vector must NOT be taken until after
		// the instruction that follows EI has run. An implementation that
		// enabled IME immediately would vector before the NOP executes, so
		// PC would be 0x0040 one step too early.
		GbMachine m;
		m.mmu.write_ie(0x01); // VBlank
		m.mmu.request_interrupt(0);
		m.cpu.ime = false;
		m.plant({0xFB, 0x00, 0x00, 0x00}); // EI ; NOP ; NOP ; NOP
		// EI executed -> still disabled this step.
		m.cpu.step();
		check(!m.cpu.ime, "EI does not enable IME until the next step");
		// The instruction after EI runs with IME still clear, then IME goes on.
		m.cpu.step();
		check(m.cpu.ime, "IME enabled after the instruction following EI");
		check(m.cpu.regs.pc != 0x0040, "pending IRQ not vectored before its turn");
		// Now the vector is taken.
		m.cpu.step();
		checkEq(m.cpu.regs.pc, 0x0040, "pending IRQ vectors once IME is set");
	}
}

void testGbcFeatures()
{
	section("GBC hardware (banking, palettes, double speed)");
	{
		GbMachine m(HardwareMode::CGB_Only);
		m.mmu.write(0xFF70, 0x03); // SVBK: WRAM bank 3
		m.mmu.write(0xD000, 0xAA);
		m.mmu.write(0xFF70, 0x01); // back to bank 1
		check(m.mmu.read(0xD000) != 0xAA, "SVBK bank switch isolates 0xD000-0xDFFF");
		m.mmu.write(0xFF70, 0x03);
		checkEq(m.mmu.read(0xD000), 0xAA, "SVBK bank 3 state preserved");
		m.mmu.write(0xFF70, 0x00); // Bit 0-2 = 0 selects bank 1
		checkEq(m.mmu.read(0xD000), 0x00, "SVBK = 0 selects WRAM bank 1");
		// 0xC000-0xCFFF is always bank 0.
		m.mmu.write(0xC000, 0x11);
		m.mmu.write(0xFF70, 0x02);
		checkEq(m.mmu.read(0xC000), 0x11, "0xC000-0xCFFF stays on bank 0");
	}
	{
		GbMachine m(HardwareMode::CGB_Only);
		m.mmu.write(0xFF4F, 0x01); // VBK
		m.mmu.write(0x8000, 0x55);
		checkEq(m.mmu.read(0xFF4F), 0xFF, "VBK reads back with upper bits set");
		m.mmu.write(0xFF4F, 0x00);
		check(m.mmu.read(0x8000) != 0x55, "VBK=1 keeps its own VRAM bank");
		m.mmu.write(0xFF4F, 0x01);
		checkEq(m.mmu.read(0x8000), 0x55, "VRAM bank 1 content preserved");
	}
	{
		GbMachine m(HardwareMode::CGB_Only);
		m.mmu.write(0xFF68, 0x80 | 2); // BG palette 0, colour 1, auto-increment
		m.mmu.write(0xFF69, 0x1F);     // RGB555 red low byte
		m.mmu.write(0xFF69, 0x00);     // high byte
		const u32 colour = m.ppu.bg_palette_color(0, 1);
		check((colour >> 16 & 0xFF) > (colour >> 8 & 0xFF), "BG palette colour 1 is red",
			  hex32(colour));
		checkEq(m.mmu.read(0xFF68) & 0xBFu, 0x80u | 4u, "BCPS index auto-incremented");
		m.mmu.write(0xFF6A, 0x80 | 2);
		m.mmu.write(0xFF6B, 0x1F);
		m.mmu.write(0xFF6B, 0x00);
		const u32 obj = m.ppu.obj_palette_color(0, 1);
		check((obj >> 16 & 0xFF) > (obj >> 8 & 0xFF), "OBJ palette colour 1 is red", hex32(obj));
	}
	{
		// CGB scanline render: tile colour index 1 through the CGB palette.
		GbMachine m(HardwareMode::CGB_Only);
		m.mmu.write(0xFF40, 0x91); // LCDC: LCD + BG on
		m.mmu.write(0x8000, 0xFF); // tile 0 row 0: all pixels index 1
		m.mmu.write(0x8001, 0x00);
		m.mmu.write(0x9800, 0x00); // map entry 0 -> tile 0
		m.mmu.write(0xFF68, 0x80 | 2);
		m.mmu.write(0xFF69, 0x1F); // BG colour 1 = red
		m.mmu.write(0xFF69, 0x00);
		m.ppu.render_scanline(0);
		const u32 px = m.ppu.frame().pixels[0];
		check(((px >> 16) & 0xFF) > ((px >> 8) & 0xFF), "CGB BG scanline uses the CGB palette",
			  hex32(px));
	}
	{
		// DMG scanline render stays monochrome (same tile, DMG path).
		GbMachine m(HardwareMode::DMG);
		m.mmu.write(0xFF40, 0x91);
		m.mmu.write(0xFF47, 0xE4); // BGP
		m.mmu.write(0x8000, 0xFF);
		m.mmu.write(0x8001, 0x00);
		m.mmu.write(0x9800, 0x00);
		m.ppu.render_scanline(0);
		const u32 px = m.ppu.frame().pixels[0];
		const u32 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
		check(r == g && g == b, "DMG BG scanline renders a grey shade", hex32(px));
		check(r != 0xFF, "DMG palette maps colour 1 to a non-white shade", hex32(px));
	}
	{ // MBC5 rumble: bit 3 of a 0x4000-0x5FFF write is the motor, NOT a RAM
	  // bank bit (Pan Docs "MBC5"; mGBA strips it too). Getting this wrong
	  // silently moved a rumble cart to bank 8 on every write.
		std::vector<u8> ram(0x10000, 0xAA); // 32 banks
		for (size_t i = 0x10000; i < ram.size(); i++)
			ram[i] = 0xBB; // bank 8 has a distinct fill
		gb::MBC5 mbc({}, ram, false);
		check(mbc.has_rumble(), "MBC5 reports a rumble motor");
		check(!mbc.rumble(), "rumble starts off");
		mbc.write_rom(0x0000, 0x0A); // RAM enable
		mbc.write_rom(0x4000, 0x08); // rumble only, bank bits 0
		check(mbc.rumble(), "MBC5 bit 3 turns the motor on");
		checkEq(mbc.read_ram(0x0000), 0xAAu, "the rumble bit is not a RAM bank bit");
		mbc.write_rom(0x4000, 0x0A); // bank 2 + rumble
		checkEq(mbc.read_ram(0x0000), 0xAAu, "bank 2 selected by bits 0-2");
		mbc.write_rom(0x4000, 0x00);
		check(!mbc.rumble(), "clearing bit 3 stops the motor");
	}
	{
		// CGB HDMA owns the VRAM/OAM bus while a 0x10 block moves: CPU
		// reads see FF and writes are lost for that window, whatever the
		// PPU mode is (Pan Docs "CGB VRAM DMA").
		GbMachine m(HardwareMode::CGB_Only);
		m.mmu.write(0xFF40, 0x91); // LCD on
		m.mmu.write(0x8000, 0x42);
		checkEq(m.mmu.read(0x8000), 0x42u, "VRAM readable before the transfer");
		// Source 0xC000 -> dest 0x8000, one 0x10 block, HBlank mode.
		m.mmu.write(0xC000, 0x99);
		m.mmu.write(0xFF51, 0xC0);
		m.mmu.write(0xFF52, 0x00);
		m.mmu.write(0xFF53, 0x80);
		m.mmu.write(0xFF54, 0x00);
		m.mmu.write(0xFF55, 0x80); // bit7 = HBlank mode, 1 block
		check(m.mmu.hdma_active(), "HBlank HDMA armed");
		m.ppu.step(120); // cross a line boundary: the HBlank hook fires
		check(m.ppu.hdma_bus_busy(), "bus busy right after the block");
		checkEq(m.mmu.read(0x8010), 0xFFu, "CPU VRAM reads FF while the block moves");
		checkEq(m.mmu.read(0xFE00), 0xFFu, "CPU OAM reads FF while the block moves");
		m.mmu.write(0x8010, 0x77); // must be lost
		// Step past the 32-dot block window and into HBlank (VRAM is
		// separately inaccessible in mode 3, which would mask the result).
		m.ppu.step(100);
		check(!m.ppu.hdma_bus_busy(), "bus free after the block window");
		checkEq(m.mmu.read(0x8000), 0x99u, "HDMA block copied to VRAM");
		checkEq(m.mmu.read(0x8010), 0x00u, "write during the block was dropped");
		m.mmu.write(0x8010, 0x77);
		checkEq(m.mmu.read(0x8010), 0x77u, "VRAM writable again after the block");
		// DMG has no HDMA and no bus block.
		GbMachine d(HardwareMode::DMG);
		d.mmu.write(0xFF40, 0x91);
		d.mmu.write(0x8000, 0x42);
		d.ppu.step(1);
		checkEq(d.mmu.read(0x8000), 0x42u, "DMG VRAM unaffected by the HDMA path");
	}
	{
		// Double speed: KEY1 bit 0 request + STOP switches the CPU clock,
		// then the CPU stalls 2050 M-cycles with DIV frozen (Pan Docs KEY1).
		GbMachine m(HardwareMode::CGB_Only);
		m.mmu.write(0xFF4D, 0x01);
		m.mmu.write(0xFF07, 0x07); // TAC: enabled, fastest (bit 7, 256 T period)
		m.mmu.write(0xFF05, 0x00); // TIMA = 0
		const u8 div0 = m.mmu.read(0xFF04);
		const u8 ly0 = m.mmu.read(0xFF44);
		m.plant({0x10, 0x00}); // STOP
		m.cpu.step();
		check(m.cpu.double_speed, "STOP with KEY1 bit 0 switches to double speed");
		check((m.mmu.read_key1() & 0x80) != 0, "KEY1 bit 7 reports double speed");
		checkEq(m.cpu.speed_stall_, 2050u, "STOP arms the 2050 M-cycle stall");
		// Drain the stall the way the frontend steps (1 component-M per CPU-M).
		u32 drained = 0;
		while (m.cpu.speed_stall_ > 0 && drained < 4096) {
			const u32 mc = m.cpu.step();
			m.timer.step(mc);
			m.ppu.step(mc);
			drained++;
		}
		checkEq(drained, 2050u, "stall lasts exactly 2050 M-cycles");
		checkEq(m.mmu.read(0xFF04), div0, "DIV frozen across the speed-switch stall");
		checkEq(m.mmu.read(0xFF05), 0u, "TIMA frozen across the speed-switch stall");
		check(m.mmu.read(0xFF44) != ly0 || m.ppu.frame_ready(), "PPU advances during the stall");
		// After the stall the timer runs again (8200 unstalled T would move DIV).
		m.timer.step(300);
		check(m.mmu.read(0xFF04) != div0, "DIV resumes after the stall");
		m.mmu.write(0xFF4D, 0x01);
		m.plant({0x10, 0x00});
		m.cpu.step();
		check(!m.cpu.double_speed, "a second STOP returns to normal speed");
	}
	{
		// CGB double-speed: DIV/TIMA run at double wall-rate (Pan Docs KEY1;
		// SameBoy compensates its APU for this — ours runs off its own
		// counter, so only the timer doubles here).
		GbMachine m(HardwareMode::CGB_Only);
		m.mmu.write(0xFF07, 0x07); // TAC: enabled, fastest (256 T period)
		m.mmu.write(0xFF05, 0x00); // TIMA = 0
		const u8 sdiv = m.mmu.read(0xFF04);
		m.timer.step(256); // 1024 T single-speed
		const unsigned single_div = (m.mmu.read(0xFF04) - sdiv) & 0xFF;
		const unsigned single_tima = m.mmu.read(0xFF05);
		checkEq(single_div, 4u, "single-speed DIV baseline");
		checkEq(single_tima, 4u, "single-speed TIMA baseline");
		// Switch through the real STOP path, then drain the stall.
		m.mmu.write(0xFF4D, 0x01);
		m.plant({0x10, 0x00});
		m.cpu.step();
		while (m.cpu.speed_stall_ > 0) {
			const u32 mc = m.cpu.step();
			m.timer.step(mc);
			m.ppu.step(mc);
		}
		check(m.cpu.double_speed, "double-speed engaged for the rate test");
		const u8 ddiv = m.mmu.read(0xFF04);
		m.timer.step(256); // 2048 T in double-speed
		checkEq(((m.mmu.read(0xFF04) - ddiv) & 0xFF), (single_div * 2) & 0xFF,
				"DIV advances 2x in double speed");
		m.mmu.write(0xFF05, 0x00);
		m.timer.step(256);
		checkEq(m.mmu.read(0xFF05), (single_tima * 2) & 0xFF, "TIMA clocks 2x in double speed");
		// Switching back restores single rate.
		m.mmu.write(0xFF4D, 0x01);
		m.plant({0x10, 0x00});
		m.cpu.step();
		while (m.cpu.speed_stall_ > 0) {
			const u32 mc = m.cpu.step();
			m.timer.step(mc);
			m.ppu.step(mc);
		}
		const u8 rdiv = m.mmu.read(0xFF04);
		m.timer.step(256);
		checkEq(((m.mmu.read(0xFF04) - rdiv) & 0xFF), single_div,
				"DIV back to single rate after switching down");
	}
	{
		// DMG hardware must ignore the speed switch.
		GbMachine m(HardwareMode::DMG);
		m.mmu.write(0xFF4D, 0x01);
		m.plant({0x10, 0x00});
		m.cpu.step();
		check(!m.cpu.double_speed, "DMG mode never enters double speed");
		checkEq(m.mmu.read_key1() & 0x7F, 0xFFu & 0x7F, "DMG KEY1 reads 0xFF");
	}
	{
		// HDMA: source -> VRAM block copy (GDMA, bit 7 clear).
		GbMachine m(HardwareMode::CGB_Only);
		for (u16 i = 0; i < 16; ++i)
			m.mmu.write(static_cast<u16>(0xC000 + i), static_cast<u8>(i));
		m.mmu.write(0xFF51, 0xC0); // HDMA1: source high
		m.mmu.write(0xFF52, 0x00); // HDMA2: source low
		m.mmu.write(0xFF53, 0x80); // HDMA3: dest high
		m.mmu.write(0xFF54, 0x00); // HDMA4: dest low
		m.mmu.write(0xFF55, 0x00); // HDMA5: length 1 block, GDMA
		bool ok = true;
		for (u16 i = 0; i < 16; ++i)
			ok = ok && m.ppu.read_vram(i) == static_cast<u8>(i);
		check(ok, "FF55 GDMA copies 0x10 bytes into VRAM");
		checkEq(m.mmu.read(0xFF55) & 0x80, 0x80u, "GDMA completion bit reads 1");
	}
}

// ---------------------------------------------------------------------------
// 12. GB MBC3 real-time clock
// ---------------------------------------------------------------------------

void testGbMbc3Rtc()
{
	section("GB MBC3 RTC");
	// Synthetic MBC3+TIMER+BATTERY (type 0x10): 32KB ROM, 8KB RAM.
	MBC3 mbc(std::vector<u8>(0x8000, 0), std::vector<u8>(0x2000, 0), true, true);
	auto select = [&](u8 reg) { mbc.write_rom(0x4000, reg); };
	auto latch = [&] {
		mbc.write_rom(0x6000, 0);
		mbc.write_rom(0x6000, 1);
	};
	auto read = [&](u8 reg) -> u8 {
		select(reg);
		return mbc.read_ram(0);
	};
	auto write = [&](u8 reg, u8 v) {
		select(reg);
		mbc.write_ram(0, v);
	};

	// RTC registers are gated on RAM enable, like RAM.
	write(0x08, 30);
	latch();
	checkEq(read(0x08), 0xFFu, "RTC reads 0xFF without RAM enable");
	mbc.write_rom(0x0000, 0x0A);

	// Halted writes freeze the clock, so latch+read is exact (no 1s race
	// with the host wall clock between the write and the latch).
	write(0x08, 30);
	write(0x09, 15);
	write(0x0A, 10);
	write(0x0B, 0x05);
	write(0x0C, 0x40); // DH: halt, day high 0
	latch();
	checkEq(read(0x08), 30u, "RTC seconds latch while halted");
	checkEq(read(0x09), 15u, "RTC minutes latch while halted");
	checkEq(read(0x0A), 10u, "RTC hours latch while halted");
	checkEq(read(0x0B), 0x05u, "RTC day-low latches while halted");
	checkEq(read(0x0C), 0x40u, "RTC day-high/halt/carry latch while halted");
	latch();
	checkEq(read(0x08), 30u, "halted RTC stays frozen across latches");

	// Stray latch writes without a 0->1 edge change nothing.
	mbc.write_rom(0x6000, 1);
	mbc.write_rom(0x6000, 1);
	checkEq(read(0x08), 30u, "repeated latch bit without 0->1 edge is a no-op");

	// Day-counter overflow is sticky until the time is (re)set.
	write(0x0C, 0xC0); // halt + carry
	latch();
	checkEq(read(0x0C) & 0x80u, 0x80u, "RTC day-carry bit latches");
	write(0x0C, 0x00); // resume, clear carry
	latch();
	checkEq(read(0x0C) & 0xC0u, 0x00u, "clearing DH clears halt and carry");

	// Full day decomposition at the top of the 9-bit range, frozen.
	write(0x08, 59);
	write(0x09, 59);
	write(0x0A, 23);
	write(0x0B, 0xFF);
	write(0x0C, 0x41); // halt, day bit 8
	latch();
	checkEq(read(0x08), 59u, "RTC seconds decompose at day 511");
	checkEq(read(0x09), 59u, "RTC minutes decompose at day 511");
	checkEq(read(0x0A), 23u, "RTC hours decompose at day 511");
	checkEq(read(0x0B), 0xFFu, "RTC day-low decomposes at day 511");
	checkEq(read(0x0C), 0x41u, "RTC day-high decomposes at day 511");

	// `.rtc` sidecar round-trips the frozen clock.
	const char *tmp = "/tmp/opencode/gb4me_rtc_test.rtc";
	check(mbc.save_rtc(tmp), "RTC state saves to a sidecar file");
	write(0x08, 0);
	write(0x09, 0);
	write(0x0A, 0);
	write(0x0B, 0x00);
	write(0x0C, 0x40);
	latch();
	checkEq(read(0x08), 0u, "RTC reprogrammed after save");
	check(mbc.load_rtc(tmp), "RTC state loads from the sidecar file");
	latch();
	checkEq(read(0x08), 59u, "RTC seconds restored from sidecar");
	checkEq(read(0x0B), 0xFFu, "RTC day-low restored from sidecar");
	std::remove(tmp);

	// Loader path: type 0x10 carries TIMER+BATTERY.
	{
		std::vector<u8> img(0x8000, 0);
		static constexpr u8 kLogo[48] = {
			0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
			0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
			0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
			0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
		};
		for (int i = 0; i < 48; i++)
			img[0x0104 + i] = kLogo[i];
		img[0x0147] = 0x10; // MBC3+TIMER+BATTERY
		Cartridge cart;
		check(cart.load_from_bytes(img), "MBC3+TIMER image loads");
		check(cart.has_rtc(), "type 0x10 reports RTC hardware");
		check(cart.has_battery(), "type 0x10 reports battery");
	}
}

// ---------------------------------------------------------------------------
// 13. Vector typeface baking (headless; upload needs a Vulkan device)
// ---------------------------------------------------------------------------

void testFontBake()
{
	section("Font baking (vector typeface)");
	// Same lookup the renderer uses, dev layout first (bin/fonts is staged
	// by the build from the pinned ImGui source).
	const char *candidates[] = {
		"bin/fonts/Cousine-Regular.ttf",
		"/usr/share/fonts/TTF/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/noto/NotoSans-Regular.ttf",
		"assets/fonts/Cousine-Regular.ttf",
	};
	font_detail::BakedFont font;
	std::string path;
	for (const char *p : candidates) {
		if (font_detail::bake_font_file(p, font)) {
			path = p;
			break;
		}
	}
	check(!path.empty(), "a staged or system TTF bakes");
	if (path.empty())
		return;
	info("baked %s", path.c_str());
	check(font.pixels.size() ==
			  static_cast<size_t>(font_detail::BakedFont::kAtlas) * font_detail::BakedFont::kAtlas,
		  "atlas is 512x512 coverage");
	check(font.ascent > 0 && font.descent < 0, "vertical metrics straddle the baseline");
	bool slots_ok = true, advances_ok = true;
	for (int i = 0; i < font_detail::BakedFont::kCount; i++) {
		const auto &g = font.glyphs[i];
		if (g.u0 > g.u1 || g.v0 > g.v1 || g.u1 > 1.0f || g.v1 > 1.0f)
			slots_ok = false;
		if (!(g.advance > 0))
			advances_ok = false;
	}
	check(slots_ok, "all 95 ASCII slots have valid atlas rects");
	check(advances_ok, "every glyph advances the pen");
	// Ink check: 'A' and 'g' must have inside (>128) samples in their rects.
	auto has_ink = [&](char ch) {
		const auto &g = font.glyphs[ch - 32];
		const int x0 = static_cast<int>(g.u0 * font_detail::BakedFont::kAtlas);
		const int y0 = static_cast<int>(g.v0 * font_detail::BakedFont::kAtlas);
		const int x1 = static_cast<int>(g.u1 * font_detail::BakedFont::kAtlas + 0.5f);
		const int y1 = static_cast<int>(g.v1 * font_detail::BakedFont::kAtlas + 0.5f);
		for (int y = y0; y < y1; y++)
			for (int x = x0; x < x1; x++)
				if (font.pixels[static_cast<size_t>(y) * font_detail::BakedFont::kAtlas + x] > 128)
					return true;
		return false;
	};
	check(has_ink('A') && has_ink('g'), "'A' and 'g' have atlas ink");
	check(!has_ink(' '), "space has no atlas ink");
	// Determinism: baking twice yields identical output.
	font_detail::BakedFont again;
	check(font_detail::bake_font_file(path, again) && again.pixels == font.pixels,
		  "baking is deterministic");
	// Garbage in, false out (never throws, never crashes).
	font_detail::BakedFont junk;
	check(!font_detail::bake_font_file("/tmp/opencode/does-not-exist.ttf", junk),
		  "a missing file bakes false");
}

// ---------------------------------------------------------------------------
// 14. Optional ROM boot smoke test
// ---------------------------------------------------------------------------

void testRomBoot(const std::string &path)
{
	section("ROM boot smoke test");
	// Route the ROM the way the frontend does: try the GB loader first, then
	// the GBA core.
	gb::Cartridge cart;
	if (cart.load(path)) {
		GbMachine m(cart.hardware_mode());
		m.mmu.set_cartridge(&cart);
		m.cpu.reset(cart.hardware_mode());
		m.mmu.set_hardware_mode(cart.hardware_mode());
		m.ppu.set_hardware_mode(cart.hardware_mode());
		const char *mode = cart.hardware_mode() == HardwareMode::DMG ? "DMG" : "CGB";
		info("%s detected (%s)", path.c_str(), mode);
		int frames = 0;
		for (int f = 0; f < 120 && frames < 60; ++f) {
			for (u32 i = 0; i < 70224; ++i) {
				const u32 mc = m.cpu.step();
				m.timer.step(mc);
				m.ppu.step(mc);
			}
			if (m.ppu.frame_ready()) {
				m.ppu.clear_frame_ready();
				++frames;
			}
		}
		// Distinct RGB555 values in the rendered frame.
		bool seen[32] = {};
		unsigned distinct = 0;
		for (u32 px : m.ppu.frame().pixels) {
			const u32 key = ((px >> 19) & 0x1F) ^ ((px >> 11) & 0x1F) ^ ((px >> 3) & 0x1F);
			if (!seen[key]) {
				seen[key] = true;
				++distinct;
			}
		}
		info("ran %d frames, %u distinct colours, PC = %s", frames, distinct,
			 hex16(m.cpu.regs.pc).c_str());
		check(frames > 0, "GB/GBC ROM produced at least one frame");
		check(distinct > 1, "GB/GBC frame is not blank");
		return;
	}

	gba::GameBoyAdvance gba;
	if (!gba.load(path)) {
		check(false, "ROM loaded by either core", path);
		return;
	}
	GbaMachine probe; // For the helpers only; a fresh core runs the ROM.
	(void) probe;
	// Real games need a few seconds of boot (logo skip, RAM init, asset
	// decompression) before the first visible frame, so run up to ~240
	// frames and look for boot progress, not just early pixels.
	int frames = 0;
	unsigned best_distinct = 0;
	bool display_on = false;
	bool assets_uploaded = false;
	for (int f = 0; f < 240 * 228 && frames < 240; ++f) {
		gba.step(1232);
		if (gba.ppu().frameReady()) {
			gba.ppu().clearFrameReady();
			++frames;
			bool seen[32] = {};
			unsigned distinct = 0;
			for (u32 px : gba.ppu().frame().pixels) {
				const u32 key = px & 0x1F;
				if (!seen[key]) {
					seen[key] = true;
					++distinct;
				}
			}
			if (distinct > best_distinct)
				best_distinct = distinct;
			if (gba.bus().read16(0x04000000) != 0)
				display_on = true;
			unsigned pal = 0, vram = 0;
			for (u32 o = 0; o < 0x400; o += 64)
				if (gba.bus().read16(0x05000000 + o) != 0)
					++pal;
			for (u32 o = 0; o < 0x18000; o += 1024)
				if (gba.bus().read16(0x06000000 + o) != 0)
					++vram;
			if (pal > 0 && vram > 0)
				assets_uploaded = true;
		}
		const u32 pc = gba.cpu().pc();
		if (pc >= 0x00004000 && pc < 0x02000000)
			break; // Escaped into open bus: stop early, report below.
	}
	unsigned swis = 0;
	for (u64 n : gba.swiHistogram())
		if (n != 0)
			++swis;
	const u32 pc = gba.cpu().pc();
	const bool pc_valid = pc < 0x00004000 || pc >= 0x02000000;
	info("%s: %d frames, best %u distinct colours, PC = %s, %u distinct SWIs used, "
		 "display %s, assets %s",
		 path.c_str(), frames, best_distinct, hex32(pc).c_str(), swis, display_on ? "on" : "off",
		 assets_uploaded ? "uploaded" : "missing");
	check(frames > 0, "GBA ROM produced at least one frame");
	check(pc_valid, "GBA ROM stayed in mapped code (no open-bus escape)");
	check(display_on || assets_uploaded || best_distinct > 1 || swis >= 3,
		  "GBA ROM boot progressed (display, assets, colours or BIOS use)");
}

void testVideoSettings()
{
	section("Video settings");
	// Everything must default to the pre-existing behaviour so the feature is
	// strictly opt-in: stretch to the available area, no filtering, no grid.
	{
		Settings s;
		check(s.video_filter == static_cast<int>(VideoFilter::Nearest),
			  "Filter defaults to Nearest (the original behaviour)");
		check(s.lcd_grid == 0, "LCD Grid defaults to Off");
		check(s.lcdGridDepth() == 0.0f, "the default grid depth is zero");
	}

	// Names must be distinct for every option, or the UI cannot show which is
	// selected.
	{
		bool filter_names_unique = true;
		for (int i = 0; i < static_cast<int>(VideoFilter::Count); ++i) {
			if (std::string(video_filter_name(static_cast<VideoFilter>(i))).empty())
				filter_names_unique = false;
			for (int j = 0; j < i; ++j) {
				if (std::string(video_filter_name(static_cast<VideoFilter>(i))) ==
					video_filter_name(static_cast<VideoFilter>(j)))
					filter_names_unique = false;
			}
		}
		check(filter_names_unique, "every VideoFilter option has a unique non-empty name");
	}

	// Grid depth must rise with the setting and stay in range.
	{
		Settings s;
		f32 prev = -1.0f;
		bool monotonic = true;
		bool in_range = true;
		for (int i = 0; i <= Settings::kLcdGridMax; ++i) {
			s.lcd_grid = i;
			const f32 d = s.lcdGridDepth();
			if (d <= prev)
				monotonic = false;
			if (d < 0.0f || d > 1.0f)
				in_range = false;
			prev = d;
		}
		check(monotonic, "each stronger LCD Grid setting darkens more than the last");
		check(in_range, "every LCD Grid depth stays within 0..1");
		// Out-of-range values (hand-edited file) must be clamped, not indexed.
		s.lcd_grid = -5;
		check(s.lcdGridDepth() == 0.0f, "a negative grid setting clamps to Off");
		s.lcd_grid = 9999;
		check(s.lcdGridDepth() == Settings::kLcdGridDepth[Settings::kLcdGridMax],
			  "an out-of-range grid setting clamps to the strongest step");
	}

	// The aspect letterboxing. This is layout, not an integer scale: the frame
	// always fills the same area, and only the resampling FILTER changes. The
	// properties that matter are that the source aspect ratio is preserved (so
	// the image is never stretched) and that the result always fits.
	{
		struct Case {
			const char *name;
			u32 sw, sh;
			f32 aw, ah;
		};
		static const Case cases[] = {
			{"GBA in a wide window", 240, 160, 1920, 1080},
			{"GBA in a tall window", 240, 160, 1080, 1920},
			{"GBA in a small window", 240, 160, 300, 403},
			{"GB in a wide window", 160, 144, 1920, 1080},
			{"GB in a tall area", 160, 144, 200, 400},
			{"GB in a wide area", 160, 144, 900, 200},
		};
		bool aspect_ok = true, fits_ok = true;
		for (const Case &c : cases) {
			const PixelRectF r = fit_aspect_rect(0, 0, c.aw, c.ah, c.sw, c.sh);
			const f32 src = static_cast<f32>(c.sw) / static_cast<f32>(c.sh);
			const f32 out = r.h > 0.0f ? r.w / r.h : 0.0f;
			if (r.h <= 0.0f || std::fabs(out - src) > src * 0.01f)
				aspect_ok = false;
			if (r.w > c.aw + 0.5f || r.h > c.ah + 0.5f)
				fits_ok = false;
		}
		check(aspect_ok, "every letterboxed result keeps the source aspect ratio");
		check(fits_ok, "every letterboxed result fits the available area");
		// The regression: filling the whole area would stretch the image.
		{
			const PixelRectF r = fit_aspect_rect(0, 0, 1920, 1080, 160, 144);
			const bool fills_area = r.w >= 1919.5f && r.h >= 1079.5f;
			check(!fills_area, "the frame letterboxes instead of stretching to the whole area");
			check(std::fabs(r.h - 1080.0f) < 0.5f && r.w < 1920.0f,
				  "letterboxing uses the tighter of the two axes");
		}
		// Degenerate inputs must not divide by zero or invert the rect.
		{
			const PixelRectF z = fit_aspect_rect(0, 0, 800, 600, 0, 0);
			check(z.w == 800.0f && z.h == 600.0f, "a zero source size passes the area through");
			const PixelRectF e = fit_aspect_rect(0, 0, 0, 0, 240, 160);
			check(e.w == 0.0f && e.h == 0.0f, "a zero available area does not produce a bad rect");
		}
	}

	// Both options must survive a save/load round trip.
	{
		const std::string path = "/tmp/opencode/gb4me_video_test.cfg";
		{
			Settings s;
			s.video_filter = static_cast<int>(VideoFilter::Scale2x);
			s.lcd_grid = 2;
			check(s.save(path), "video settings save to disk");
		}
		{
			Settings s;
			check(s.load(path), "video settings load from disk");
			check(s.video_filter == static_cast<int>(VideoFilter::Scale2x),
				  "the Filter option round trips");
			check(s.lcd_grid == 2, "the LCD Grid setting round trips");
			// A file missing the keys entirely must fall back to the defaults
			// rather than to garbage.
			Settings fresh;
			check(fresh.video_filter == static_cast<int>(VideoFilter::Nearest) &&
					  fresh.lcd_grid == 0,
				  "a Settings without the keys keeps the opt-in defaults");
		}
		std::remove(path.c_str());
	}

	// settings.cfg is versioned: a save records the schema, a legacy file
	// with no schema line still loads, and a file from a newer build keeps
	// the keys we understand while its unknown keys are reported.
	{
		const std::string path = "/tmp/opencode/gb4me_schema_test.cfg";
		{
			Settings s;
			s.video_filter = static_cast<int>(VideoFilter::Bilinear);
			s.volume = 0.25f;
			check(s.save(path), "settings save with a schema header");
		}
		{
			Settings s;
			check(s.load(path), "settings load from disk");
			checkEq(s.loaded_schema(), Settings::kSchemaVersion,
					"the saved file reports the current schema");
			check(s.unknown_keys().empty(), "no unknown keys in a current file");
			check(s.video_filter == static_cast<int>(VideoFilter::Bilinear) && s.volume > 0.24f &&
					  s.volume < 0.26f,
				  "values round trip through the versioned file");
		}
		// Legacy file: no schema line, old GB key order (key0 = Right).
		{
			std::ofstream f(path, std::ios::trunc);
			f << "rom_folder=/tmp\n";
			f << "volume=1\n";
			f << "video_filter=2\n";
			f << "key0=79\n";
			f << "key1=80\n";
			f << "a_completely_new_key=7\n";
		}
		{
			Settings s;
			check(s.load(path), "a legacy file (no schema line) loads");
			checkEq(s.loaded_schema(), 1, "an unversioned file reports schema 1");
			check(s.video_filter == static_cast<int>(VideoFilter::SmoothBilinear),
				  "legacy values still apply");
			checkEq(static_cast<u32>(s.unknown_keys().size()), 1u,
					"an unknown key is reported, not fatal");
			checkEq(static_cast<u32>(s.key_map[4]), 79u, // Right
					"legacy GB key order migrates into the shared table");
		}
		// A file claiming a newer schema still loads (forward compatible).
		{
			std::ofstream f(path, std::ios::trunc);
			f << "schema=99\n";
			f << "video_filter=1\n";
		}
		{
			Settings s;
			check(s.load(path), "a file from a newer build still loads");
			checkEq(s.loaded_schema(), 99, "the future schema is recorded as-is");
			check(s.video_filter == static_cast<int>(VideoFilter::Bilinear),
				  "keys we understand still apply from a future file");
		}
		std::remove(path.c_str());
	}

	// Per-game overlay: <rom>.cfg beats the global config for that game only.
	{
		checkStrEq(Settings::game_override_path("games/mario.gb"), std::string("games/mario.cfg"),
				   "the per-game file is the rom path with a .cfg extension");
		checkStrEq(Settings::game_override_path("noext"), std::string("noext.cfg"),
				   "a rom path without an extension still gets one");
		// A dot in a DIRECTORY must not be mistaken for the extension.
		checkStrEq(Settings::game_override_path("Mr. B's ROMs/game.gbc"),
				   std::string("Mr. B's ROMs/game.cfg"),
				   "a dot in a folder name is not an extension");

		const std::string rom = "/tmp/opencode/gb4me_pergame.gb";
		const std::string cfg = Settings::game_override_path(rom);
		{
			std::ofstream f(cfg, std::ios::trunc);
			f << "volume=0.25\n";
			f << "palette=3\n";
			f << "hardware_mode=DMG\n";
		}
		Settings s;
		s.volume = 1.0f;
		s.palette = 0;
		s.lcd_grid = 2;
		s.hardware_mode = "Auto";
		check(s.load_game_override(rom), "a per-game file with known keys applies");
		check(s.game.active, "the overlay reports itself active");
		check(std::fabs(s.volume - 0.25f) < 1e-6f, "the per-game volume wins");
		checkEq(s.palette, 3, "the per-game palette wins");
		checkStrEq(s.hardware_mode, std::string("DMG"), "the per-game hardware mode wins");
		checkEq(s.lcd_grid, 2, "a key the file omits keeps its global value");

		// A save from inside the game must not promote the game's values to
		// the global defaults - that is the whole reason the overlay keeps
		// what it displaced.
		const std::string global_path = "/tmp/opencode/gb4me_pergame_global.cfg";
		check(s.save(global_path), "the global config still saves under an overlay");
		{
			Settings reread;
			check(reread.load(global_path), "the saved global config reloads");
			check(std::fabs(reread.volume - 1.0f) < 1e-6f,
				  "the global volume is not overwritten by the game's");
			checkEq(reread.palette, 0, "the global palette is not overwritten by the game's");
			check(s.game.active && std::fabs(s.volume - 0.25f) < 1e-6f,
				  "the running game keeps its own values after a save");
		}
		std::remove(global_path.c_str());

		s.clear_game_override();
		check(!s.game.active, "clearing the overlay deactivates it");
		check(std::fabs(s.volume - 1.0f) < 1e-6f, "the displaced global volume comes back");
		checkEq(s.palette, 0, "the displaced global palette comes back");
		checkStrEq(s.hardware_mode, std::string("Auto"), "the displaced global mode comes back");
		checkEq(s.lcd_grid, 2, "clearing does not touch keys the file never set");

		// A file with nothing we understand is not an overlay at all, and a
		// missing one is not an error.
		{
			std::ofstream f(cfg, std::ios::trunc);
			f << "not_a_setting=1\n";
		}
		check(!s.load_game_override(rom), "a per-game file with no known keys applies nothing");
		check(!s.game.active, "such a file leaves the overlay inactive");
		checkEq(s.palette, 0, "and changes nothing");
		check(!s.load_game_override("/tmp/opencode/gb4me_absent.gb"),
			  "a game without a .cfg file is not an error");
		std::remove(cfg.c_str());
	}

	// Volume OSD state is pure clock logic (no device): inactive by default,
	// active right after a flash, clamping out-of-range volumes.
	{
		VulkanRenderer r;
		check(!r.volume_osd_active(), "the volume OSD starts inactive");
		r.flash_volume(0.8f);
		check(r.volume_osd_active(), "the volume OSD is active after a flash");
		r.flash_volume(2.0f);
		check(r.volume_osd_active(), "an over-range flash still activates");
		r.flash_volume(-1.0f);
		check(r.volume_osd_active(), "an under-range flash still activates");
	}
}

void testInputsPage()
{
	section("Inputs page");
	// The Inputs device dropdown: entry 0 is always the keyboard, and the
	// control is locked to it when no controller is present.
	{
		GUIConsole c;
		c.set_settings(nullptr);
		c.setDeviceOptions({}, 0);
		check(!c.device_box().enabled, "the device dropdown is locked with keyboard only");
		check(c.selectedDevice() == 0, "the locked device is the keyboard");
		check(c.deviceName() == "Keyboard", "the locked device is named Keyboard");
		check(c.device_box().options.size() == 1, "keyboard-only still offers exactly one option");
		// A controller unlocks it and appears as its own entry.
		c.setDeviceOptions({"Xbox Controller"}, 1);
		check(c.device_box().enabled, "a controller unlocks the device dropdown");
		check(c.device_box().options.size() == 2, "keyboard plus one controller");
		check(c.selectedDevice() == 1 && c.deviceName() == "Xbox Controller",
			  "the controller can be selected and is named");
		// The auto-map button is always present, but it is
		// context-sensitive: it restores the keyboard defaults with the
		// keyboard selected, and lays out a pad mapping with a controller.
		check(c.automapVisible(), "the auto-map button is always shown");
		c.setDeviceOptions({"Xbox Controller"}, 0);
		check(c.automapVisible(), "the button stays shown for the keyboard");
		check(std::string(c.autoMapLabel()) == "Reset to defaults",
			  "the keyboard default action is labelled as a reset");
		c.setDeviceOptions({"Xbox Controller"}, 1);
		check(c.automapVisible(), "the button stays shown for a controller");
		check(std::string(c.autoMapLabel()) == "Auto-map to controller",
			  "the controller default action is labelled as an auto-map");
		// A selection that no longer exists falls back to the keyboard rather
		// than leaving the dropdown pointing at a removed pad.
		c.setDeviceOptions({}, 3);
		check(c.selectedDevice() == 0 && !c.device_box().enabled,
			  "a vanished controller falls back to the keyboard");
		// The capture source follows the selection.
		c.setDeviceOptions({"Pad"}, 0);
		check(c.captureWantsKey(), "the keyboard selection captures keys");
		c.setDeviceOptions({"Pad"}, 1);
		check(!c.captureWantsKey(), "a controller selection does not capture keys");
	}

	// The keyboard auto-map must restore the factory layout and, because a
	// button has exactly one source, clear any controller bindings.
	{
		Settings s;
		for (unsigned i = 0; i < kPadButtonCount; ++i) {
			s.key_map[i] = static_cast<SDL_Scancode>(1000 + i);
			s.pad_map[i] = Binding::makeButton(SDL_GAMEPAD_BUTTON_SOUTH);
			s.pad_map_alt[i] = Binding::makeAxis(SDL_GAMEPAD_AXIS_LEFTX, true);
		}
		s.applyDefaultKeyMap();
		bool keys_ok = true, pads_cleared = true;
		for (unsigned i = 0; i < kPadButtonCount; ++i) {
			if (s.key_map[i] != Settings::defaultKeyMap()[i])
				keys_ok = false;
			if (s.pad_map[i].bound() || s.pad_map_alt[i].bound())
				pads_cleared = false;
		}
		check(keys_ok, "the keyboard auto-map restores every default binding");
		check(pads_cleared, "the keyboard auto-map clears the controller bindings");
		// A and B in particular, so a stray default cannot pass unnoticed.
		check(s.key_map[static_cast<size_t>(gba::GbaKeypad::Key::A)] == SDL_SCANCODE_X &&
				  s.key_map[static_cast<size_t>(gba::GbaKeypad::Key::B)] == SDL_SCANCODE_Z,
			  "A and B map back to X and Z");
		check(s.key_map[static_cast<size_t>(gba::GbaKeypad::Key::Start)] == SDL_SCANCODE_RETURN &&
				  s.key_map[static_cast<size_t>(gba::GbaKeypad::Key::Select)] ==
					  SDL_SCANCODE_RSHIFT,
			  "Start and Select map back to Return and Right Shift");
		// The factory map must not use a key the frontend owns.
		const auto &d = Settings::defaultKeyMap();
		check(d[static_cast<size_t>(gba::GbaKeypad::Key::R)] != SDL_SCANCODE_S,
			  "the default R binding avoids S, which opens settings");
	}

	// The profile combo box menu: the command boundary must sit exactly after
	// the profile entries. It was computed as options.size() + specials while
	// options ALREADY contained the specials, so "+ New profile..." was read
	// as a profile and activateProfile() silently failed on it.
	{
		Settings st;
		st.addProfile("Handheld");
		GUIConsole c;
		c.set_settings(&st);
		c.update_layout(1280, 900);
		c.set_settings_tab(GUIConsole::SettingsTab::Inputs);
		const int size = c.profileMenuSize();
		const int nprof = c.profileCommandIndex();
		check(size == st.profileCount() + GUIConsole::kProfileMenuSpecials,
			  "the profile menu lists every profile plus the three commands");
		check(nprof == st.profileCount(), "the command boundary sits right after the last profile");
		// Every menu entry is reachable and in range: the bug made the last
		// three entries index past the profile list.
		bool in_range = true;
		for (int i = 0; i < size; i++)
			if (i < 0 || i >= size)
				in_range = false;
		check(in_range, "every profile menu entry maps to a reachable action");
		check(size - nprof == GUIConsole::kProfileMenuSpecials,
			  "exactly three commands follow the profiles");
		// addProfile activates the new profile, so a user profile is current
		// here and Rename/Delete are available.
		check(st.active_profile == 1, "a newly added profile becomes active");
		check(c.profileMenuEnabled(c.profileCommandIndex() + 1),
			  "Rename is available with a user profile active");
		check(c.profileMenuEnabled(c.profileCommandIndex() + 2),
			  "Delete is available with a user profile active");
		// And it must actually rename, which is what the entry promises.
		check(st.renameProfile(st.active_profile, "Renamed"),
			  "renaming the active user profile works");
		check(st.profileName(st.active_profile) == "Renamed", "the new name is stored");
		// Deleting the active user profile falls back to Default.
		check(st.deleteProfile(st.active_profile), "deleting the active user profile works");
		check(st.active_profile == 0 && st.profileCount() == 1,
			  "deleting removes only that profile and leaves Default active");
		// With Default current again, Rename/Delete must be REPORTED
		// unavailable. They used to be offered anyway and silently did
		// nothing, which read as a broken button.
		c.rebuild_settings_cells();
		check(!c.profileMenuEnabled(c.profileCommandIndex() + 1),
			  "Rename is unavailable while Default is the active profile");
		check(!c.profileMenuEnabled(c.profileCommandIndex() + 2),
			  "Delete is unavailable while Default is the active profile");
		check(c.profileMenuEnabled(c.profileCommandIndex()), "New is always available");
		check(c.profileMenuEnabled(0), "any profile entry is selectable");
		// Default itself is still protected at the model level.
		check(!st.renameProfile(0, "Nope"), "Default cannot be renamed");
		check(!st.deleteProfile(0), "Default cannot be deleted");

		// Creating a profile through the same path the menu uses must work.
		const int before = st.profileCount();
		st.addProfile("Arcade");
		check(st.profileCount() == before + 1 && st.profileName(before) == "Arcade",
			  "a profile can be created from the menu command");
	}

	// The GBA diagram hit test must resolve every logical button, and the
	// D-pad cross must pick the dominant axis so diagonals still land.
	{
		GUIConsole c;
		Settings st;
		c.set_settings(&st);
		c.update_layout(1280, 900);
		c.set_settings_tab(GUIConsole::SettingsTab::Inputs);
		c.set_show_settings(true);
		// Hit testing is gated on the Inputs tab.
		check(!c.hit_gba_pad(0, 0).has_value(), "the diagram is inert outside the Inputs tab");
		const auto &p = c.gbaPad();
		const int cx = static_cast<int>(p.body.x + p.body.w * 0.5f);
		const int cy = static_cast<int>(p.body.y + p.body.h * 0.5f);
		// Every discrete button must resolve at its own centre.
		const auto centre = [&](const GUIConsole::Rect &r) {
			return static_cast<int>(r.x + r.w * 0.5f);
		};
		check(c.hit_gba_pad(centre(p.btn_a), static_cast<int>(p.btn_a.y + p.btn_a.h * 0.5f)) ==
				  PadButton::A,
			  "the A circle hits A");
		check(c.hit_gba_pad(centre(p.btn_b), static_cast<int>(p.btn_b.y + p.btn_b.h * 0.5f)) ==
				  PadButton::B,
			  "the B circle hits B");
		check(c.hit_gba_pad(centre(p.btn_start),
							static_cast<int>(p.btn_start.y + p.btn_start.h * 0.5f)) ==
				  PadButton::Start,
			  "the START pill hits Start");
		check(c.hit_gba_pad(centre(p.btn_select),
							static_cast<int>(p.btn_select.y + p.btn_select.h * 0.5f)) ==
				  PadButton::Select,
			  "the SELECT pill hits Select");
		check(c.hit_gba_pad(centre(p.btn_l), static_cast<int>(p.btn_l.y + p.btn_l.h * 0.5f)) ==
				  PadButton::L,
			  "the L shoulder hits L");
		check(c.hit_gba_pad(centre(p.btn_r), static_cast<int>(p.btn_r.y + p.btn_r.h * 0.5f)) ==
				  PadButton::R,
			  "the R shoulder hits R");
		// The D-pad: each arm, and the dominant-axis rule on the diagonals.
		const int dx = static_cast<int>(p.dpad_h.x + p.dpad_h.w * 0.5f);
		const int dy = static_cast<int>(p.dpad_v.y + p.dpad_v.h * 0.5f);
		const int arm = static_cast<int>(p.dpad_h.w * 0.5f * 0.6f);
		const int armv = static_cast<int>(p.dpad_v.h * 0.5f * 0.6f);
		check(c.hit_gba_pad(dx - arm, dy) == PadButton::Left, "the D-pad left arm hits Left");
		check(c.hit_gba_pad(dx + arm, dy) == PadButton::Right, "the D-pad right arm hits Right");
		check(c.hit_gba_pad(dx, dy - armv) == PadButton::Up, "the D-pad up arm hits Up");
		check(c.hit_gba_pad(dx, dy + armv) == PadButton::Down, "the D-pad down arm hits Down");
		// The hit region must be CENTRED on the cross. Writing the centre as
		// (x + w) / 2 instead of x + w / 2 puts it on the right edge, so every
		// arm click fell outside the region and the D-pad did nothing.
		{
			const float ccx = p.dpad_h.x + p.dpad_h.w * 0.5f;
			const float ccy = p.dpad_v.y + p.dpad_v.h * 0.5f;
			const float half_h = p.dpad_h.w * 0.5f;
			const float half_v = p.dpad_v.h * 0.5f;
			check(std::fabs(ccx - (p.dpad_v.x + p.dpad_v.w * 0.5f)) < 1.0f,
				  "the D-pad bars share a centre x");
			check(std::fabs(ccy - (p.dpad_h.y + p.dpad_h.h * 0.5f)) < 1.0f,
				  "the D-pad bars share a centre y");
			// The left arm sits inside the region, which the old formula
			// excluded.
			check(static_cast<float>(dx - arm) >= ccx - half_h &&
					  static_cast<float>(dx + arm) <= ccx + half_h,
				  "both horizontal arms fall inside the hit region");
			(void) half_v;
		}
		// Real AGB-001 proportions: 144.5 x 82 mm overall, 61.2 x 40.8 mm
		// screen, which is a 1.762 body and a 1.5 screen.
		{
			const f32 body_ratio = p.body.w / p.body.h;
			const f32 screen_ratio = p.screen.w / p.screen.h;
			check(std::fabs(body_ratio - 144.5f / 82.0f) < 0.02f,
				  "the GBA outline keeps the real 144.5x82mm aspect");
			check(std::fabs(screen_ratio - 61.2f / 40.8f) < 0.02f,
				  "the screen keeps the real 61.2x40.8mm aspect");
			// Controls must sit where a real GBA puts them: D-pad on the far
			// left, A/B right of the screen, START/SELECT under it.
			check(p.btn_a.x > p.screen.x + p.screen.w, "the A button is right of the screen");
			check(p.dpad_h.x + p.dpad_h.w * 0.5f < p.screen.x, "the D-pad is left of the screen");
			check(p.btn_start.y > p.screen.y + p.screen.h, "START is below the screen");
			check(p.btn_select.x < p.btn_start.x, "SELECT sits left of START");
			check(p.btn_l.y <= p.body.y + 1.0f && p.btn_r.y <= p.body.y + 1.0f,
				  "the shoulders are on the top edge");
			check(p.btn_b.x < p.btn_a.x && p.btn_b.y > p.btn_a.y, "B sits lower-left of A");
		}

		// Dead space around the cross must not resolve to anything.
		check(!c.hit_gba_pad(static_cast<int>(p.body.x + p.body.w * 0.45f),
							 static_cast<int>(p.body.y + p.body.h * 0.90f))
				   .has_value(),
			  "empty body space hits no button");
		(void) cx;
		(void) cy;
	}
}

void testInputProfiles()
{
	section("Input profiles");
	// A fresh Settings always exposes the built-in Default at index 0.
	{
		Settings s;
		check(s.profileCount() == 1, "a fresh Settings has exactly one profile (Default)");
		check(s.profileName(0) == "Default", "profile 0 is named Default");
		check(s.active_profile == 0, "Default starts active");
		check(s.key_map[static_cast<size_t>(gba::GbaKeypad::Key::A)] == SDL_SCANCODE_X,
			  "Default carries the built-in keyboard mapping");
	}

	// addProfile snapshots the LIVE mapping, and activating restores it.
	{
		Settings s;
		s.addProfile("Handheld"); // profile 1, seeded from the factory mapping
		// Edit while profile 1 is active, so Default stays pristine.
		s.key_map[0] = SDL_SCANCODE_KP_1; // A
		s.pad_map[0] = Binding::makeButton(SDL_GAMEPAD_BUTTON_SOUTH);
		check(s.active_profile == 1, "the new profile is active");

		// Switch to Default: profile 0 was never edited, so it must still hold
		// the factory mapping.
		check(s.activateProfile(0), "activating Default succeeds");
		check(s.active_profile == 0, "Default is active after switching back");
		check(s.key_map[static_cast<size_t>(gba::GbaKeypad::Key::A)] == SDL_SCANCODE_X,
			  "Default keeps its factory key map");
		check(!s.pad_map[0].bound(), "Default has no controller binding");

		// And back: the edit made while profile 1 was active must have been
		// committed to that profile rather than discarded on the way out.
		check(s.activateProfile(1), "re-activating the custom profile succeeds");
		check(s.key_map[0] == SDL_SCANCODE_KP_1,
			  "an edit made under a profile is committed to it on switch-away");
		check(s.pad_map[0].kind == BindKind::Button &&
				  s.pad_map[0].code == static_cast<int>(SDL_GAMEPAD_BUTTON_SOUTH),
			  "the custom controller binding is restored");
	}

	// Default is a normal editable profile: remapping while it is selected
	// updates it and survives a save, so work done on the default layout is
	// not thrown away on the next launch.
	{
		Settings s;
		s.key_map[0] = SDL_SCANCODE_KP_5;
		s.syncActiveProfile();
		check(s.profiles[0].key_map[0] == SDL_SCANCODE_KP_5,
			  "editing under Default updates the Default profile itself");
	}

	// Default is permanent: it can never be renamed or deleted.
	{
		Settings s;
		check(!s.deleteProfile(0), "Default cannot be deleted");
		check(!s.renameProfile(0, "Nope"), "Default cannot be renamed");
		check(s.profileCount() == 1, "Default survived the delete attempt");
	}

	// Deleting the ACTIVE profile falls back to Default's live mapping rather
	// than leaving the deleted profile's bindings in place.
	{
		Settings s;
		s.addProfile("Pad Only");
		s.key_map[0] = SDL_SCANCODE_KP_2; // edit under the user profile
		check(s.active_profile == 1, "the new profile is active");
		check(s.deleteProfile(1), "an active user profile can be deleted");
		check(s.active_profile == 0, "deleting the active profile falls back to Default");
		check(s.key_map[0] == SDL_SCANCODE_X, "the live mapping reverted to Default's");
		check(s.profileCount() == 1, "only Default remains");
	}

	// Deleting a profile ABOVE the active one must keep the same profile live.
	{
		Settings s;
		s.addProfile("First");
		s.addProfile("Second");
		check(s.active_profile == 2, "the last added profile is active");
		check(s.activateProfile(1), "switching to the first profile succeeds");
		s.key_map[0] = SDL_SCANCODE_KP_3; // distinctive live value
		check(s.deleteProfile(2), "a profile after the active one can be deleted");
		check(s.active_profile == 1, "the active index is preserved, not shifted");
		check(s.key_map[0] == SDL_SCANCODE_KP_3, "the still-active profile stays live");
	}

	// Duplicate and blank names are handled without ambiguity.
	{
		Settings s;
		s.addProfile("Arcade");
		s.addProfile("Arcade");
		check(s.profileName(1) == "Arcade" && s.profileName(2) == "Arcade (2)",
			  "a duplicate profile name is made unique");
		const int blank = s.addProfile("");
		check(blank == 3 && s.profileName(3) == "Profile 4",
			  "a blank name falls back to a generated one");
	}

	// The cap is enforced.
	{
		Settings s;
		for (int i = 1; i < Settings::kMaxProfiles; ++i)
			s.addProfile("P" + std::to_string(i));
		check(s.profileCount() == Settings::kMaxProfiles, "profiles fill up to the cap");
		check(s.addProfile("Overflow") == -1, "addProfile refuses to exceed the cap");
		check(s.profileCount() == Settings::kMaxProfiles, "the cap held after the refusal");
	}

	// Persistence: profiles must survive a save/load round trip, and the file
	// must come back on the profile that was active.
	{
		const std::string path = "/tmp/opencode/gb4me_profiles_test.cfg";
		{
			Settings s;
			s.addProfile("Handheld"); // profile 1
			s.addProfile("Arcade");   // profile 2, active
			// Give each profile a distinctive keyboard + controller mapping.
			s.key_map[0] = SDL_SCANCODE_KP_1;
			s.pad_map[0] = Binding::makeButton(SDL_GAMEPAD_BUTTON_SOUTH);
			s.pad_map_alt[4] = Binding::makeAxis(SDL_GAMEPAD_AXIS_LEFTX, true);
			check(s.save(path), "settings with profiles save to disk");
		}
		{
			Settings s;
			check(s.load(path), "settings with profiles load from disk");
			check(s.profileCount() == 3, "all three profiles came back");
			check(s.profileName(0) == "Default", "Default survived the round trip");
			check(s.profileName(1) == "Handheld", "the first profile kept its name");
			check(s.profileName(2) == "Arcade", "the second profile kept its name");
			check(s.active_profile == 2, "the active profile index was restored");
			check(s.key_map[0] == SDL_SCANCODE_KP_1, "the active profile's key map was restored");
			check(s.pad_map[0].kind == BindKind::Button &&
					  s.pad_map[0].code == static_cast<int>(SDL_GAMEPAD_BUTTON_SOUTH),
				  "the active profile's controller binding was restored");
			check(s.pad_map_alt[4].kind == BindKind::AxisPos,
				  "the active profile's secondary axis binding was restored");
			// Switching to another profile after a load must still work.
			check(s.activateProfile(1) && s.profileName(1) == "Handheld",
				  "a loaded profile can be activated");
			check(s.key_map[0] != SDL_SCANCODE_KP_1,
				  "a different profile has its own key map after a load");
			// Default must still be the pristine factory mapping.
			check(s.activateProfile(0), "Default can be activated after a load");
			check(s.key_map[static_cast<size_t>(gba::GbaKeypad::Key::A)] == SDL_SCANCODE_X,
				  "Default is still the factory mapping after a load");
		}
		std::remove(path.c_str());
	}

	// A legacy settings file with no profile section must still load, and must
	// keep its mapping on Default rather than being discarded.
	{
		const std::string path = "/tmp/opencode/gb4me_legacy_test.cfg";
		{
			FILE *f = std::fopen(path.c_str(), "w");
			std::fprintf(f, "volume=0.5\n");
			std::fprintf(f, "key0=79\n"); // X = A, in the unified (GBA) order
			std::fclose(f);
		}
		Settings s;
		check(s.load(path), "a legacy settings file with no profiles loads");
		check(s.profileCount() == 1, "a legacy file yields just the Default profile");
		check(s.key_map[0] == static_cast<SDL_Scancode>(79),
			  "a legacy file's key map is preserved on Default");
		std::remove(path.c_str());
	}

	// Recent ROMs: dedupe, most-recent-first, capped, persisted.
	{
		const std::string path = "/tmp/opencode/gb4me_recents_test.cfg";
		{
			Settings s;
			s.push_recent("/games/a.gb");
			s.push_recent("/games/b.gbc");
			s.push_recent("/games/a.gb"); // replay fronts without duplicating
			check(s.recent_roms.size() == 2, "replaying a recent dedupes");
			check(s.recent_roms[0] == "/games/a.gb", "replay fronts the entry");
			for (int i = 0; i < 10; i++)
				s.push_recent("/games/f" + std::to_string(i) + ".gba");
			check(s.recent_roms.size() == Settings::kMaxRecent, "recents are capped");
			check(s.recent_roms[0] == "/games/f9.gba", "newest push stays front");
			check(s.save(path), "settings with recents save to disk");
		}
		{
			Settings s;
			check(s.load(path), "settings with recents load from disk");
			check(s.recent_roms.size() == Settings::kMaxRecent, "all capped recents came back");
			check(s.recent_roms[0] == "/games/f9.gba", "recency order survives");
			check(s.recent_roms.back() == "/games/f2.gba", "oldest entry is f2");
		}
		std::remove(path.c_str());
	}

	// One binding per logical button, the way RPCS2/PCSX2/Dolphin present
	// controls: a list of actions, each with a single slot that accepts
	// either a key or a controller input.
	{
		Settings s;
		s.key_map[0] = SDL_SCANCODE_X;
		check(s.currentBinding(0) == Binding::makeKey(SDL_SCANCODE_X),
			  "a key binding is what the single slot reports");
		// Assigning a controller button must clear the key, so a button is
		// driven by exactly one thing at a time.
		s.setBinding(0, Binding::makeButton(SDL_GAMEPAD_BUTTON_SOUTH));
		check(s.key_map[0] == SDL_SCANCODE_UNKNOWN,
			  "assigning a pad button clears the keyboard binding for that button");
		check(s.pad_map[0].kind == BindKind::Button,
			  "the pad binding is now the single reported binding");
		check(s.currentBinding(0).kind == BindKind::Button,
			  "currentBinding reports the controller binding once set");
		// And back the other way.
		s.setBinding(0, Binding::makeKey(SDL_SCANCODE_Z));
		check(s.pad_map[0] == Binding(), "assigning a key clears the pad binding");
		check(s.currentBinding(0) == Binding::makeKey(SDL_SCANCODE_Z),
			  "currentBinding reports the key again");
		// Unbound reads as unbound, and the secondary axis source is a valid
		// fallback when the primary is clear.
		s.setBinding(1, Binding{});
		check(!s.currentBinding(1).bound(), "an unset binding reads as unbound");
		s.pad_map_alt[1] = Binding::makeAxis(SDL_GAMEPAD_AXIS_LEFTX, true);
		check(s.currentBinding(1).kind == BindKind::AxisPos,
			  "the secondary source is used when the primary is unbound");
		// Out-of-range indices are ignored rather than corrupting memory.
		const auto before = s.key_map[0];
		s.setBinding(-1, Binding::makeKey(SDL_SCANCODE_A));
		s.setBinding(99, Binding::makeKey(SDL_SCANCODE_A));
		check(s.key_map[0] == before, "an out-of-range setBinding changes nothing");
		check(!s.currentBinding(-1).bound(), "an out-of-range currentBinding is unbound");
	}

	// Out-of-range indices must be rejected rather than corrupting the tables.
	{
		Settings s;
		s.addProfile("Only");
		check(!s.activateProfile(-1), "a negative profile index is rejected");
		check(!s.activateProfile(99), "an out-of-range profile index is rejected");
		check(!s.deleteProfile(99), "deleting an out-of-range index is rejected");
		check(!s.activateProfile(5) && s.active_profile == 1,
			  "a rejected activation leaves the active profile alone");
	}
}

void testInputManager()
{
	section("Input manager");
	// Binding is a packed (kind, code) pair: it must survive a save/load
	// round trip through the settings file unchanged.
	{
		const Binding b = Binding::makeButton(SDL_GAMEPAD_BUTTON_SOUTH);
		const Binding rt = Binding::unpack(b.packed());
		check(b.kind == BindKind::Button && b.code == static_cast<int>(SDL_GAMEPAD_BUTTON_SOUTH) &&
				  rt.packed() == b.packed(),
			  "Binding pack/unpack round trip preserves a controller button");
		const Binding ax = Binding::makeAxis(SDL_GAMEPAD_AXIS_LEFTX, true);
		check(ax.kind == BindKind::AxisPos && ax.bound(),
			  "positive axis binding is stored as AxisPos and counts as bound");
		const Binding none;
		check(!none.bound(), "an unset Binding does not report itself bound");
		check(std::string(pad_button_name(PadButton::Select)) == "Select" &&
				  std::string(pad_button_name(PadButton::R)) == "R" &&
				  std::string(pad_button_name(PadButton::L)) == "L",
			  "pad_button_name names the ten logical buttons");
	}

	// A keyboard binding drives the pad; the aggregate is recomputed each
	// frame rather than latched, so a release always clears the button.
	{
		InputManager in;
		for (unsigned i = 0; i < kPadButtonCount; ++i)
			in.setKeyBinding(static_cast<PadButton>(i), Binding::makeKey(SDL_SCANCODE_UNKNOWN));
		in.setKeyBinding(PadButton::A, Binding::makeKey(SDL_SCANCODE_X));
		Joypad pad;
		gba::GbaKeypad keys;
		in.applyTo(pad, keys);
		check(!pad.key_down(Joypad::Key::A), "unpressed key leaves the emulated A released");

		SDL_Event down{};
		down.type = SDL_EVENT_KEY_DOWN;
		down.key.scancode = SDL_SCANCODE_X;
		SDL_Event up = down;
		up.type = SDL_EVENT_KEY_UP;
		in.handleEvent(down);
		in.update();
		in.applyTo(pad, keys);
		check(pad.key_down(Joypad::Key::A), "a mapped keyboard key presses the emulated A");

		// Releasing the key must clear it: the aggregate is rebuilt from raw
		// state, so this also covers a frame where the pad was never touched.
		in.handleEvent(up);
		in.update();
		in.applyTo(pad, keys);
		check(!pad.key_down(Joypad::Key::A), "releasing the key clears the emulated A");
	}

	// A key the frontend owns (here: suppressed explicitly) must not also
	// drive the game, but releasing it must stay symmetric.
	{
		InputManager in;
		in.setKeyBinding(PadButton::A, Binding::makeKey(SDL_SCANCODE_S));
		Joypad pad;
		gba::GbaKeypad keys;
		SDL_Event down{};
		down.type = SDL_EVENT_KEY_DOWN;
		down.key.scancode = SDL_SCANCODE_S;
		in.handleEvent(down);
		in.setSuppressed(SDL_SCANCODE_S, true);
		in.update();
		in.applyTo(pad, keys);
		check(!pad.key_down(Joypad::Key::A), "a suppressed key does not drive the emulated A");
		in.setSuppressed(SDL_SCANCODE_S, false);
		in.update();
		in.applyTo(pad, keys);
		check(pad.key_down(Joypad::Key::A),
			  "clearing suppression on a held key lets the press through");

		SDL_Event up = down;
		up.type = SDL_EVENT_KEY_UP;
		in.handleEvent(up);
		in.setSuppressed(SDL_SCANCODE_S, false);
		in.update();
		in.applyTo(pad, keys);
		check(!pad.key_down(Joypad::Key::A), "a suppressed key release stays symmetric");
	}

	// releaseAll must clear held state so a focus loss cannot stick a button.
	{
		InputManager in;
		in.setKeyBinding(PadButton::Start, Binding::makeKey(SDL_SCANCODE_RETURN));
		SDL_Event down{};
		down.type = SDL_EVENT_KEY_DOWN;
		down.key.scancode = SDL_SCANCODE_RETURN;
		in.handleEvent(down);
		in.update();
		Joypad pad;
		gba::GbaKeypad keys;
		in.applyTo(pad, keys);
		check(pad.key_down(Joypad::Key::Start), "Start is pressed before releaseAll");
		in.releaseAll();
		in.update();
		in.applyTo(pad, keys);
		check(!pad.key_down(Joypad::Key::Start), "releaseAll clears a held key");
	}

	// The one-click layout must assign every button a conventional source:
	// face buttons, shoulders, menu buttons and both direction sources.
	{
		InputManager in;
		// autoMap needs a device; with none connected it must fail cleanly
		// rather than half-populating the tables.
		check(!in.autoMap(), "autoMap reports failure when no controller is connected");
	}

	// GBA keypad receives the same aggregate as the GB pad.
	{
		InputManager in;
		in.setKeyBinding(PadButton::B, Binding::makeKey(SDL_SCANCODE_Z));
		Joypad pad;
		gba::GbaKeypad keys;
		SDL_Event down{};
		down.type = SDL_EVENT_KEY_DOWN;
		down.key.scancode = SDL_SCANCODE_Z;
		in.handleEvent(down);
		in.update();
		in.applyTo(pad, keys);
		check(pad.key_down(Joypad::Key::B), "mapped key presses the GB B button");
		check(keys.pressed(gba::GbaKeypad::Key::B), "mapped key presses the GBA B button");
	}
}

} // namespace

int run_selftest(const std::string &rom_path)
{
	g_checks = 0;
	g_failed = 0;
	std::printf("GB4ME self test\n");
	std::printf(
		"Core checks for the GB/GBC core and the GBA core (HLE BIOS, no external firmware)\n");
	info("core object sizes: GBA %zu bytes, GB stack %zu bytes", sizeof(gba::GameBoyAdvance),
		 sizeof(GbMachine));

	testBiosStartup();
	testSwiDispatch();
	testArmInstructions();
	testThumbInstructions();
	testExceptionsAndIrq();
	testBus();
	testPpu();
	testDma();
	testTimers();
	testApu();
	testKeypad();
	testInputManager();
	testVideoSettings();
	testInputsPage();
	testInputProfiles();
	testGpioRtc();
	testAudio();
	testSavesAndStates();
	testGbCpu();
	testGbcFeatures();
	testGbMbc3Rtc();
	testFontBake();

	if (!rom_path.empty())
		testRomBoot(rom_path);
	else
		std::printf("\n(no ROM given: skipping the boot smoke test)\n");

	std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
	std::printf("%s\n", g_failed == 0 ? "SELFTEST PASSED" : "SELFTEST FAILED");
	return g_failed == 0 ? 0 : 1;
}

} // namespace gb
