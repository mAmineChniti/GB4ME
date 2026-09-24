// ARM (32-bit) decoder + executor.
// https://problemkaputt.de/gbatek.htm#arm32bitopcodesarmcode
// Classes by bits 27-26: 00 = data processing/misc, 01 = single transfer,
// 10 = block transfer, 11 = branch/coprocessor/SWI. The 011xxxxx space is
// Undefined on ARMv4T (PLD/BKPT are ARMv5 and trap here).
// Cycle model: S=N=I=1 until Phase 3 waitstates (GBATEK "Instruction Cycle
// Times" counts preserved as S/N/I terms).

#include "gba/bus.h"
#include "gba/cpu.h"

namespace gba
{

namespace
{

// Sign-extend helpers.
u32 signExtend(u32 v, u8 bits)
{
	const u32 m = 1u << (bits - 1);
	return (v ^ m) - m;
}

unsigned popcount16(u32 v)
{
	return static_cast<unsigned>(__builtin_popcount(v & 0xFFFFu));
}

// Multiplier-dependent I cycles for MUL/MLA (by magnitude of Rs).
u32 mulExtraCycles(u32 rs)
{
	if ((rs & 0xFFFFFF00u) == 0 || (rs & 0xFFFFFF00u) == 0xFFFFFF00u)
		return 1;
	if ((rs & 0xFFFF0000u) == 0 || (rs & 0xFFFF0000u) == 0xFFFF0000u)
		return 2;
	if ((rs & 0xFF000000u) == 0 || (rs & 0xFF000000u) == 0xFF000000u)
		return 3;
	return 4;
}

} // namespace

// Barrel shifter operand shared by data processing + address modes.
// `alu_pc12` selects the ALU rule (GBATEK "Using R15 (PC)", mGBA
// _shiftLSL/etc.): with shift-by-register, Rm=R15 reads PC+12; address
// modes (LDR/STR offsets, mGBA ADDR_MODE_2_*) always read PC+8.
static Arm7Tdmi::ShiftResult barrelShift(Arm7Tdmi &cpu, u32 instr, bool imm_form,
										 bool alu_pc12 = false)
{
	const bool carry = cpu.getFlag(kFlagC);
	if (imm_form) {
		const u32 rot = ((instr >> 8) & 0xFu) * 2u;
		const u32 imm = instr & 0xFFu;
		if (rot == 0)
			return {imm, carry}; // No shift (not RRX).
		return {(imm >> rot) | (imm << (32 - rot)), ((imm >> (rot - 1)) & 1) != 0};
	}
	const u8 rm = instr & 0xF;
	const u32 rm_val = cpu.reg(rm);
	const u8 type = (instr >> 5) & 3;
	u32 amount;
	bool is_imm;
	if (((instr >> 4) & 1) == 0) {
		amount = (instr >> 7) & 0x1F;
		is_imm = true;
	} else {
		const u8 rs = (instr >> 8) & 0xF;
		// Register-specified shift reading R15 sees PC+12 ($+12).
		amount = cpu.reg(rs) & 0xFFu;
		is_imm = false;
		if (rm == 15 && alu_pc12) {
			const u32 v = (cpu.lastInstrAddr() + 12);
			switch (type) {
			case 0:
				return cpu.lsl(v, amount, carry);
			case 1:
				return cpu.lsr(v, amount, carry, false);
			case 2:
				return cpu.asr(v, amount, carry, false);
			default:
				return cpu.ror(v, amount, carry, false);
			}
		}
	}
	switch (type) {
	case 0:
		return cpu.lsl(rm_val, amount, carry);
	case 1:
		return cpu.lsr(rm_val, amount, carry, is_imm);
	case 2:
		return cpu.asr(rm_val, amount, carry, is_imm);
	default:
		return cpu.ror(rm_val, amount, carry, is_imm);
	}
}

// Data processing / MRS / MSR / MUL / SWP / halfword group (bits 27-26 = 00).
static u32 executeGroup00(Arm7Tdmi &cpu, u32 instr, u32 instr_addr)
{
	const u8 rn = (instr >> 16) & 0xF;
	const u8 rd = (instr >> 12) & 0xF;
	const bool bit25 = ((instr >> 25) & 1) != 0;

	// BX first: its encoding (0001 0010 ... 0001 Rm) otherwise aliases MSR.
	if ((instr & 0x0FFFFFF0u) == 0x012FFF10u) {
		const u32 rm = cpu.reg(instr & 0xF);
		cpu.setState(((rm & 1) != 0) ? CpuState::Thumb : CpuState::Arm);
		cpu.setPc(rm);
		return 3; // 2S+1N.
	}

	// MRS: 00010R00 1111 xxxx 0000 xxxx.
	if (((instr >> 23) & 0x1F) == 0x02 && ((instr >> 16) & 0x3F) == 0x0F && (instr & 0xFFF) == 0) {
		const bool spsr = ((instr >> 22) & 1) != 0;
		cpu.setReg(rd, cpu.readPsr(spsr));
		return 1;
	}
	// MSR (register): 00010R10 mask.
	if (((instr >> 23) & 0x1F) == 0x02 && ((instr >> 20) & 3) == 0x02 && ((instr >> 25) & 1) == 0) {
		const bool spsr = ((instr >> 22) & 1) != 0;
		cpu.writePsr(spsr, cpu.reg(instr & 0xF), (instr >> 16) & 0xF);
		return 1;
	}
	// MSR (immediate): bits 27-20 = 0011_001R (0x32 CPSR / 0x36 SPSR).
	// Partition per GBATEK "MSR Psr,Imm (I=1)" and mGBA's emitter table
	// (-32/-36 rows, which ignore the field Rd for routing). The old
	// gate required bits 25-23 == 011, which no real encoding has (they
	// are 110), so every MSR-immediate fell through to data processing.
	if ((((instr >> 20) & 0xFFu) == 0x32u) || (((instr >> 20) & 0xFFu) == 0x36u)) {
		const bool spsr = ((instr >> 22) & 1) != 0;
		const auto sh = barrelShift(cpu, instr, true);
		cpu.writePsr(spsr, sh.value, (instr >> 16) & 0xF);
		return 1;
	}
	// Multiply: bits 27-23 = 00000 (MUL/MLA) or 00001 (MULL, bit 22 = U),
	// bit 25 = 0, bits 7-4 = 1001 exactly. The exact nibble also keeps
	// post-indexed LDRSB/LDRSH/STRH (nibbles 1011/1101/1111) decoding
	// correctly; GBATEK's bit-6 quirk note is not honored because no
	// toolchain emits it and honoring it would swallow LDRSB.
	if (((instr >> 23) & 0x1F) <= 1 && !bit25 && ((instr & 0xF0u) == 0x90u)) {
		const u8 rm = instr & 0xF;
		const u8 rs = (instr >> 8) & 0xF;
		const u8 r_n = (instr >> 12) & 0xF;
		const u8 r_d = (instr >> 16) & 0xF;
		const u32 extra = mulExtraCycles(cpu.reg(rs));
		if (((instr >> 23) & 1) == 0) { // MUL / MLA.
			const bool acc = ((instr >> 21) & 1) != 0;
			u32 res = (cpu.reg(rm) * cpu.reg(rs)) & 0xFFFFFFFFu;
			if (acc)
				res += cpu.reg(r_n);
			cpu.setReg(r_d, res);
			if (((instr >> 20) & 1) != 0) { // S: N/Z (C/V unchanged).
				cpu.setFlag(kFlagN, (res >> 31) != 0);
				cpu.setFlag(kFlagZ, res == 0);
			}
			return 1 + extra + (acc ? 1 : 0);
		}
		// Multiply long (U = bit 22, S = bit 21).
		const bool is_signed = ((instr >> 22) & 1) != 0;
		u64 res;
		if (is_signed) {
			const i64 a = static_cast<i32>(cpu.reg(rm));
			const i64 b = static_cast<i32>(cpu.reg(rs));
			res = static_cast<u64>(a * b);
		} else {
			res = static_cast<u64>(cpu.reg(rm)) * cpu.reg(rs);
		}
		if (((instr >> 21) & 1) != 0) { // Accumulate (SMLAL/UMLAL).
			const u64 acc = (static_cast<u64>(cpu.reg(r_d)) << 32) | cpu.reg(r_n);
			res += acc;
		}
		cpu.setReg(r_n, static_cast<u32>(res & 0xFFFFFFFFu));
		cpu.setReg(r_d, static_cast<u32>(res >> 32));
		if (((instr >> 20) & 1) != 0) { // S: N = bit 63, Z = (whole == 0).
			cpu.setFlag(kFlagN, (res >> 63) != 0);
			cpu.setFlag(kFlagZ, res == 0);
		}
		return 1 + extra + 1;
	}
	// SWP/SWPB: 00010B00, bits 11-4 = 0000 1001, bit 22 (B) = 0/1.
	// SWP: B=0 (word swap), SWPB: B=1 (byte swap).
	if ((instr & 0x0FB00FF0u) == 0x01000090u) {
		const bool byte_swap = ((instr >> 22) & 1) != 0;
		const u8 r_n = (instr >> 16) & 0xF;
		const u8 r_d = (instr >> 12) & 0xF;
		const u8 r_m = instr & 0xF;
		const u32 addr = cpu.reg(r_n);
		if (byte_swap) {
			// SWPB: byte swap
			const u8 temp = cpu.loadByte(addr);
			cpu.storeByte(addr, static_cast<u8>(cpu.reg(r_m) & 0xFFu));
			cpu.setReg(r_d, temp);
		} else {
			// SWP: word swap (unlike stores, the load observes GBA
			// unaligned-rotate semantics via loadWord).
			const u32 temp = cpu.loadWord(addr);
			cpu.storeWord(addr, cpu.reg(r_m));
			cpu.setReg(r_d, temp);
		}
		return 2; // S+I; the two accesses accumulate themselves.
	}
	// Halfword/signed group: bit 25 = 0, bit 7 = 1, bit 4 = 1. The bit-25
	// gate keeps data-processing-immediate (whose imm8 can set bits 7+4)
	// out of this group.
	if (!bit25 && (((instr >> 7) & 1) == 1) && (((instr >> 4) & 1) == 1)) {
		const bool load = ((instr >> 20) & 1) != 0;
		const u8 h = (instr >> 5) & 3;
		const bool is_imm = ((instr >> 22) & 1) != 0;
		u32 offset;
		if (is_imm)
			offset = (((instr >> 8) & 0xF) << 4) | (instr & 0xF);
		else
			offset = cpu.reg(instr & 0xF);
		const bool pre = ((instr >> 24) & 1) != 0;
		const bool up = ((instr >> 23) & 1) != 0;
		const bool wb = ((instr >> 21) & 1) != 0;
		u32 base = cpu.reg(rn);
		u32 addr = base;
		if (pre)
			addr = up ? (base + offset) : (base - offset);
		u32 cycles = 1; // I-cycle; the bus access accumulates itself.
		if (load) {
			u32 val = 0;
			if (h == 1)
				val = cpu.loadHalf(addr, false);
			else if (h == 2)
				val = static_cast<u32>(static_cast<i32>(static_cast<i8>(cpu.loadByte(addr))));
			else if (h == 3) {
				// LDRSH: signed halfword load
				// Special case: unaligned (odd address) loads byte and sign-extends
				if ((addr & 1) != 0) {
					// Unaligned: load byte and sign-extend to 32-bit
					val = static_cast<u32>(static_cast<i32>(static_cast<i8>(cpu.loadByte(addr))));
				} else {
					// Aligned: load halfword and sign-extend to 32-bit
					val = cpu.loadHalf(addr, true);
				}
			}
			else { // H=00 loads are LDRD/STRD (ARMv5): Undefined on v4T.
				cpu.enterException(CpuException::Undefined);
				return 3;
			}
			cpu.setReg(rd, val);
			if (rd == 15) {
				cpu.setPc(val);
				cycles = 3; // + pipeline refill.
			}
		} else {
			if (h != 1) { // Only STRH exists here on v4T.
				cpu.enterException(CpuException::Undefined);
				return 3;
			}
			// Storing R15 supplies PC+12 (the architectural value), not the
			// internal PC+8 that reg(15) holds (GBATEK "Using R15 (PC)").
			const u32 sv = (rd == 15) ? (cpu.reg(15) + 4) : cpu.reg(rd);
			cpu.storeHalf(addr & ~1u, static_cast<u16>(sv & 0xFFFFu));
			cycles = 0;
		}
		// Writeback (ARM ARM A3.2.1): post-indexed and unregister/scaled
		// offset forms are suppressed when a load targets Rn itself, but the
		// pre-indexed+writeback form always writes the computed address.
		if (pre ? wb : (!load || rn != rd)) {
			const u32 new_base = pre ? addr : (up ? (base + offset) : (base - offset));
			cpu.setReg(rn, new_base);
		}
		(void) instr_addr;
		return cycles;
	}
	// Data processing (immediate or register shift).
	const bool imm = ((instr >> 25) & 1) != 0;
	const u8 opcode = (instr >> 21) & 0xF;
	const bool set_flags = ((instr >> 20) & 1) != 0;
	// R15 as Rn or Rm reads PC+12 for shift-by-register (I=0,R=1),
	// PC+8 otherwise (GBATEK "Using R15 (PC)"; mGBA ALU shifter +4 and
	// Rn +WORD_SIZE_ARM under (opcode & 0x02000010) == 0x10).
	const bool reg_shift = !imm && ((instr >> 4) & 1) != 0;
	const auto sh = barrelShift(cpu, instr, imm, true);
	u32 rn_val = cpu.reg(rn);
	if (rn == 15 && reg_shift)
		rn_val += 4;
	const u32 op2 = sh.value;
	u32 res = 0;
	bool carry = cpu.getFlag(kFlagC);
	bool overflow = cpu.getFlag(kFlagV);
	bool write = true;
	bool arithmetic = false;
	switch (opcode) {
	case 0x0:
		res = rn_val & op2;
		carry = sh.carry;
		break; // AND
	case 0x1:
		res = rn_val ^ op2;
		carry = sh.carry;
		break;  // EOR
	case 0x2: { // SUB
		const u64 r = static_cast<u64>(rn_val) - op2;
		res = static_cast<u32>(r);
		carry = rn_val >= op2;
		overflow = (((rn_val ^ op2) & (rn_val ^ res)) >> 31) != 0;
		arithmetic = true;
		break;
	}
	case 0x3: { // RSB
		const u64 r = static_cast<u64>(op2) - rn_val;
		res = static_cast<u32>(r);
		carry = op2 >= rn_val;
		overflow = (((op2 ^ rn_val) & (op2 ^ res)) >> 31) != 0;
		arithmetic = true;
		break;
	}
	case 0x4: { // ADD
		const u64 r = static_cast<u64>(rn_val) + op2;
		res = static_cast<u32>(r);
		carry = (r >> 32) != 0;
		overflow = ((~(rn_val ^ op2) & (rn_val ^ res)) >> 31) != 0;
		arithmetic = true;
		break;
	}
	case 0x5: { // ADC
		const u64 r = static_cast<u64>(rn_val) + op2 + (carry ? 1 : 0);
		const bool cin = carry;
		res = static_cast<u32>(r);
		carry = (r >> 32) != 0;
		overflow = ((~(rn_val ^ op2) & (rn_val ^ res)) >> 31) != 0;
		(void) cin;
		arithmetic = true;
		break;
	}
	case 0x6: { // SBC
		const u32 cin = carry ? 0 : 1;
		const u64 r = static_cast<u64>(rn_val) - op2 - cin;
		res = static_cast<u32>(r);
		carry = static_cast<u64>(rn_val) >= static_cast<u64>(op2) + cin;
		overflow = (((rn_val ^ op2) & (rn_val ^ res)) >> 31) != 0;
		arithmetic = true;
		break;
	}
	case 0x7: { // RSC
		const u32 cin = carry ? 0 : 1;
		const u64 r = static_cast<u64>(op2) - rn_val - cin;
		res = static_cast<u32>(r);
		carry = static_cast<u64>(op2) >= static_cast<u64>(rn_val) + cin;
		overflow = (((op2 ^ rn_val) & (op2 ^ res)) >> 31) != 0;
		arithmetic = true;
		break;
	}
	case 0x8:
		res = rn_val & op2;
		carry = sh.carry;
		write = false;
		break; // TST
	case 0x9:
		res = rn_val ^ op2;
		carry = sh.carry;
		write = false;
		break;  // TEQ
	case 0xA: { // CMP
		const u64 r = static_cast<u64>(rn_val) - op2;
		res = static_cast<u32>(r);
		carry = rn_val >= op2;
		overflow = (((rn_val ^ op2) & (rn_val ^ res)) >> 31) != 0;
		arithmetic = true;
		write = false;
		break;
	}
	case 0xB: { // CMN
		const u64 r = static_cast<u64>(rn_val) + op2;
		res = static_cast<u32>(r);
		carry = (r >> 32) != 0;
		overflow = ((~(rn_val ^ op2) & (rn_val ^ res)) >> 31) != 0;
		arithmetic = true;
		write = false;
		break;
	}
	case 0xC:
		res = rn_val | op2;
		carry = sh.carry;
		break; // ORR
	case 0xD:
		res = op2;
		carry = sh.carry;
		break; // MOV
	case 0xE:
		res = rn_val & ~op2;
		carry = sh.carry;
		break; // BIC
	default:
		res = ~op2;
		carry = sh.carry;
		break; // MVN
	}
	(void) arithmetic;
	// Only S=1 updates flags, except TST/TEQ/CMP/CMN (opcodes 8-B) which
	// have no S=0 form (that space is MRS/MSR) and always update. The old
	// `opcode >= 0x8` wrongly updated flags for ORR/MOV/BIC/MVN with S=0.
	const bool update = set_flags || !write;
	if (update) {
		cpu.setFlag(kFlagN, (res >> 31) != 0);
		cpu.setFlag(kFlagZ, res == 0);
		cpu.setFlag(kFlagC, carry);
		cpu.setFlag(kFlagV, overflow);
	}
	if (write) {
		if (rd == 15) {
			if (set_flags) {
				// DP with Rd=15 and S restores SPSR (privileged only).
				cpu.restoreCpsrFromSpsr();
			}
			cpu.setPc(res);
			return 4; // Pipeline refill (2S+1N + margin, S=N=1).
		}
		cpu.setReg(rd, res);
	}
	(void) instr_addr;
	return 1;
}

// Single data transfer (bits 27-26 = 01).
static u32 executeSingle(Arm7Tdmi &cpu, u32 instr, u32 instr_addr)
{
	const bool imm = ((instr >> 25) & 1) == 0;
	// Register-offset form with bit 4 set is not a transfer on ARMv4T
	// (valid register offsets use immediate shifts, bit 4 = 0) -> Undefined.
	if (!imm && ((instr >> 4) & 1) == 1) {
		cpu.enterException(CpuException::Undefined);
		return 3;
	}
	const bool pre = ((instr >> 24) & 1) != 0;
	const bool up = ((instr >> 23) & 1) != 0;
	const bool byte = ((instr >> 22) & 1) != 0;
	const bool wb = ((instr >> 21) & 1) != 0;
	const bool load = ((instr >> 20) & 1) != 0;
	const u8 rn = (instr >> 16) & 0xF;
	const u8 rd = (instr >> 12) & 0xF;
	u32 offset;
	if (imm) {
		offset = instr & 0xFFFu;
	} else {
		offset = barrelShift(cpu, instr, false).value;
	}
	u32 base = cpu.reg(rn);
	u32 addr = base;
	if (pre)
		addr = up ? (base + offset) : (base - offset);
	// Overhead beyond the bus access itself: loads carry the I-cycle,
	// stores issue back-to-back (S=N model: no extra).
	u32 cycles = load ? 1 : 0;
	if (load) {
		u32 val = byte ? cpu.loadByte(addr) : cpu.loadWord(addr);
		if (rd == 15) {
			cpu.setPc(val);
			cycles = 3; // + pipeline refill.
		} else {
			cpu.setReg(rd, val);
		}
	} else {
		u32 val = cpu.reg(rd);
		if (rd == 15)
			val = instr_addr + 12; // STR R15 stores PC+12.
		if (byte)
			cpu.storeByte(addr, static_cast<u8>(val & 0xFFu));
		else
			cpu.storeWord(addr, val);
	}
	if (pre ? wb : (!load || rn != rd)) {
		const u32 new_base = pre ? addr : (up ? (base + offset) : (base - offset));
		cpu.setReg(rn, new_base);
	}
	return cycles;
}

// Block data transfer (bits 27-26 = 10).
static u32 executeBlock(Arm7Tdmi &cpu, u32 instr, u32 instr_addr)
{
	const bool pre = ((instr >> 24) & 1) != 0;
	const bool up = ((instr >> 23) & 1) != 0;
	const bool psr = ((instr >> 22) & 1) != 0; // S bit.
	const bool wb = ((instr >> 21) & 1) != 0;
	const bool load = ((instr >> 20) & 1) != 0;
	const u8 rn = (instr >> 16) & 0xF;
	u32 rlist = instr & 0xFFFFu;
	if (rlist == 0)
		rlist = 0x8000u; // Empty list: R15-only (documented).
	const unsigned n = popcount16(rlist);
	const u32 base = cpu.reg(rn);
	u32 addr;
	if (!pre && up)
		addr = base; // IA
	else if (pre && up)
		addr = base + 4; // IB
	else if (!pre && !up)
		addr = base - n * 4 + 4; // DA
	else
		addr = base - n * 4; // DB
	const u32 orig_base = base;
	// S without PC: user-bank transfer. Resolve accessors up front.
	const bool user_transfer = psr && !(load && (rlist & 0x8000u) != 0);
	auto getReg = [&](u8 i) -> u32 {
		if (user_transfer) {
			if (i < 8)
				return cpu.reg(i);
			if (i < 13) {
				// User r8-r12: temporarily read via mode switch is
				// side-effectful; replicate banking resolution directly.
				const CpuMode m = cpu.mode();
				cpu.switchMode(CpuMode::User);
				const u32 v = cpu.reg(i);
				cpu.switchMode(m);
				return v;
			}
			if (i == 13) {
				const CpuMode m = cpu.mode();
				cpu.switchMode(CpuMode::User);
				const u32 v = cpu.reg(13);
				cpu.switchMode(m);
				return v;
			}
			const CpuMode m = cpu.mode();
			cpu.switchMode(CpuMode::User);
			const u32 v = cpu.reg(14);
			cpu.switchMode(m);
			return v;
		}
		return cpu.reg(i);
	};
	auto setReg = [&](u8 i, u32 v) {
		if (user_transfer) {
			const CpuMode m = cpu.mode();
			cpu.switchMode(CpuMode::User);
			cpu.setReg(i, v);
			cpu.switchMode(m);
			return;
		}
		cpu.setReg(i, v);
	};
	bool wrote_pc = false;
	u32 pc_val = 0;
	bool seq = false; // First transfer N-cycle, rest sequential.
	for (u8 i = 0; i < 15; i++) {
		if ((rlist & (1u << i)) == 0)
			continue;
		if (load) {
			setReg(i, cpu.loadWord(addr, seq));
		} else {
			u32 v = getReg(i);
			if (i == static_cast<u8>(rn))
				v = orig_base; // STM base slot.
			else if (i == 15)
				v = getReg(15) + 4; // PC+12, not the internal PC+8.
			cpu.storeWord(addr, v, seq);
		}
		addr += 4;
		seq = true;
	}
	if ((rlist & 0x8000u) != 0) {
		if (load) {
			pc_val = cpu.loadWord(addr, seq);
			wrote_pc = true;
			addr += 4;
		} else {
			// R15 in the list stores PC+12 unless it is also the base.
			u32 v = (rn == 15) ? orig_base : (getReg(15) + 4);
			cpu.storeWord(addr, v, seq);
			addr += 4;
		}
	}
	if (load && wb && (rlist & (1u << rn)) == 0) {
		const u32 new_base = up ? (orig_base + n * 4) : (orig_base - n * 4);
		cpu.setReg(rn, new_base); // Writeback always hits the current bank.
	} else if (!load && wb) {
		const u32 new_base = up ? (orig_base + n * 4) : (orig_base - n * 4);
		cpu.setReg(rn, new_base);
	}
	if (wrote_pc) {
		if (load && psr) {
			// LDM+S+PC: restore SPSR (may change mode/state).
			cpu.restoreCpsrFromSpsr();
		}
		cpu.setPc(pc_val);
		return 4; // I + pipeline refill; transfers accumulate bus time.
	}
	(void) instr_addr;
	if (load)
		return 2; // I + refill margin; transfers accumulate bus time.
	return 1;     // Writeback/issue overhead.
}

u32 executeArm(Arm7Tdmi &cpu, u32 instr, u32 instr_addr)
{
	const u8 cond = (instr >> 28) & 0xF;
	if (cond == 0xF)
		return 1; // NV: never executes on ARMv4.
	if (!cpu.condition(cond))
		return 1;
	const u8 top = (instr >> 26) & 3; // Bits 27-26.
	if (top == 0)
		return executeGroup00(cpu, instr, instr_addr);
	if (top == 1)
		return executeSingle(cpu, instr, instr_addr);
	if (top == 2) {
		// Bit 25 splits the class: 100x = block transfer, 101x = B / BL.
		if (((instr >> 25) & 1) != 0) {
			const bool link = ((instr >> 24) & 1) != 0;
			const u32 target = instr_addr + 8 + signExtend(instr & 0xFFFFFFu, 24) * 4u;
			if (link)
				cpu.setReg(14, instr_addr + 4);
			cpu.setPc(target);
			return 3; // 2S+1N.
		}
		return executeBlock(cpu, instr, instr_addr);
	}
	// Top == 3 (11xx): SWI (1111) or coprocessor (110x/1110). No
	// coprocessor exists on GBA, so those trap as Undefined.
	if (((instr >> 24) & 0xF) == 0xF) { // SWI.
		// GBA uses the top byte of the 24-bit comment (bits 23-16), so ARM
		// and THUMB handlers share numbering via the byte at [LR-2].
		const u8 num = (instr >> 16) & 0xFF;
		cpu.enterException(CpuException::Swi);
		if (cpu.swi_handler_)
			cpu.swi_handler_(num);
		return 3; // 2S+1N.
	}
	// Coprocessor (MRC/MCR/LDC/STC/CDP) and anything else up here:
	// no coprocessor exists on GBA -> Undefined.
	cpu.enterException(CpuException::Undefined);
	return 3;
}

} // namespace gba
