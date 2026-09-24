// HLE BIOS affine helpers: BgAffineSet / ObjAffineSet.
// Fixed-point matrix math per GBATEK; float formulation matches mGBA's
// observable outputs (truncation toward zero on store).

#include "gba/bios.h"
#include "gba/bus.h"
#include "gba/cpu.h"
#include <cmath>

namespace gba
{

HleBios::Outcome HleBios::affine(u8 num, Arm7Tdmi &cpu, GbaBus &bus)
{
	(void) bus;
	if (num == kBgAffineSet) {
		// r0 = src (20-byte entries), r1 = dst (16-byte entries), r2 = count.
		u32 src = cpu.reg(0);
		u32 dst = cpu.reg(1);
		u32 count = cpu.reg(2);
		const double kPi = 3.14159265358979323846;
		while (count-- != 0) {
			const double ox = static_cast<i32>(cpu.loadWord(src)) / 256.0;
			const double oy = static_cast<i32>(cpu.loadWord(src + 4)) / 256.0;
			const double cx = static_cast<i16>(cpu.loadHalf(src + 8, false));
			const double cy = static_cast<i16>(cpu.loadHalf(src + 10, false));
			const double sx = static_cast<i16>(cpu.loadHalf(src + 12, false)) / 256.0;
			const double sy = static_cast<i16>(cpu.loadHalf(src + 14, false)) / 256.0;
			const double theta =
				(static_cast<i16>(cpu.loadHalf(src + 16, false)) >> 8) / 128.0 * kPi;
			src += 20;
			const double c = std::cos(theta), s = std::sin(theta);
			const double a = c * sx, b = -s * sx;
			const double cc = s * sy, d = c * sy;
			const double rx = ox - (a * cx + b * cy);
			const double ry = oy - (cc * cx + d * cy);
			cpu.storeHalf(dst, static_cast<u16>(static_cast<i32>(a * 256.0)));
			cpu.storeHalf(dst + 2, static_cast<u16>(static_cast<i32>(b * 256.0)));
			cpu.storeHalf(dst + 4, static_cast<u16>(static_cast<i32>(cc * 256.0)));
			cpu.storeHalf(dst + 6, static_cast<u16>(static_cast<i32>(d * 256.0)));
			cpu.storeWord(dst + 8, static_cast<u32>(static_cast<i32>(rx * 256.0)));
			cpu.storeWord(dst + 12, static_cast<u32>(static_cast<i32>(ry * 256.0)));
			dst += 16;
		}
		return {Result::Return, 16};
	}
	if (num == kObjAffineSet) {
		// r0 = src (8-byte entries), r1 = dst, r2 = count, r3 = element stride.
		u32 src = cpu.reg(0);
		u32 dst = cpu.reg(1);
		u32 count = cpu.reg(2);
		const u32 diff = cpu.reg(3);
		const double kPi = 3.14159265358979323846;
		while (count-- != 0) {
			const double sx = static_cast<i16>(cpu.loadHalf(src, false)) / 256.0;
			const double sy = static_cast<i16>(cpu.loadHalf(src + 2, false)) / 256.0;
			const double theta =
				(static_cast<i16>(cpu.loadHalf(src + 4, false)) >> 8) / 128.0 * kPi;
			src += 8;
			const double c = std::cos(theta), s = std::sin(theta);
			cpu.storeHalf(dst + 0, static_cast<u16>(static_cast<i32>(c * sx * 256.0)));
			cpu.storeHalf(dst + 2, static_cast<u16>(static_cast<i32>(-s * sx * 256.0)));
			cpu.storeHalf(dst + 4, static_cast<u16>(static_cast<i32>(s * sy * 256.0)));
			cpu.storeHalf(dst + 6, static_cast<u16>(static_cast<i32>(c * sy * 256.0)));
			dst += diff;
		}
		return {Result::Return, 16};
	}
	return {Result::Return, 4};
}

} // namespace gba
