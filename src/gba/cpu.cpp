// ARM7TDMI core: register banking, CPSR/SPSR, exceptions, shifter, step.
// https://problemkaputt.de/gbatek.htm#armcpuregisterset
// https://problemkaputt.de/gbatek.htm#armcpuexceptions
// Decoders: cpu_arm.cpp (ARM), cpu_thumb.cpp (THUMB).

#include "gba/cpu.h"
#include "gba/bus.h"
#include <cstdio>
#include <cstdlib>

namespace gba
{

// Env-gated MMIO access log (Phase 7 diagnostics; see GB4ME_IRQ_TRACE and
// GB4ME_GBA_DMA_TRACE). Covers I/O registers, the GPIO/RTC window and the
// save window — the regions whose misbehaviour stalls real games.
static void logIoAccess(char op, unsigned width, u32 addr, u32 value, u32 instr_addr)
{
#ifdef GB4ME_RELEASE
	(void) op;
	(void) width;
	(void) addr;
	(void) value;
	(void) instr_addr;
#else
	if ((addr >= 0x04000000u && addr < 0x04000100u) ||
		(addr >= 0x080000C0u && addr < 0x080000D0u) ||
		(addr >= 0x0E000000u && addr < 0x0F000000u)) {
		std::printf("gba-io: %c%u %08X = %08X @ %08X\n", op, width, addr, value,
				instr_addr);
	}
#endif // !GB4ME_RELEASE
}

Arm7Tdmi::Arm7Tdmi(GbaBus *bus) : bus_(bus)
{
	// Default SWI handler preserves exception semantics (entry + MOVS
	// return) with no service performed; HLE BIOS replaces it in Phase 4.
	swi_handler_ = [this](u8) { returnFromSwi(); };
	reset();
}

void Arm7Tdmi::reset()
{
	for (u32 &r : r_)
		r = 0;
	for (u32 &r : r8_usr_)
		r = 0;
	for (u32 &r : r8_fiq_)
		r = 0;
	r13_usr_ = 0;
	r14_usr_ = 0;
	fiq_ = Bank{};
	irq_ = Bank{};
	svc_ = Bank{};
	abt_ = Bank{};
	und_ = Bank{};
	// GBATEK Reset: T=0 (ARM), F=1, I=1, M=Supervisor, PC=0 (BIOS entry).
	cpsr_ = 0x00000000;
	setFlag(kFlagI, true);
	setFlag(kFlagF, true);
	cpsr_ = (cpsr_ & ~0x1Fu) | static_cast<u32>(CpuMode::Supervisor);
	state_ = CpuState::Arm;
	pc_ = 0;
	instr_addr_ = 0;
	fetch_seq_ = false;
	irq_pending_ = false;
	halted_ = false;
	cycles_ = 0;
	trace_head_ = 0;
	trace_frozen_ = false;
#ifndef GB4ME_RELEASE
	io_trace_ = std::getenv("GB4ME_IO_TRACE") != nullptr;
#endif
	syncBiosActive();
}

CpuMode Arm7Tdmi::mode() const
{
	return static_cast<CpuMode>(cpsr_ & 0x1F);
}

Arm7Tdmi::Bank *Arm7Tdmi::bankFor(CpuMode m)
{
	switch (m) {
	case CpuMode::Fiq:
		return &fiq_;
	case CpuMode::Irq:
		return &irq_;
	case CpuMode::Supervisor:
		return &svc_;
	case CpuMode::Abort:
		return &abt_;
	case CpuMode::Undefined:
		return &und_;
	default:
		return nullptr; // User/System share r13_usr_/r14_usr_.
	}
}

u32 &Arm7Tdmi::ref8(u8 i)
{
	if (mode() == CpuMode::Fiq)
		return r8_fiq_[i - 8];
	return r8_usr_[i - 8];
}

const u32 &Arm7Tdmi::ref8(u8 i) const
{
	if (mode() == CpuMode::Fiq)
		return r8_fiq_[i - 8];
	return r8_usr_[i - 8];
}

u32 &Arm7Tdmi::ref13()
{
	Bank *b = bankFor(mode());
	if (b != nullptr)
		return b->r13;
	return r13_usr_;
}

u32 &Arm7Tdmi::ref14()
{
	Bank *b = bankFor(mode());
	if (b != nullptr)
		return b->r14;
	return r14_usr_;
}

// M4-0 values the ARM7TDMI actually implements. Writes of an unimplemented
// mode field are ignored by the CPU (only the mode bits are dropped; the
// other PSR fields still apply), so the bank mapping can never be corrupted.
static bool isImplementedMode(CpuMode m)
{
	switch (m) {
	case CpuMode::User:
	case CpuMode::Fiq:
	case CpuMode::Irq:
	case CpuMode::Supervisor:
	case CpuMode::Abort:
	case CpuMode::Undefined:
	case CpuMode::System:
		return true;
	default:
		return false;
	}
}

void Arm7Tdmi::switchMode(CpuMode m)
{
	if (!isImplementedMode(m))
		return;
	const CpuMode old = mode();
	if (old == m)
		return;
	// r13/r14 live in ref13()/ref14() already banked by `old`, so no
	// copying is needed: each bank simply persists in its own storage.
	// (r8-r12 likewise persist in r8_usr_/r8_fiq_.)
	cpsr_ = (cpsr_ & ~0x1Fu) | static_cast<u32>(m);
}

void Arm7Tdmi::setCpsr(u32 v)
{
	const CpuMode old = mode();
	const auto next = static_cast<CpuMode>(v & 0x1F);
	// Only switch through known modes; illegal mode fields keep old mode
	// (UNPREDICTABLE on hardware — never corrupt the bank mapping).
	if (isImplementedMode(next))
		switchMode(next);
	else
		v = (v & ~0x1Fu) | static_cast<u32>(old);
	// T bit is not writable by CPSR assignment here (only BX/exception
	// entry change state); preserve current T, then track it below.
	const bool t = getFlag(kFlagT);
	cpsr_ = v;
	setFlag(kFlagT, t);
	state_ = getFlag(kFlagT) ? CpuState::Thumb : CpuState::Arm;
	(void) old;
}

u32 Arm7Tdmi::spsr() const
{
	const Bank *b = const_cast<Arm7Tdmi *>(this)->bankFor(mode());
	if (b == nullptr)
		return 0; // User/System: UNPREDICTABLE, reads zero.
	return b->spsr;
}

void Arm7Tdmi::setSpsr(u32 v)
{
	Bank *b = bankFor(mode());
	if (b != nullptr)
		b->spsr = v;
}

void Arm7Tdmi::setFlag(u8 bit, bool v)
{
	if (v)
		cpsr_ |= (1u << bit);
	else
		cpsr_ &= ~(1u << bit);
}

u32 Arm7Tdmi::pcRead() const
{
	// Pipeline PC: ARM ($+8) word-aligned, THUMB ($+4) halfword-aligned.
	if (state_ == CpuState::Arm)
		return (instr_addr_ + 8) & ~3u;
	return (instr_addr_ + 4) & ~1u;
}

u32 Arm7Tdmi::reg(u8 i) const
{
	i &= 0xF;
	if (i < 8)
		return r_[i];
	if (i < 13)
		return ref8(i);
	if (i == 13) {
		const Bank *b = const_cast<Arm7Tdmi *>(this)->bankFor(mode());
		return b != nullptr ? b->r13 : r13_usr_;
	}
	if (i == 14) {
		const Bank *b = const_cast<Arm7Tdmi *>(this)->bankFor(mode());
		return b != nullptr ? b->r14 : r14_usr_;
	}
	return pcRead();
}

void Arm7Tdmi::setReg(u8 i, u32 v)
{
	i &= 0xF;
	if (i < 8) {
		r_[i] = v;
		return;
	}
	if (i < 13) {
		ref8(i) = v;
		return;
	}
	if (i == 13) {
		ref13() = v;
		return;
	}
	if (i == 14) {
		ref14() = v;
		return;
	}
	setPc(v); // r15 write: branch, no state change (only BX changes T).
}

void Arm7Tdmi::setPc(u32 addr)
{
	if (state_ == CpuState::Arm)
		pc_ = addr & ~3u;
	else
		pc_ = addr & ~1u;
	fetch_seq_ = false; // Next fetch refills the pipeline (N-cycle).
	syncBiosActive();
}

void Arm7Tdmi::syncBiosActive()
{
	if (bus_ != nullptr)
		bus_->setBiosActive(pc_ < 0x00004000u);
}

bool Arm7Tdmi::condition(u8 cond) const
{
	const bool n = getFlag(kFlagN);
	const bool z = getFlag(kFlagZ);
	const bool c = getFlag(kFlagC);
	const bool v = getFlag(kFlagV);
	switch (cond & 0xF) {
	case 0x0:
		return z; // EQ
	case 0x1:
		return !z; // NE
	case 0x2:
		return c; // CS/HS
	case 0x3:
		return !c; // CC/LO
	case 0x4:
		return n; // MI
	case 0x5:
		return !n; // PL
	case 0x6:
		return v; // VS
	case 0x7:
		return !v; // VC
	case 0x8:
		return c && !z; // HI
	case 0x9:
		return !c || z; // LS
	case 0xA:
		return n == v; // GE
	case 0xB:
		return n != v; // LT
	case 0xC:
		return !z && (n == v); // GT
	case 0xD:
		return z || (n != v); // LE
	case 0xE:
		return true; // AL
	default:
		return false; // NV (ARMv4): never executes.
	}
}

u32 Arm7Tdmi::readPsr(bool spsr) const
{
	return spsr ? this->spsr() : cpsr_;
}

void Arm7Tdmi::writePsr(bool use_spsr, u32 value, u8 field_mask)
{
	u32 mask = 0;
	// GBATEK: in CPSR the S (23-16) and X (15-8) fields are architecturally
	// reserved / do-not-change, and mGBA's MSR writes only flags + control
	// (src/arm/isa-arm.c:704-718) and likewise excludes S/X from the SPSR
	// mask (:729-735). Writing them let code that probes reserved fields
	// corrupt the mode/flag byte.
	const bool privileged = mode() != CpuMode::User;
	const bool mask_sx = use_spsr ? true : privileged && false;
	if (field_mask & 0x8)
		mask |= 0xFF000000u; // f: flags 31-24.
	if (mask_sx && (field_mask & 0x4))
		mask |= 0x00FF0000u; // s: status 23-16 (SPSR only).
	if (mask_sx && (field_mask & 0x2))
		mask |= 0x0000FF00u; // x: extension 15-8 (SPSR only).
	if (field_mask & 0x1)
		mask |= 0x000000FFu; // c: control 7-0.
	if (!privileged) {
		if (use_spsr)
			return;          // No SPSR in User mode.
		mask &= 0xFF000000u; // User mode: flags only.
	}
	mask &= ~(1u << kFlagT); // T is never MSR-writable.
	if (use_spsr) {
		setSpsr((spsr() & ~mask) | (value & mask));
		return;
	}
	const u32 cur_mode = cpsr_ & 0x1F;
	u32 next = (cpsr_ & ~mask) | (value & mask);
	// Unimplemented mode fields are ignored (ARM7TDMI); the rest of the
	// written fields still take effect.
	if (!isImplementedMode(static_cast<CpuMode>(next & 0x1F)))
		next = (next & ~0x1Fu) | cur_mode;
	if ((next & 0x1F) != cur_mode) {
		// Mode change via control field: route through bank switching.
		const u32 keep = next & ~0x1Fu;
		switchMode(static_cast<CpuMode>(next & 0x1F));
		cpsr_ = (cpsr_ & 0x1F) | keep;
	} else {
		cpsr_ = next;
	}
	state_ = getFlag(kFlagT) ? CpuState::Thumb : CpuState::Arm;
}

// ---- Shifter ----

Arm7Tdmi::ShiftResult Arm7Tdmi::lsl(u32 v, u32 amount, bool carry_in) const
{
	if (amount == 0)
		return {v, carry_in};
	if (amount < 32)
		return {v << amount, ((v >> (32 - amount)) & 1) != 0};
	if (amount == 32)
		return {0, (v & 1) != 0};
	return {0, false};
}

Arm7Tdmi::ShiftResult Arm7Tdmi::lsr(u32 v, u32 amount, bool carry_in, bool imm) const
{
	if (imm && amount == 0) { // LSR #0 = LSR #32.
		return {0, ((v >> 31) & 1) != 0};
	}
	if (amount == 0)
		return {v, carry_in};
	if (amount < 32)
		return {v >> amount, ((v >> (amount - 1)) & 1) != 0};
	if (amount == 32)
		return {0, ((v >> 31) & 1) != 0};
	return {0, false};
}

Arm7Tdmi::ShiftResult Arm7Tdmi::asr(u32 v, u32 amount, bool carry_in, bool imm) const
{
	if (imm && amount == 0) { // ASR #0 = ASR #32 (sign fill).
		const bool top = (v >> 31) != 0;
		return {top ? 0xFFFFFFFFu : 0u, top};
	}
	if (amount == 0)
		return {v, carry_in};
	const bool top = (v >> 31) != 0;
	if (amount < 32) {
		u32 res = v >> amount;
		if (top)
			res |= 0xFFFFFFFFu << (32 - amount);
		return {res, ((v >> (amount - 1)) & 1) != 0};
	}
	return {top ? 0xFFFFFFFFu : 0u, top};
}

Arm7Tdmi::ShiftResult Arm7Tdmi::ror(u32 v, u32 amount, bool carry_in, bool imm) const
{
	if (imm && amount == 0) { // RRX.
		return {(carry_in ? 0x80000000u : 0u) | (v >> 1), (v & 1) != 0};
	}
	amount &= 31;
	if (amount == 0)
		return {v, carry_in};
	return {(v >> amount) | (v << (32 - amount)), ((v >> (amount - 1)) & 1) != 0};
}

// ---- Memory (GBATEK "CPU Memory Alignments") ----

u32 Arm7Tdmi::loadWord(u32 addr, bool seq)
{
	cycles_ += bus_->accessCycles(addr, 4, seq);
	const u32 aligned = addr & ~3u;
	const u32 word = bus_->read32(aligned);
	if (io_trace_)
		logIoAccess('L', 4, aligned, word, instr_addr_);
	// Unaligned LDR rotates right by 8 * (addr & 3).
	const u32 rot = (addr & 3u) * 8u;
	if (rot == 0)
		return word;
	return (word >> rot) | (word << (32 - rot));
}

void Arm7Tdmi::storeWord(u32 addr, u32 value, bool seq)
{
	cycles_ += bus_->accessCycles(addr, 4, seq);
	if (io_trace_)
		logIoAccess('S', 4, addr & ~3u, value, instr_addr_);
	bus_->write32(addr & ~3u, value); // Low bits ignored on store.
}

u32 Arm7Tdmi::loadHalf(u32 addr, bool sign_extend, bool seq)
{
	cycles_ += bus_->accessCycles(addr, 2, seq);
	// GBATEK "Mis-aligned LDRH,LDRSH" on ARM7:
	//   LDRH  Rd,[odd] -> LDRH Rd,[odd-1] ROR 8   (bits 0-7 and 24-31)
	//   LDRSH Rd,[odd] -> LDRSB Rd,[odd]          (sign-extend that byte)
	const u32 aligned = addr & ~1u;
	if (sign_extend) {
		if ((addr & 1u) != 0) {
			const u8 b = bus_->read8(addr);
			if (io_trace_)
				logIoAccess('L', 1, addr, b, instr_addr_);
			return static_cast<u32>(static_cast<i32>(static_cast<i8>(b)));
		}
		const u16 half = bus_->read16(aligned);
		if (io_trace_)
			logIoAccess('L', 2, aligned, half, instr_addr_);
		return static_cast<u32>(static_cast<i32>(static_cast<i16>(half)));
	}
	const u16 half = bus_->read16(aligned);
	if (io_trace_)
		logIoAccess('L', 2, aligned, half, instr_addr_);
	if ((addr & 1u) == 0)
		return half;
	const u32 rotated = (static_cast<u32>(half) >> 8) | (static_cast<u32>(half) << 24);
	return rotated;
}

void Arm7Tdmi::storeHalf(u32 addr, u16 value, bool seq)
{
	cycles_ += bus_->accessCycles(addr, 2, seq);
	if (io_trace_)
		logIoAccess('S', 2, addr & ~1u, value, instr_addr_);
	bus_->write16(addr & ~1u, value);
}

u8 Arm7Tdmi::loadByte(u32 addr, bool seq)
{
	cycles_ += bus_->accessCycles(addr, 1, seq);
	const u8 v = bus_->read8(addr);
	if (io_trace_)
		logIoAccess('L', 1, addr, v, instr_addr_);
	return v;
}

void Arm7Tdmi::storeByte(u32 addr, u8 value, bool seq)
{
	cycles_ += bus_->accessCycles(addr, 1, seq);
	if (io_trace_)
		logIoAccess('S', 1, addr, value, instr_addr_);
	bus_->write8(addr, value);
}

// ---- Exceptions ----

static u32 exceptionVector(CpuException e)
{
	switch (e) {
	case CpuException::Reset:
		return 0x00;
	case CpuException::Undefined:
		return 0x04;
	case CpuException::Swi:
		return 0x08;
	case CpuException::PrefetchAbort:
		return 0x0C;
	case CpuException::DataAbort:
		return 0x10;
	case CpuException::Irq:
		return 0x18;
	case CpuException::Fiq:
		return 0x1C;
	}
	return 0x00;
}

void Arm7Tdmi::enterException(CpuException e)
{
	CpuMode new_mode = CpuMode::Supervisor;
	u32 lr = 0;
	const bool thumb = (state_ == CpuState::Thumb);
	const u32 next_fetch = pc_; // Already advanced past instr_addr_.
	switch (e) {
	case CpuException::Reset:
		new_mode = CpuMode::Supervisor;
		lr = 0;
		break;
	case CpuException::Undefined:
	case CpuException::Swi:
		// GBATEK: ARM R14=PC+4, THUMB R14=PC+2 (PC = $ here).
		new_mode = (e == CpuException::Swi) ? CpuMode::Supervisor : CpuMode::Undefined;
		lr = thumb ? (instr_addr_ + 2) : (instr_addr_ + 4);
		break;
	case CpuException::PrefetchAbort:
		new_mode = CpuMode::Abort;
		lr = instr_addr_ + 4;
		break;
	case CpuException::DataAbort:
		new_mode = CpuMode::Abort;
		lr = instr_addr_ + 8;
		break;
	case CpuException::Irq:
		new_mode = CpuMode::Irq;
		lr = next_fetch + 4; // Resume at next fetch via SUBS#4.
		break;
	case CpuException::Fiq:
		new_mode = CpuMode::Fiq;
		lr = next_fetch + 4;
		break;
	}
	Bank *b = bankFor(new_mode);
	if (b != nullptr) {
		b->spsr = cpsr_;
		b->r14 = lr;
	}
	// Abort-class entries wedge real software (no handler restores them);
	// freeze the trace so the vector B-loop can't overwrite the history.
	if (e == CpuException::Undefined || e == CpuException::PrefetchAbort ||
		e == CpuException::DataAbort) {
		trace_frozen_ = true;
	}
	switchMode(new_mode);
	setFlag(kFlagT, false);
	state_ = CpuState::Arm;
	setFlag(kFlagI, true);
	if (e == CpuException::Reset || e == CpuException::Fiq)
		setFlag(kFlagF, true);
	pc_ = exceptionVector(e);
	fetch_seq_ = false;
	syncBiosActive();
}

void Arm7Tdmi::exceptionReturn(u32 lr_adjust)
{
	Bank *b = bankFor(mode());
	if (b == nullptr)
		return; // User/System have no SPSR to restore.
	const u32 spsr = b->spsr;
	const u32 ret = b->r14 - lr_adjust;
	// Restore CPSR first (mode switch), then branch. T comes from SPSR.
	const bool thumb = (spsr & (1u << kFlagT)) != 0;
	cpsr_ = spsr;
	state_ = thumb ? CpuState::Thumb : CpuState::Arm;
	if (state_ == CpuState::Arm)
		pc_ = ret & ~3u;
	else
		pc_ = ret & ~1u;
	fetch_seq_ = false;
	syncBiosActive();
}

void Arm7Tdmi::restoreCpsrFromSpsr()
{
	// Same as exceptionReturn but keeps executing at the already-set PC
	// (used when the destination PC came from the instruction itself).
	Bank *b = bankFor(mode());
	if (b == nullptr)
		return;
	cpsr_ = b->spsr;
	state_ = getFlag(kFlagT) ? CpuState::Thumb : CpuState::Arm;
}

void Arm7Tdmi::setState(CpuState s)
{
	state_ = s;
	setFlag(kFlagT, s == CpuState::Thumb);
}

u32 Arm7Tdmi::fetchArm()
{
	cycles_ += bus_->accessCycles(instr_addr_ & ~3u, 4, fetch_seq_);
	fetch_seq_ = true;
	return bus_->read32(instr_addr_ & ~3u);
}

u16 Arm7Tdmi::fetchThumb()
{
	cycles_ += bus_->accessCycles(instr_addr_ & ~1u, 2, fetch_seq_);
	fetch_seq_ = true;
	return bus_->read16(instr_addr_ & ~1u);
}

void Arm7Tdmi::checkIrq()
{
	if (irq_pending_ && !getFlag(kFlagI)) {
		irq_pending_ = false;
		enterException(CpuException::Irq);
		cycles_ += 3; // Pipeline refill on entry (2S+1N, S=N=1).
	}
}

u32 Arm7Tdmi::step()
{
	const u64 start = cycles_;
	if (halted_) { // Low-power wait: burn a cycle, then wake on GBATEK's
		// Halt condition (IE AND IF) != 0. IME and CPSR.I don't gate waking
		// (only exception entry via checkIrq below); irq_pending_ covers the
		// case where the bus is unavailable (unit tests).
		cycles_ += 1;
		bool fire = irq_pending_ && !getFlag(kFlagI);
		if (bus_ != nullptr) {
			const u16 fired =
				static_cast<u16>(bus_->read16(0x04000200) & bus_->read16(0x04000202));
			if (fired != 0)
				fire = true;
		}
		if (fire) {
			halted_ = false;
			checkIrq();
		}
		return static_cast<u32>(cycles_ - start);
	}
	const bool was_thumb = (state_ == CpuState::Thumb);
	if (!was_thumb) {
		instr_addr_ = pc_;
		pc_ = instr_addr_ + 4;
		const u32 instr = fetchArm();
		if (!trace_frozen_) {
			trace_[trace_head_ % kTraceSize] = {instr_addr_, instr, cpsr_, false};
			trace_head_++;
		}
		cycles_ += executeArm(*this, instr, instr_addr_);
	} else {
		instr_addr_ = pc_;
		pc_ = instr_addr_ + 2;
		const u16 instr = fetchThumb();
		if (!trace_frozen_) {
			trace_[trace_head_ % kTraceSize] = {instr_addr_, instr, cpsr_, true};
			trace_head_++;
		}
		cycles_ += executeThumb(*this, instr, instr_addr_);
	}
	checkIrq();
	// Branch ring: any non-fallthrough PC change (branch, exception
	// entry/return, SWI, IRQ). Skipped while frozen (fault wedged).
	if (!trace_frozen_) {
		const u32 fallthrough = instr_addr_ + (was_thumb ? 2u : 4u);
		if (pc_ != fallthrough) {
			branches_[branch_head_ % kBranchSize] = {instr_addr_, pc_, cpsr_, was_thumb};
			branch_head_++;
		}
	}
	return static_cast<u32>(cycles_ - start);
}

void Arm7Tdmi::save(StateBuffer &out) const
{
	out.write(r_);
	out.write(r8_usr_);
	out.write(r8_fiq_);
	out.write(r13_usr_);
	out.write(r14_usr_);
	out.write(fiq_);
	out.write(irq_);
	out.write(svc_);
	out.write(abt_);
	out.write(und_);
	out.write(cpsr_);
	out.write(pc_);
	out.write(instr_addr_);
	out.write(fetch_seq_);
	out.write(state_);
	out.write(irq_pending_);
	out.write(halted_);
	out.write(cycles_);
}

void Arm7Tdmi::load(const StateBuffer &in)
{
	in.read(r_);
	in.read(r8_usr_);
	in.read(r8_fiq_);
	in.read(r13_usr_);
	in.read(r14_usr_);
	in.read(fiq_);
	in.read(irq_);
	in.read(svc_);
	in.read(abt_);
	in.read(und_);
	in.read(cpsr_);
	in.read(pc_);
	in.read(instr_addr_);
	in.read(fetch_seq_);
	in.read(state_);
	in.read(irq_pending_);
	in.read(halted_);
	in.read(cycles_);
}

} // namespace gba
