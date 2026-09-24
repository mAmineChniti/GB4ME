// THUMB (16-bit) decoder + executor.
// https://problemkaputt.de/gbatek.htm#arm16bitopcodesthumbcode
// BLX-immediate and BKPT are ARMv5 and trap as Undefined on this ARMv4T.
// Cycle model matches cpu_arm.cpp (S=N=I=1 until Phase 3 waitstates).

#include "gba/bus.h"
#include "gba/cpu.h"

namespace gba
{

namespace
{

u32 signExtend32(u32 v, u8 bits)
{
	const u32 m = 1u << (bits - 1);
	return (v ^ m) - m;
}

struct Arith {
	u32 res;
	bool carry;
	bool overflow;
};

Arith addFlags(u32 a, u32 b, bool carry_in)
{
	const u64 r = static_cast<u64>(a) + b + (carry_in ? 1 : 0);
	const u32 res = static_cast<u32>(r);
	return {res, (r >> 32) != 0, ((~(a ^ b) & (a ^ res)) >> 31) != 0};
}

Arith subFlags(u32 a, u32 b, bool carry_in)
{
	const u32 borrow = carry_in ? 0 : 1;
	const u64 r = static_cast<u64>(a) - b - borrow;
	const u32 res = static_cast<u32>(r);
	return {res, static_cast<u64>(a) >= static_cast<u64>(b) + borrow,
			(((a ^ b) & (a ^ res)) >> 31) != 0};
}

void setNz(Arm7Tdmi &cpu, u32 res)
{
	cpu.setFlag(kFlagN, (res >> 31) != 0);
	cpu.setFlag(kFlagZ, res == 0);
}

} // namespace

u32 executeThumb(Arm7Tdmi &cpu, u16 instr, u32 instr_addr)
{
	const u8 top = (instr >> 12) & 0xF;
	switch (top) {
	case 0x0:
	case 0x1: {
		// 000xx: shifts (00000-00010) or ADD/SUB (00011).
		// GBATEK THUMB.2: bits[10:9] = 0:ADD-reg, 1:SUB-reg, 2:ADD-imm,
		// 3:SUB-imm — bit 10 is the immediate flag, bit 9 selects ADD/SUB.
		if (((instr >> 11) & 0x1F) == 0x03) {
			const bool sub = ((instr >> 9) & 1) != 0;
			const bool imm = ((instr >> 10) & 1) != 0;
			const u32 op = imm ? ((instr >> 6) & 7) : cpu.reg((instr >> 6) & 7);
			const u32 rn = cpu.reg((instr >> 3) & 7);
			const u8 rd = instr & 7;
			const Arith r = sub ? subFlags(rn, op, true) : addFlags(rn, op, false);
			cpu.setReg(rd, r.res);
			setNz(cpu, r.res);
			cpu.setFlag(kFlagC, r.carry);
			cpu.setFlag(kFlagV, r.overflow);
			return 1;
		}
		const u8 op = (instr >> 11) & 3; // 00 LSL, 01 LSR, 10 ASR.
		const u8 amount = (instr >> 6) & 0x1F;
		const u32 rs = cpu.reg((instr >> 3) & 7);
		const u8 rd = instr & 7;
		Arm7Tdmi::ShiftResult sh;
		if (op == 0)
			sh = cpu.lsl(rs, amount, cpu.getFlag(kFlagC));
		else if (op == 1)
			sh = cpu.lsr(rs, amount, cpu.getFlag(kFlagC), true);
		else
			sh = cpu.asr(rs, amount, cpu.getFlag(kFlagC), true);
		cpu.setReg(rd, sh.value);
		setNz(cpu, sh.value);
		cpu.setFlag(kFlagC, sh.carry);
		return 1;
	}
	case 0x2:
	case 0x3: {
		// 001xx: MOV / CMP / ADD / SUB immediate.
		const u8 op = (instr >> 11) & 3;
		const u8 rd = (instr >> 8) & 7;
		const u32 imm = instr & 0xFFu;
		if (op == 0) {
			cpu.setReg(rd, imm);
			setNz(cpu, imm); // C/V unaffected.
			return 1;
		}
		if (op == 1) {
			const Arith r = subFlags(cpu.reg(rd), imm, true);
			setNz(cpu, r.res);
			cpu.setFlag(kFlagC, r.carry);
			cpu.setFlag(kFlagV, r.overflow);
			return 1;
		}
		if (op == 2) {
			const Arith r = addFlags(cpu.reg(rd), imm, false);
			cpu.setReg(rd, r.res);
			setNz(cpu, r.res);
			cpu.setFlag(kFlagC, r.carry);
			cpu.setFlag(kFlagV, r.overflow);
			return 1;
		}
		const Arith r = subFlags(cpu.reg(rd), imm, true);
		cpu.setReg(rd, r.res);
		setNz(cpu, r.res);
		cpu.setFlag(kFlagC, r.carry);
		cpu.setFlag(kFlagV, r.overflow);
		return 1;
	}
	case 0x4: {
		// 0100xx: ALU (010000), HiReg/BX (010001), literal (01001x).
		const u8 sub = (instr >> 10) & 3;
		if (sub == 0) {
			const u8 op = (instr >> 6) & 0xF;
			const u8 rs = (instr >> 3) & 7;
			const u8 rd = instr & 7;
			const u32 a = cpu.reg(rd);
			const u32 b = cpu.reg(rs);
			switch (op) {
			case 0x0: { // AND
				const u32 r = a & b;
				cpu.setReg(rd, r);
				setNz(cpu, r);
				return 1;
			}
			case 0x1: { // EOR
				const u32 r = a ^ b;
				cpu.setReg(rd, r);
				setNz(cpu, r);
				return 1;
			}
			case 0x2: { // LSL by register
				const auto sh = cpu.lsl(a, b & 0xFFu, cpu.getFlag(kFlagC));
				cpu.setReg(rd, sh.value);
				setNz(cpu, sh.value);
				cpu.setFlag(kFlagC, sh.carry);
				return 1;
			}
			case 0x3: { // LSR by register
				const auto sh = cpu.lsr(a, b & 0xFFu, cpu.getFlag(kFlagC), false);
				cpu.setReg(rd, sh.value);
				setNz(cpu, sh.value);
				cpu.setFlag(kFlagC, sh.carry);
				return 1;
			}
			case 0x4: { // ASR by register
				const auto sh = cpu.asr(a, b & 0xFFu, cpu.getFlag(kFlagC), false);
				cpu.setReg(rd, sh.value);
				setNz(cpu, sh.value);
				cpu.setFlag(kFlagC, sh.carry);
				return 1;
			}
			case 0x5: { // ADC
				const Arith r = addFlags(a, b, cpu.getFlag(kFlagC));
				cpu.setReg(rd, r.res);
				setNz(cpu, r.res);
				cpu.setFlag(kFlagC, r.carry);
				cpu.setFlag(kFlagV, r.overflow);
				return 1;
			}
			case 0x6: { // SBC
				const Arith r = subFlags(a, b, cpu.getFlag(kFlagC));
				cpu.setReg(rd, r.res);
				setNz(cpu, r.res);
				cpu.setFlag(kFlagC, r.carry);
				cpu.setFlag(kFlagV, r.overflow);
				return 1;
			}
			case 0x7: { // ROR by register
				const auto sh = cpu.ror(a, b & 0xFFu, cpu.getFlag(kFlagC), false);
				cpu.setReg(rd, sh.value);
				setNz(cpu, sh.value);
				cpu.setFlag(kFlagC, sh.carry);
				return 1;
			}
			case 0x8: { // TST
				const u32 r = a & b;
				setNz(cpu, r);
				return 1;
			}
			case 0x9: { // NEG (0 - Rs), V updated.
				const Arith r = subFlags(0, b, true);
				cpu.setReg(rd, r.res);
				setNz(cpu, r.res);
				cpu.setFlag(kFlagC, r.carry);
				cpu.setFlag(kFlagV, r.overflow);
				return 1;
			}
			case 0xA: { // CMP
				const Arith r = subFlags(a, b, true);
				setNz(cpu, r.res);
				cpu.setFlag(kFlagC, r.carry);
				cpu.setFlag(kFlagV, r.overflow);
				return 1;
			}
			case 0xB: { // CMN
				const Arith r = addFlags(a, b, false);
				setNz(cpu, r.res);
				cpu.setFlag(kFlagC, r.carry);
				cpu.setFlag(kFlagV, r.overflow);
				return 1;
			}
			case 0xC: { // ORR
				const u32 r = a | b;
				cpu.setReg(rd, r);
				setNz(cpu, r);
				return 1;
			}
			case 0xD: { // MUL (N/Z; C/V preserved).
				const u32 r = (a * b) & 0xFFFFFFFFu;
				cpu.setReg(rd, r);
				setNz(cpu, r);
				return 2; // 1S + mI simplified (S=N=I=1).
			}
			case 0xE: { // BIC
				const u32 r = a & ~b;
				cpu.setReg(rd, r);
				setNz(cpu, r);
				return 1;
			}
			default: { // MVN
				const u32 r = ~b;
				cpu.setReg(rd, r);
				setNz(cpu, r);
				return 1;
			}
			}
		}
		if (sub == 1) {
			// HiReg / BX: 010001 op H1 H2 Rs Rd.
			const u8 op = (instr >> 8) & 3;
			const u8 rd = ((instr >> 7) & 1 ? 8 : 0) | (instr & 7);
			const u8 rs = ((instr >> 6) & 1 ? 8 : 0) | ((instr >> 3) & 7);
			if (op == 3) { // BX (BLX-reg is ARMv5: same encoding is BX).
				const u32 target = cpu.reg(rs);
				cpu.setState(((target & 1) != 0) ? CpuState::Thumb : CpuState::Arm);
				cpu.setPc(target);
				return 3; // 2S+1N.
			}
			if (op == 0) { // ADD (no flags).
				cpu.setReg(rd, cpu.reg(rd) + cpu.reg(rs));
				if (rd == 15)
					return 3;
				return 1;
			}
			if (op == 1) { // CMP (flags, no write).
				const Arith r = subFlags(cpu.reg(rd), cpu.reg(rs), true);
				setNz(cpu, r.res);
				cpu.setFlag(kFlagC, r.carry);
				cpu.setFlag(kFlagV, r.overflow);
				return 1;
			}
			cpu.setReg(rd, cpu.reg(rs)); // MOV (no flags).
			if (rd == 15)
				return 3;
			return 1;
		}
		// Literal load: Rt = [((PC+4) & ~3) + imm8*4].
		const u8 rt = (instr >> 8) & 7;
		const u32 addr = ((instr_addr + 4) & ~3u) + ((instr & 0xFFu) * 4u);
		cpu.setReg(rt, cpu.loadWord(addr));
		return 1; // I-cycle; the access accumulates itself.
	}
	case 0x5:
	case 0x6:
	case 0x7: {
		// Register-offset (0101) and immediate word/byte (011x).
		u32 addr;
		bool load;
		bool byte = false;
		u8 rt;
		if (top == 0x5) {
			addr = cpu.reg((instr >> 6) & 7) + cpu.reg((instr >> 3) & 7);
			rt = instr & 7;
			if (((instr >> 9) & 1) != 0) {
				// THUMB.8 register-offset halfword/signed (GBATEK): Ro is
				// bits[8:6] like above; opcode is bits[11:10] (0 STRH,
				// 1 LDSB, 2 LDRH, 3 LDSH).
				const u8 op8 = (instr >> 10) & 3;
				if (op8 == 0) {
					cpu.storeHalf(addr & ~1u,
							static_cast<u16>(cpu.reg(rt) & 0xFFFFu));
					return 0;
				}
				if (op8 == 1) {
					cpu.setReg(rt,
							static_cast<u32>(static_cast<i32>(
									static_cast<i8>(cpu.loadByte(addr)))));
					return 1;
				}
				if (op8 == 3 && (addr & 1) != 0) {
					// LDSH from an odd address loads a byte and
					// sign-extends it (ARMv4T; mirrors the ARM
					// halfword-group path in cpu_arm.cpp).
					cpu.setReg(rt,
							static_cast<u32>(static_cast<i32>(
									static_cast<i8>(cpu.loadByte(addr)))));
					return 1;
				}
				cpu.setReg(rt, cpu.loadHalf(addr, op8 == 3));
				return 1;
			}
			const bool l = ((instr >> 11) & 1) != 0;
			byte = ((instr >> 10) & 1) != 0;
			load = l;
		} else {
			byte = ((instr >> 12) & 1) != 0;
			load = ((instr >> 11) & 1) != 0;
			const u32 off = (instr >> 6) & 0x1F;
			addr = cpu.reg((instr >> 3) & 7) + (byte ? off : off * 4u);
			rt = instr & 7;
		}
		if (load) {
			cpu.setReg(rt, byte ? cpu.loadByte(addr) : cpu.loadWord(addr));
			return 1;
		}
		const u32 v = cpu.reg(rt);
		if (byte)
			cpu.storeByte(addr, static_cast<u8>(v & 0xFFu));
		else
			cpu.storeWord(addr, v);
		return 0;
	}
	case 0x8: {
		// THUMB.10 load/store halfword (GBATEK): bit 11 is the load flag
		// (0 STRH, 1 LDRH); bits[10:6] are the offset scaled by 2
		// (nn = 0..62, step 2). There are no LDSB/LDSH forms here — bit 10
		// is the top offset bit, not an opcode bit (mGBA decodes
		// ((opcode >> 6) & 0x1F) * 2 the same way).
		const bool load = ((instr >> 11) & 1) != 0;
		const u32 off = ((instr >> 6) & 0x1F) * 2u;
		const u32 addr = cpu.reg((instr >> 3) & 7) + off;
		const u8 rt = instr & 7;
		if (load) {
			cpu.setReg(rt, cpu.loadHalf(addr, false));
			return 1;
		}
		cpu.storeHalf(addr & ~1u, static_cast<u16>(cpu.reg(rt) & 0xFFFFu));
		return 0;
	}
	case 0x9: {
		// SP-relative: 1001 L Rt Word8.
		const bool load = ((instr >> 11) & 1) != 0;
		const u8 rt = (instr >> 8) & 7;
		const u32 addr = cpu.sp() + ((instr & 0xFFu) * 4u);
		if (load) {
			cpu.setReg(rt, cpu.loadWord(addr));
			return 1;
		}
		cpu.storeWord(addr, cpu.reg(rt));
		return 0;
	}
	case 0xA: {
		// ADD PC/SP: Rd = (SP ? SP : ((PC+4) & ~3)) + imm8*4.
		const u8 rd = (instr >> 8) & 7;
		const u32 base = ((instr >> 11) & 1) != 0 ? cpu.sp() : ((instr_addr + 4) & ~3u);
		cpu.setReg(rd, base + ((instr & 0xFFu) * 4u));
		return 1;
	}
	case 0xB: {
		const u8 kind = (instr >> 8) & 0xF;
		if (kind == 0x0) { // ADD SP, +/-imm7*4.
			const u32 off = (instr & 0x7Fu) * 4u;
			if (((instr >> 7) & 1) != 0)
				cpu.setReg(13, cpu.sp() - off);
			else
				cpu.setReg(13, cpu.sp() + off);
			return 1;
		}
		if (kind == 0x4 || kind == 0x5) { // PUSH (1011010R).
			u32 rlist = instr & 0xFFu;
			if ((kind & 1) != 0)
				rlist |= (1u << 14);
			u32 sp = cpu.sp();
			bool seq = false;
			for (int i = 14; i >= 0; i--) {
				if ((rlist & (1u << i)) == 0)
					continue;
				sp -= 4;
				cpu.storeWord(sp, cpu.reg(static_cast<u8>(i)), seq);
				seq = true;
			}
			cpu.setReg(13, sp);
			return 1;
		}
		if (kind == 0xC || kind == 0xD) { // POP (1011110R).
			u32 rlist = instr & 0xFFu;
			const bool pc = (kind & 1) != 0;
			if (pc)
				rlist |= (1u << 15);
			u32 sp = cpu.sp();
			bool seq = false;
			for (u8 i = 0; i < 15; i++) {
				if ((rlist & (1u << i)) == 0)
					continue;
				cpu.setReg(i, cpu.loadWord(sp, seq));
				sp += 4;
				seq = true;
			}
			if (pc) {
				const u32 v = cpu.loadWord(sp, seq);
				sp += 4;
				cpu.setReg(13, sp);
				cpu.setPc(v); // Bit 0 ignored (stays THUMB).
				return 4;
			}
			cpu.setReg(13, sp);
			return 1;
		}
		// Everything else here (incl. BKPT): Undefined on ARMv4T.
		cpu.enterException(CpuException::Undefined);
		return 3;
	}
	case 0xC: {
		// STMIA (11000) / LDMIA (11001).
		const bool load = ((instr >> 11) & 1) != 0;
		const u8 rb = (instr >> 8) & 7;
		const u32 rlist = instr & 0xFFu;
		if (rlist == 0)
			return 1; // UNPREDICTABLE: benign no-op (documented).
		u32 addr = cpu.reg(rb);
		bool seq = false; // First transfer N-cycle, rest sequential.
		if (load) {
			for (u8 i = 0; i < 8; i++) {
				if ((rlist & (1u << i)) == 0)
					continue;
				cpu.setReg(i, cpu.loadWord(addr, seq));
				addr += 4;
				seq = true;
			}
			if ((rlist & (1u << rb)) == 0)
				cpu.setReg(rb, addr);
			return 2;
		}
		for (u8 i = 0; i < 8; i++) {
			if ((rlist & (1u << i)) == 0)
				continue;
			cpu.storeWord(addr, cpu.reg(i), seq);
			addr += 4;
			seq = true;
		}
		cpu.setReg(rb, addr);
		return 1;
	}
	case 0xD: {
		const u8 cond = (instr >> 8) & 0xF;
		if (cond == 0xE) { // 11011110: Undefined (not a branch).
			cpu.enterException(CpuException::Undefined);
			return 3;
		}
		if (cond == 0xF) { // SWI.
			const u8 num = instr & 0xFFu;
			cpu.enterException(CpuException::Swi);
			if (cpu.swi_handler_)
				cpu.swi_handler_(num);
			return 3; // 2S+1N.
		}
		if (cpu.condition(cond)) {
			const u32 target = instr_addr + 4 + signExtend32(instr & 0xFFu, 8) * 2u;
			cpu.setPc(target);
			return 3; // Taken: 2S+1N.
		}
		return 1;
	}
	case 0xE: {
		// B: target = $+4 + sext(imm11)*2.
		const u32 target = instr_addr + 4 + signExtend32(instr & 0x7FFu, 11) * 2u;
		cpu.setPc(target);
		return 3;
	}
	default: { // 0xF: BL / BLX-suffix / undefined.
		const u8 h = (instr >> 11) & 3;
		const u32 nn = instr & 0x7FFu;
		if (h == 2) { // First half: LR = $+4 + (sext(nn) << 12).
			const u32 off = signExtend32(nn, 11) << 12;
			cpu.setReg(14, instr_addr + 4 + off);
			return 1;
		}
		if (h == 3) { // Second half: PC = LR + (nn<<1), LR = $+2|1.
			const u32 lr = cpu.reg(14);
			cpu.setReg(14, (instr_addr + 2) | 1u);
			cpu.setPc(lr + (nn << 1)); // Stays THUMB (BL, not BLX).
			return 3;
		}
		// H=00/01 (incl. ARMv5 BLX suffix): Undefined on ARMv4T.
		cpu.enterException(CpuException::Undefined);
		return 3;
	}
	}
}

} // namespace gba
