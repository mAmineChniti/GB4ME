// HLE BIOS memory routines: CpuSet / CpuFastSet.
// GBATEK SWI 0Bh/0Ch. Bulk transfers run through the CPU bus accessors so
// WAITCNT timing accrues naturally. BIOS-area sources are refused (GBATEK).

#include "gba/bios.h"
#include "gba/bus.h"
#include "gba/cpu.h"

namespace gba
{

HleBios::Outcome HleBios::memory(u8 num, Arm7Tdmi &cpu, GbaBus &bus)
{
	(void) bus; // All transfers go through the CPU accessors (timed).
	const u32 src = cpu.reg(0);
	const u32 dst = cpu.reg(1);
	const u32 ctrl = cpu.reg(2);
	// Refuse BIOS-area sources (GBATEK; mirrors mGBA's guard).
	auto inBios = [](u32 addr) {
		return addr < 0x00004000u;
	};
	if (num == kCpuSet) {
		// GBATEK: r2 = length/mode (bits 0-20 = count, bit 24 = fixed, bit 26 = 32/16-bit)
		const bool word32 = ((ctrl >> 26) & 1) != 0;
		const bool fixed = ((ctrl >> 24) & 1) != 0;
		const u32 count = ctrl & 0x1FFFFFu; // Halfwords (16-bit) or words.
		if (count == 0 || inBios(src))
			return {Result::Return, 8};
		if (word32) {
			u32 s = src & ~3u, d = dst & ~3u;
			if (fixed) {
				const u32 v = cpu.loadWord(s);
				for (u32 i = 0; i < count; i++)
					cpu.storeWord(d + i * 4, v, i != 0);
			} else {
				for (u32 i = 0; i < count; i++) {
					cpu.storeWord(d + i * 4, cpu.loadWord(s + i * 4), i != 0);
				}
			}
		} else {
			u32 s = src & ~1u, d = dst & ~1u;
			if (fixed) {
				const u16 v = static_cast<u16>(cpu.loadHalf(s, false));
				for (u32 i = 0; i < count; i++)
					cpu.storeHalf(d + i * 2, v, i != 0);
			} else {
				for (u32 i = 0; i < count; i++) {
					cpu.storeHalf(d + i * 2, static_cast<u16>(cpu.loadHalf(s + i * 2, false)),
							i != 0);
				}
			}
		}
		// No register side effects (GBATEK: no return value).
		return {Result::Return, 8};
	}
	// CpuFastSet: always 32-bit, wordcount (bytes/4), rounded UP to 8 words on GBA.
	// GBATEK: r2 bits 0-20 = wordcount, bit 24 = fixed source (no bit 26 datasize bit).
	u32 words = ctrl & 0x1FFFFFu;
	if (words == 0 || inBios(src))
		return {Result::Return, 8};
	words = (words + 7u) & ~7u; // Round up to multiple of 8 on GBA.
	const bool fixed = ((ctrl >> 24) & 1) != 0;
	const u32 s = src & ~3u, d = dst & ~3u;
	if (fixed) {
		const u32 v = cpu.loadWord(s);
		for (u32 i = 0; i < words; i++)
			cpu.storeWord(d + i * 4, v, i != 0);
	} else {
		for (u32 i = 0; i < words; i++) {
			cpu.storeWord(d + i * 4, cpu.loadWord(s + i * 4), i != 0);
		}
	}
	return {Result::Return, 8};
}

} // namespace gba
