// HLE BIOS math routines: Div/DivArm/Sqrt/ArcTan/ArcTan2/MidiKey2Freq.
// Integer algorithms and cycle models follow mGBA bios.c observable
// behavior (polynomial constants, edge cases, stall formulas), re-expressed
// here with overflow-safe intermediates (C++23 wrap semantics preserved by
// truncating exact 64-bit products to 32 bits).

#include "gba/bios.h"
#include "gba/bus.h"
#include "gba/cpu.h"
#include <cmath>
#include <cstdint>

namespace gba
{

namespace
{

// Multiplier stall contribution, matching the CPU's own MUL model.
u32 mulWait(u32 r)
{
	if ((r & 0xFFFFFF00u) == 0 || (r & 0xFFFFFF00u) == 0xFFFFFF00u)
		return 1;
	if ((r & 0xFFFF0000u) == 0 || (r & 0xFFFF0000u) == 0xFFFF0000u)
		return 2;
	if ((r & 0xFF000000u) == 0 || (r & 0xFF000000u) == 0xFF000000u)
		return 3;
	return 4;
}

u32 clz32(u32 v)
{
	return v == 0 ? 32 : static_cast<u32>(__builtin_clz(v));
}

i32 mul32(i32 a, i32 b)
{
	return static_cast<i32>(static_cast<i64>(a) * b);
}

// Wrapping negation / division matching ARM SDIV/RSB overflow behavior
// (INT32_MIN negates/divides to itself; C++ would trap on both).
i32 neg32(i32 v)
{
	return static_cast<i32>(0u - static_cast<u32>(v));
}

i32 div32(i32 a, i32 b)
{
	if (b == -1 && a == INT32_MIN)
		return INT32_MIN;
	return a / b;
}

// Wrapped shift-left (matches ARM LSL bit patterns, no C++ UB).
i32 shl14(i32 v)
{
	return static_cast<i32>(static_cast<u32>(v) << 14);
}

} // namespace

HleBios::Outcome HleBios::math(u8 num, Arm7Tdmi &cpu, GbaBus &bus)
{
	(void) bus;
	switch (num) {
	case kDiv:
	case kDivArm: {
		// Div: num=r0, denom=r1. DivArm: num=r1, denom=r0.
		const i32 number =
			(num == kDiv) ? static_cast<i32>(cpu.reg(0)) : static_cast<i32>(cpu.reg(1));
		const i32 denom =
			(num == kDiv) ? static_cast<i32>(cpu.reg(1)) : static_cast<i32>(cpu.reg(0));
		u32 cycles;
		if (denom == 0) {
			// Real BIOS hangs for |num|>1; HLE returns the
			// sign/remainder contract instead (documented).
			cpu.setReg(0, (number < 0) ? 0xFFFFFFFFu : 1u);
			cpu.setReg(1, static_cast<u32>(number));
			cpu.setReg(3, 1);
			cycles = 16;
		} else if (denom == -1 && number == INT32_MIN) {
			cpu.setReg(0, 0x80000000u);
			cpu.setReg(1, 0);
			cpu.setReg(3, 0x80000000u);
			cycles = 16;
		} else {
			const i32 quot = div32(number, denom);
			const i32 rem = (denom == 0) ? number : number - mul32(quot, denom);
			cpu.setReg(0, static_cast<u32>(quot));
			cpu.setReg(1, static_cast<u32>(rem));
			cpu.setReg(3, static_cast<u32>(quot < 0 ? neg32(quot) : quot));
			u32 loops = clz32(static_cast<u32>(denom)) - clz32(static_cast<u32>(number));
			if (loops < 1)
				loops = 1;
			cycles = 4 + 13 * loops + 7;
		}
		return {Result::Return, cycles};
	}
	case kSqrt: {
		// Integer square root (mGBA-compatible).
		const u32 x = cpu.reg(0);
		u32 cycles;
		u32 result;
		if (x == 0) {
			result = 0;
			cycles = 53;
		} else {
			u32 current = 15;
			u32 upper = x;
			u32 bound = 1;
			while (bound < upper) {
				upper >>= 1;
				bound <<= 1;
				current += 6;
			}
			while (true) {
				current += 6;
				upper = x;
				u32 accum = 0;
				u32 lower = bound;
				while (true) {
					current += 5;
					const u32 old_lower = lower;
					if (lower <= upper >> 1)
						lower <<= 1;
					if (old_lower >= upper >> 1)
						break;
				}
				while (true) {
					current += 8;
					accum <<= 1;
					if (upper >= lower) {
						++accum;
						upper -= lower;
					}
					if (lower == bound)
						break;
					lower >>= 1;
				}
				const u32 old_bound = bound;
				bound += accum;
				bound >>= 1;
				if (bound >= old_bound) {
					bound = old_bound;
					break;
				}
			}
			result = bound;
			cycles = current;
		}
		cpu.setReg(0, result);
		return {Result::Return, cycles};
	}
	case kArcTan:
	case kArcTan2: {
		// Fixed-point polynomial (r0 -> r0, intermediates in r1/r3).
		auto arctan = [](i32 i, i32 &r1, i32 &r3, u32 &cycles) -> i16 {
			// Polynomial approximation for arctan in 1.14 fixed-point.
			// Matches mGBA _ArcTan exactly.
			u32 current = 37;
			current += mulWait(static_cast<u32>(mul32(i, i)));
			i32 a = -((i * i) >> 14);
			current += mulWait(static_cast<u32>(mul32(0xA9, a)));
			i32 b = ((0xA9 * a) >> 14) + 0x390;
			current += mulWait(static_cast<u32>(mul32(b, a)));
			b = ((b * a) >> 14) + 0x91C;
			current += mulWait(static_cast<u32>(mul32(b, a)));
			b = ((b * a) >> 14) + 0xFB6;
			current += mulWait(static_cast<u32>(mul32(b, a)));
			b = ((b * a) >> 14) + 0x16AA;
			current += mulWait(static_cast<u32>(mul32(b, a)));
			b = ((b * a) >> 14) + 0x2081;
			current += mulWait(static_cast<u32>(mul32(b, a)));
			b = ((b * a) >> 14) + 0x3651;
			current += mulWait(static_cast<u32>(mul32(b, a)));
			b = ((b * a) >> 14) + 0xA2F9;
			if (r1) {
				r1 = a;
			}
			if (r3) {
				r3 = b;
			}
			cycles = current;
			return static_cast<i16>((i * b) >> 16);
		};
		if (num == kArcTan) {
			i32 r1 = 0, r3 = 0;
			u32 cycles = 0;
			const i16 res = arctan(static_cast<i32>(cpu.reg(0)), r1, r3, cycles);
			cpu.setReg(0, static_cast<u32>(static_cast<i32>(res)));
			cpu.setReg(1, static_cast<u32>(r1));
			cpu.setReg(3, static_cast<u32>(r3));
			return {Result::Return, cycles};
		}
		const i32 x = static_cast<i32>(cpu.reg(0));
		const i32 y = static_cast<i32>(cpu.reg(1));
		u32 cycles = 11;
		u32 out;
		i32 r1 = 0;
		if (y == 0) {
			out = (x >= 0) ? 0u : 0x8000u;
		} else if (x == 0) {
			out = (y >= 0) ? 0x4000u : 0xC000u;
		} else if (y >= 0) {
			if (x >= 0) {
				if (x >= y) {
					i32 r3 = 0;
					out = static_cast<u16>(arctan(div32(shl14(y), x), r1, r3, cycles));
				} else {
					i32 r3 = 0;
					out = 0x4000u - static_cast<u16>(arctan(div32(shl14(x), y), r1, r3, cycles));
				}
			} else if (neg32(x) >= y) {
				i32 r3 = 0;
				out = static_cast<u16>(arctan(div32(shl14(y), x), r1, r3, cycles)) + 0x8000u;
			} else {
				i32 r3 = 0;
				out = 0x4000u - static_cast<u16>(arctan(div32(shl14(x), y), r1, r3, cycles));
			}
		} else {
			if (x <= 0) {
				if (neg32(x) > neg32(y)) {
					i32 r3 = 0;
					out = static_cast<u16>(arctan(div32(shl14(y), x), r1, r3, cycles)) + 0x8000u;
				} else {
					i32 r3 = 0;
					out = 0xC000u - static_cast<u16>(arctan(div32(shl14(x), y), r1, r3, cycles));
				}
			} else if (x >= neg32(y)) {
				i32 r3 = 0;
				out = static_cast<u16>(arctan(div32(shl14(y), x), r1, r3, cycles)) + 0x10000u;
			} else {
				i32 r3 = 0;
				out = 0xC000u - static_cast<u16>(arctan(div32(shl14(x), y), r1, r3, cycles));
			}
		}
		cpu.setReg(0, out & 0xFFFFu);
		cpu.setReg(1, static_cast<u32>(r1));
		cpu.setReg(3, 0x170);
		return {Result::Return, cycles};
	}
	case kMidiKey2Freq: {
		// freq = waveKey / 2^((180 - coarse - fine/256)/12).
		// Note: [r0+4] is song data in guest RAM, not BIOS bytes, so no
		// BIOS-read protection applies here (the historical dump vector
		// targeted real BIOS bytes, which HLE does not contain).
		const u32 key = bus.read32(cpu.reg(0) + 4);
		const double exp =
			(180.0 - static_cast<double>(cpu.reg(1)) - static_cast<double>(cpu.reg(2)) / 256.0) /
			12.0;
		cpu.setReg(0, static_cast<u32>(static_cast<double>(key) / std::exp2(exp)));
		return {Result::Return, 16};
	}
	default:
		return {Result::Return, 4};
	}
}

} // namespace gba
