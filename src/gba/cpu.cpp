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
		std::printf("gba-io: %c%u %08X = %08X @ %08X\n", op, width, addr, value, instr_addr);
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
	pf_valid_ = false; // Fresh code everywhere after a reset.
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
	// Any PC change moves the fetch stream: the queue's address/stride no
	// longer describes what comes next. This is not just a timing reset - a
	// Thumb->ARM switch can land on a queue entry filled with a *halfword*
	// (padded to 32 bits), and the address check then matches while the
	// decoded ARM word is garbage. That is what sent Pokemon Emerald's boot
	// code into a block of 0x00000000 ~200 frames in.
	invalidatePrefetch();
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
	const bool mask_sx = use_spsr; // Bank access is privileged; no SPR mask bit.
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
	if (!imm && amount != 0 && (amount & 31u) == 0) {
		// ROR by a nonzero multiple of 32: result unchanged, carry out is
		// bit 31 (ARM ARM; jsmolka thumb test 66). Distinct from amount 0,
		// which leaves carry untouched (below).
		return {v, (v >> 31) != 0};
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
		logIoAccess('S', 4, addr, value, instr_addr_);
	// No pre-alignment: plain regions ignore A1-0 inside write32, while
	// SRAM needs the low bits for its 8-bit-bus byte select (save 8).
	bus_->write32(addr, value);
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
		logIoAccess('S', 2, addr, value, instr_addr_);
	// No pre-alignment: plain regions ignore A0 inside write16, while SRAM
	// needs the odd address for its 8-bit-bus byte select (jsmolka save 6).
	bus_->write16(addr, value);
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
	invalidatePrefetch(); // Exception entry jumps into the BIOS vectors.
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
	// The queue holds halfwords in Thumb and words in ARM: never reuse it
	// across the switch (see setPc).
	invalidatePrefetch();
	setFlag(kFlagT, s == CpuState::Thumb);
}

u32 Arm7Tdmi::fetchArm()
{
	cycles_ += bus_->fetchCycles(instr_addr_ & ~3u, 4, fetch_seq_);
	fetch_seq_ = true;
	if (!(pf_valid_ && pf_addr_[0] == instr_addr_)) {
		// Flush (branch, exception, reset, load, fresh code): this fetch
		// plus the next, straight from the bus.
		pf_addr_[0] = instr_addr_;
		pf_val_[0] = bus_->read32(instr_addr_ & ~3u);
		pf_addr_[1] = instr_addr_ + 4;
		pf_val_[1] = bus_->read32((instr_addr_ + 4) & ~3u);
		pf_valid_ = true;
	}
	const u32 instr = pf_val_[0];
	// Shift, then refill the tail BEFORE execution: the refill observes
	// memory ahead of the stores below (fetch-ahead, nes test 1).
	pf_addr_[0] = pf_addr_[1];
	pf_val_[0] = pf_val_[1];
	pf_addr_[1] = pf_addr_[0] + 4;
	pf_val_[1] = bus_->read32(pf_addr_[1] & ~3u);
	return instr;
}

u16 Arm7Tdmi::fetchThumb()
{
	cycles_ += bus_->fetchCycles(instr_addr_ & ~1u, 2, fetch_seq_);
	fetch_seq_ = true;
	if (!(pf_valid_ && pf_addr_[0] == instr_addr_)) {
		pf_addr_[0] = instr_addr_;
		pf_val_[0] = bus_->read16(instr_addr_ & ~1u);
		pf_addr_[1] = instr_addr_ + 2;
		pf_val_[1] = bus_->read16((instr_addr_ + 2) & ~1u);
		pf_valid_ = true;
	}
	const u16 instr = static_cast<u16>(pf_val_[0] & 0xFFFFu);
	pf_addr_[0] = pf_addr_[1];
	pf_val_[0] = pf_val_[1];
	pf_addr_[1] = pf_addr_[0] + 2;
	pf_val_[1] = bus_->read16(pf_addr_[1] & ~1u);
	return instr;
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
			const u16 fired = static_cast<u16>(bus_->read16(0x04000200) & bus_->read16(0x04000202));
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
	// The value-prefetch queue is a deterministic function of (pc, memory)
	// and mid-instruction states are never serialized, so it rebuilds on
	// the next fetch instead of round-tripping.
	pf_valid_ = false;
	in.read(state_);
	in.read(irq_pending_);
	in.read(halted_);
	in.read(cycles_);
}

// ---- Minimal ARMv4T disassembler (debugger trace viewer) ----
// Not a full decoder: the common forms render, everything else falls back
// to `.word`. Mirrors the execute dispatch (executeGroup00 / executeSingle
// / executeBlock / B-BL in cpu_arm.cpp, executeThumb in cpu_thumb.cpp).

namespace
{

const char *armCondName(unsigned cond)
{
	static const char *names[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
									"hi", "ls", "ge", "lt", "gt", "le", "",   "nv"};
	return names[cond & 15];
}

// ABI register name into a 4-byte buffer ("sp"/"lr"/"pc", else "rN").
// Each call site uses its own buffers (argument order is unspecified).
const char *regName(unsigned r, char *buf)
{
	switch (r) {
	case 13:
		return "sp";
	case 14:
		return "lr";
	case 15:
		return "pc";
	default:
		break;
	}
	std::snprintf(buf, 4, "r%u", r & 15);
	return buf;
}

// "r0-r3, lr" (bit14 = lr, bit15 = pc); "{}" when empty.
void appendRlist(char *out, size_t n, size_t &len, u32 mask)
{
	static const char *reg[16] = {"r0", "r1", "r2",  "r3",  "r4",  "r5", "r6", "r7",
								  "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
	len += static_cast<size_t>(std::snprintf(out + len, n - len, "{"));
	bool first = true;
	for (unsigned r = 0; r < 16;) {
		if ((mask & (1u << r)) == 0) {
			r++;
			continue;
		}
		unsigned e = r;
		while (e + 1 < 16 && (mask & (1u << (e + 1))) != 0)
			e++;
		if (!first)
			len += static_cast<size_t>(std::snprintf(out + len, n - len, ", "));
		first = false;
		if (e == r)
			len += static_cast<size_t>(std::snprintf(out + len, n - len, "%s", reg[r]));
		else if (e == r + 1)
			len += static_cast<size_t>(std::snprintf(out + len, n - len, "%s, %s", reg[r], reg[e]));
		else
			len += static_cast<size_t>(std::snprintf(out + len, n - len, "%s-%s", reg[r], reg[e]));
		r = e + 1;
	}
	len += static_cast<size_t>(std::snprintf(out + len, n - len, "}"));
}

// Data-processing second operand: "#12" / "#0x3A00000" / "lr" / "r1, lsl #2".
void appendOp2(char *out, size_t n, size_t &len, u32 instr, bool imm)
{
	if (imm) {
		const unsigned rot = ((instr >> 8) & 0xF) * 2;
		const u32 imm8 = instr & 0xFFu;
		u32 v = rot == 0 ? imm8 : ((imm8 >> rot) | (imm8 << (32 - rot)));
		if (rot == 0)
			len += static_cast<size_t>(std::snprintf(out + len, n - len, "#%u", v));
		else
			len += static_cast<size_t>(std::snprintf(out + len, n - len, "#0x%X", v));
		return;
	}
	static const char *shift[4] = {"lsl", "lsr", "asr", "ror"};
	const unsigned rm = instr & 0xF;
	const unsigned type = (instr >> 5) & 3;
	char rmb[4], rsb[4];
	if (((instr >> 4) & 1) == 0) {
		const unsigned amount = (instr >> 7) & 0x1F;
		if (amount == 0 && type == 0) {
			len += static_cast<size_t>(std::snprintf(out + len, n - len, "%s", regName(rm, rmb)));
			return;
		}
		if (type == 3 && amount == 0) {
			len +=
				static_cast<size_t>(std::snprintf(out + len, n - len, "%s, rrx", regName(rm, rmb)));
			return;
		}
		len += static_cast<size_t>(
			std::snprintf(out + len, n - len, "%s, %s #%u", regName(rm, rmb), shift[type], amount));
		return;
	}
	len += static_cast<size_t>(std::snprintf(out + len, n - len, "%s, %s %s", regName(rm, rmb),
											 shift[type], regName((instr >> 8) & 0xF, rsb)));
}

} // namespace

std::string disasmArm(u32 addr, u32 instr)
{
	char out[128];
	size_t len = 0;
	const unsigned cond = (instr >> 28) & 0xF;
	const char *cs = armCondName(cond);
	const unsigned top = (instr >> 26) & 3;
	if (top == 0) {
		const bool bit25 = ((instr >> 25) & 1) != 0;
		const unsigned rn = (instr >> 16) & 0xF;
		const unsigned rd = (instr >> 12) & 0xF;
		// BX (checked first: aliases MSR).
		if ((instr & 0x0FFFFFF0u) == 0x012FFF10u) {
			char rmb[4];
			std::snprintf(out, sizeof out, "bx%s %s", cs, regName(instr & 0xF, rmb));
			return out;
		}
		// MRS.
		if (((instr >> 23) & 0x1F) == 0x02 && ((instr >> 16) & 0x3F) == 0x0F &&
			(instr & 0xFFF) == 0) {
			char rdb[4];
			std::snprintf(out, sizeof out, "mrs%s %s, %s", cs, regName(rd, rdb),
						  ((instr >> 22) & 1) != 0 ? "spsr" : "cpsr");
			return out;
		}
		// MSR (register).
		if (((instr >> 23) & 0x1F) == 0x02 && ((instr >> 20) & 3) == 0x02 && !bit25) {
			char fields[5] = "";
			size_t fl = 0;
			const unsigned mask = (instr >> 16) & 0xF;
			if ((mask & 8) != 0)
				fields[fl++] = 'f';
			if ((mask & 4) != 0)
				fields[fl++] = 's';
			if ((mask & 2) != 0)
				fields[fl++] = 'x';
			if ((mask & 1) != 0)
				fields[fl++] = 'c';
			fields[fl] = '\0';
			char rmb[4];
			std::snprintf(out, sizeof out, "msr%s %s_%s, %s", cs,
						  ((instr >> 22) & 1) != 0 ? "spsr" : "cpsr", fields,
						  regName(instr & 0xF, rmb));
			return out;
		}
		// MSR (immediate).
		if ((((instr >> 20) & 0xFFu) == 0x32u) || (((instr >> 20) & 0xFFu) == 0x36u)) {
			len += static_cast<size_t>(std::snprintf(out, sizeof out, "msr%s %s_fsxc, ", cs,
													 ((instr >> 22) & 1) != 0 ? "spsr" : "cpsr"));
			appendOp2(out, sizeof out, len, instr, true);
			return out;
		}
		// Multiply.
		if (((instr >> 23) & 0x1F) <= 1 && !bit25 && ((instr & 0xF0u) == 0x90u)) {
			const unsigned rm = instr & 0xF;
			const unsigned rs = (instr >> 8) & 0xF;
			const unsigned r_n = (instr >> 12) & 0xF;
			const unsigned r_d = (instr >> 16) & 0xF;
			const bool s = ((instr >> 20) & 1) != 0;
			if (((instr >> 23) & 1) == 0) {
				const bool acc = ((instr >> 21) & 1) != 0;
				char rdb[4], rmb[4], rsb[4], rnb[4];
				if (acc)
					std::snprintf(out, sizeof out, "mla%s%s %s, %s, %s, %s", cs, s ? "s" : "",
								  regName(r_d, rdb), regName(rm, rmb), regName(rs, rsb),
								  regName(r_n, rnb));
				else
					std::snprintf(out, sizeof out, "mul%s%s %s, %s, %s", cs, s ? "s" : "",
								  regName(r_d, rdb), regName(rm, rmb), regName(rs, rsb));
				return out;
			}
			const bool is_signed = ((instr >> 22) & 1) != 0;
			const bool acc = ((instr >> 21) & 1) != 0;
			const char *lname = !is_signed ? (acc ? "umlal" : "umull") : (acc ? "smlal" : "smull");
			char rnb[4], rdb[4], rmb[4], rsb[4];
			std::snprintf(out, sizeof out, "%s%s%s %s, %s, %s, %s", lname, cs, s ? "s" : "",
						  regName(r_n, rnb), regName(r_d, rdb), regName(rm, rmb), regName(rs, rsb));
			return out;
		}
		// SWP/SWPB.
		if ((instr & 0x0FB00FF0u) == 0x01000090u) {
			char rdb[4], rmb[4], rnb[4];
			std::snprintf(out, sizeof out, "swp%s%s %s, %s, [%s]", cs,
						  ((instr >> 22) & 1) != 0 ? "b" : "", regName(rd, rdb),
						  regName(instr & 0xF, rmb), regName(rn, rnb));
			return out;
		}
		// Halfword/signed group.
		if (!bit25 && ((instr >> 7) & 1) != 0 && ((instr >> 4) & 1) != 0) {
			const bool load = ((instr >> 20) & 1) != 0;
			const unsigned h = (instr >> 5) & 3;
			const bool pre = ((instr >> 24) & 1) != 0;
			const bool up = ((instr >> 23) & 1) != 0;
			const bool wb = ((instr >> 21) & 1) != 0;
			const char *name = nullptr;
			if (h == 1)
				name = load ? "ldrh" : "strh";
			else if (h == 2 && load)
				name = "ldrsb";
			else if (h == 3 && load)
				name = "ldrsh";
			if (name == nullptr) {
				std::snprintf(out, sizeof out, ".word 0x%08X", instr);
				return out;
			}
			char off[24];
			if (((instr >> 22) & 1) != 0)
				std::snprintf(off, sizeof off, "#%u", (((instr >> 8) & 0xF) << 4) | (instr & 0xF));
			else
				std::snprintf(off, sizeof off, "r%u", instr & 0xF);
			if (pre) {
				char rdb[4], rnb[4];
				std::snprintf(out, sizeof out, "%s%s %s, [%s, %s%s]%s", name, cs, regName(rd, rdb),
							  regName(rn, rnb), up ? "" : "-", off, wb ? "!" : "");
			} else {
				char rdb[4], rnb[4];
				std::snprintf(out, sizeof out, "%s%s %s, [%s], %s%s", name, cs, regName(rd, rdb),
							  regName(rn, rnb), up ? "" : "-", off);
			}
			return out;
		}
		// Data processing.
		const unsigned opcode = (instr >> 21) & 0xF;
		static const char *dp[16] = {"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
									 "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"};
		const bool set_flags = ((instr >> 20) & 1) != 0;
		if (opcode >= 8 && opcode <= 11) { // Comparisons: no Rd, always set.
			char rnb[4];
			len += static_cast<size_t>(
				std::snprintf(out, sizeof out, "%s%s %s, ", dp[opcode], cs, regName(rn, rnb)));
			appendOp2(out, sizeof out, len, instr, bit25);
			return out;
		}
		if (opcode == 13 || opcode == 15) { // MOV/MVN: no Rn.
			char rdb[4];
			len += static_cast<size_t>(std::snprintf(out, sizeof out, "%s%s%s %s, ", dp[opcode], cs,
													 set_flags ? "s" : "", regName(rd, rdb)));
			appendOp2(out, sizeof out, len, instr, bit25);
			return out;
		}
		char rdb[4], rnb[4];
		len += static_cast<size_t>(std::snprintf(out, sizeof out, "%s%s%s %s, %s, ", dp[opcode], cs,
												 set_flags ? "s" : "", regName(rd, rdb),
												 regName(rn, rnb)));
		appendOp2(out, sizeof out, len, instr, bit25);
		return out;
	}
	if (top == 1) {
		// Single data transfer.
		const bool imm = ((instr >> 25) & 1) == 0;
		if (!imm && ((instr >> 4) & 1) == 1) {
			std::snprintf(out, sizeof out, ".word 0x%08X", instr);
			return out;
		}
		const bool pre = ((instr >> 24) & 1) != 0;
		const bool up = ((instr >> 23) & 1) != 0;
		const bool byte = ((instr >> 22) & 1) != 0;
		const bool wb = ((instr >> 21) & 1) != 0;
		const bool load = ((instr >> 20) & 1) != 0;
		const unsigned rn = (instr >> 16) & 0xF;
		const unsigned rd = (instr >> 12) & 0xF;
		const char *t = (!pre && wb) ? "t" : "";
		char off[32];
		if (imm)
			std::snprintf(off, sizeof off, "#%u", instr & 0xFFFu);
		else {
			size_t ol = 0;
			appendOp2(off, sizeof off, ol, instr, false);
		}
		if (pre) {
			char rdb[4], rnb[4];
			std::snprintf(out, sizeof out, "%s%s%s %s, [%s, %s%s]%s", load ? "ldr" : "str", cs,
						  byte ? "b" : "", regName(rd, rdb), regName(rn, rnb), up ? "" : "-", off,
						  wb ? "!" : "");
		} else {
			char rdb[4], rnb[4];
			std::snprintf(out, sizeof out, "%s%s%s%s %s, [%s], %s%s", load ? "ldr" : "str", cs,
						  byte ? "b" : "", t, regName(rd, rdb), regName(rn, rnb), up ? "" : "-",
						  off);
		}
		return out;
	}
	if (top == 2) {
		if (((instr >> 25) & 1) != 0) { // B / BL.
			const bool link = ((instr >> 24) & 1) != 0;
			// 24-bit sign-extended offset, *4, from $+8.
			const i32 off = (static_cast<i32>(instr & 0xFFFFFFu) ^ 0x800000) - 0x800000;
			std::snprintf(out, sizeof out, "%s%s 0x%08X", link ? "bl" : "b", link ? "" : cs,
						  addr + 8 + static_cast<u32>(off * 4));
			return out;
		}
		// Block transfer.
		const bool pre = ((instr >> 24) & 1) != 0;
		const bool up = ((instr >> 23) & 1) != 0;
		const bool psr = ((instr >> 22) & 1) != 0;
		const bool wb = ((instr >> 21) & 1) != 0;
		const bool load = ((instr >> 20) & 1) != 0;
		const unsigned rn = (instr >> 16) & 0xF;
		const char *mode = !pre && up ? "ia" : (pre && up ? "ib" : (!pre ? "da" : "db"));
		char rnb[4];
		len += static_cast<size_t>(std::snprintf(out, sizeof out, "%s%s%s %s%s, ",
												 load ? "ldm" : "stm", cs, mode, regName(rn, rnb),
												 wb ? "!" : ""));
		appendRlist(out, sizeof out, len, instr & 0xFFFFu);
		if (psr)
			len += static_cast<size_t>(std::snprintf(out + len, sizeof out - len, "^"));
		return out;
	}
	// Top == 3: SWI or coprocessor/undefined.
	if (((instr >> 24) & 0xF) == 0xF) {
		std::snprintf(out, sizeof out, "swi 0x%06X", instr & 0xFFFFFFu);
		return out;
	}
	std::snprintf(out, sizeof out, ".word 0x%08X", instr);
	return out;
}

std::string disasmThumb(u32 addr, u16 instr)
{
	char out[96];
	const unsigned top = (instr >> 12) & 0xF;
	static const char *tcond[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
									"hi", "ls", "ge", "lt", "gt", "le", "",   "nv"};
	switch (top) {
	case 0x0:
	case 0x1: {
		// 000xx: shifts, except x0011x which is ADD/SUB (mirrors the
		// decoder gate: bits[15:11] == 00011).
		if (((instr >> 11) & 0x1F) == 0x03) {
			// ADD/SUB: bit 10 selects imm3 vs register, bit 9 SUB vs ADD.
			const bool is_sub = ((instr >> 9) & 1) != 0;
			const unsigned rn = (instr >> 3) & 7;
			if (((instr >> 10) & 1) != 0) {
				std::snprintf(out, sizeof out, "%s r%u, r%u, #%u", is_sub ? "sub" : "add",
							  instr & 7, rn, (instr >> 6) & 7);
			} else {
				std::snprintf(out, sizeof out, "%s r%u, r%u, r%u", is_sub ? "sub" : "add",
							  instr & 7, rn, (instr >> 6) & 7);
			}
			return out;
		}
		// Shifts: op 00 LSL, 01 LSR, 10/11 ASR (decoder else-branch).
		const unsigned op = (instr >> 11) & 3;
		static const char *sh[4] = {"lsl", "lsr", "asr", "asr"};
		const unsigned rs = (instr >> 3) & 7;
		const unsigned rd = instr & 7;
		const unsigned imm = (instr >> 6) & 0x1F;
		if (imm == 0 && op == 0) {
			std::snprintf(out, sizeof out, "mov r%u, r%u", rd, rs);
			return out;
		}
		std::snprintf(out, sizeof out, "%s r%u, r%u, #%u", sh[op], rd, rs,
					  op == 0 ? imm : (imm == 0 ? 32 : imm));
		return out;
	}
	case 0x2:
	case 0x3: {
		// MOV/CMP/ADD/SUB (immediate).
		const unsigned op = (instr >> 11) & 3;
		static const char *names[4] = {"mov", "cmp", "add", "sub"};
		std::snprintf(out, sizeof out, "%s r%u, #%u", names[op], (instr >> 8) & 7, instr & 0xFFu);
		return out;
	}
	case 0x4: {
		const unsigned sub = (instr >> 10) & 3;
		if (sub == 0) {
			// ALU.
			static const char *alu[16] = {"and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror",
										  "tst", "neg", "cmp", "cmn", "orr", "mul", "bic", "mvn"};
			const unsigned op = (instr >> 6) & 0xF;
			const unsigned rs = (instr >> 3) & 7;
			const unsigned rd = instr & 7;
			std::snprintf(out, sizeof out, "%s r%u, r%u", alu[op], rd, rs);
			return out;
		}
		if (sub == 1) {
			// HiReg / BX.
			const unsigned op = (instr >> 8) & 3;
			const unsigned rd = (((instr >> 7) & 1) != 0 ? 8 : 0) | (instr & 7);
			const unsigned rs = (((instr >> 6) & 1) != 0 ? 8 : 0) | ((instr >> 3) & 7);
			char rdb[4], rsb[4];
			if (op == 3) {
				std::snprintf(out, sizeof out, "bx %s", regName(rs, rsb));
				return out;
			}
			static const char *hrop[3] = {"add", "cmp", "mov"};
			std::snprintf(out, sizeof out, "%s %s, %s", hrop[op], regName(rd, rdb),
						  regName(rs, rsb));
			return out;
		}
		// Literal load.
		std::snprintf(out, sizeof out, "ldr r%u, [pc, #%u]", (instr >> 8) & 7,
					  (instr & 0xFFu) * 4u);
		return out;
	}
	case 0x5:
	case 0x6:
	case 0x7: {
		const unsigned rt = instr & 7;
		if (top == 0x5 && ((instr >> 9) & 1) != 0) {
			// Halfword/signed, register offset.
			static const char *op8[4] = {"strh", "ldsb", "ldrh", "ldsh"};
			std::snprintf(out, sizeof out, "%s r%u, [r%u, r%u]", op8[(instr >> 10) & 3], rt,
						  (instr >> 6) & 7, (instr >> 3) & 7);
			return out;
		}
		if (top == 0x5) {
			const bool load = ((instr >> 11) & 1) != 0;
			const bool byte = ((instr >> 10) & 1) != 0;
			std::snprintf(out, sizeof out, "%s%s r%u, [r%u, r%u]", load ? "ldr" : "str",
						  byte ? "b" : "", rt, (instr >> 6) & 7, (instr >> 3) & 7);
			return out;
		}
		const bool byte = ((instr >> 12) & 1) != 0;
		const bool load = ((instr >> 11) & 1) != 0;
		const unsigned off = (instr >> 6) & 0x1F;
		std::snprintf(out, sizeof out, "%s%s r%u, [r%u, #%u]", load ? "ldr" : "str",
					  byte ? "b" : "", rt, (instr >> 3) & 7, byte ? off : off * 4u);
		return out;
	}
	case 0x8: {
		// STRH/LDRH immediate.
		const bool load = ((instr >> 11) & 1) != 0;
		std::snprintf(out, sizeof out, "%sh r%u, [r%u, #%u]", load ? "ldr" : "str", instr & 7,
					  (instr >> 3) & 7, ((instr >> 6) & 0x1F) * 2u);
		return out;
	}
	case 0x9: {
		// SP-relative.
		const bool load = ((instr >> 11) & 1) != 0;
		std::snprintf(out, sizeof out, "%s r%u, [sp, #%u]", load ? "ldr" : "str", (instr >> 8) & 7,
					  (instr & 0xFFu) * 4u);
		return out;
	}
	case 0xA: {
		// ADD PC/SP.
		const bool sp = ((instr >> 11) & 1) != 0;
		std::snprintf(out, sizeof out, "add r%u, %s, #%u", (instr >> 8) & 7, sp ? "sp" : "pc",
					  (instr & 0xFFu) * 4u);
		return out;
	}
	case 0xB: {
		const unsigned kind = (instr >> 8) & 0xF;
		if (kind == 0x0) {
			const unsigned off = (instr & 0x7Fu) * 4u;
			std::snprintf(out, sizeof out, "%s sp, #%u", ((instr >> 7) & 1) != 0 ? "sub" : "add",
						  off);
			return out;
		}
		if (kind == 0x4 || kind == 0x5) { // PUSH (R = lr).
			u32 rlist = instr & 0xFFu;
			if ((kind & 1) != 0)
				rlist |= 1u << 14;
			size_t len = static_cast<size_t>(std::snprintf(out, sizeof out, "push "));
			appendRlist(out, sizeof out, len, rlist);
			return out;
		}
		if (kind == 0xC || kind == 0xD) { // POP (R = pc).
			u32 rlist = instr & 0xFFu;
			if ((kind & 1) != 0)
				rlist |= 1u << 15;
			size_t len = static_cast<size_t>(std::snprintf(out, sizeof out, "pop "));
			appendRlist(out, sizeof out, len, rlist);
			return out;
		}
		std::snprintf(out, sizeof out, ".hword 0x%04X", instr);
		return out;
	}
	case 0xC: {
		// STMIA / LDMIA.
		const bool load = ((instr >> 11) & 1) != 0;
		const unsigned rb = (instr >> 8) & 7;
		const u32 rlist = instr & 0xFFu;
		size_t len = static_cast<size_t>(
			std::snprintf(out, sizeof out, "%smia r%u!, ", load ? "ld" : "st", rb));
		appendRlist(out, sizeof out, len, rlist);
		return out;
	}
	case 0xD: {
		const unsigned cond = (instr >> 8) & 0xF;
		if (cond == 0xE) {
			std::snprintf(out, sizeof out, ".hword 0x%04X", instr);
			return out;
		}
		if (cond == 0xF) {
			std::snprintf(out, sizeof out, "swi %u", instr & 0xFFu);
			return out;
		}
		const i32 off = (static_cast<i32>(instr & 0xFFu) ^ 0x80) - 0x80;
		std::snprintf(out, sizeof out, "b%s 0x%08X", tcond[cond],
					  addr + 4 + static_cast<u32>(off * 2));
		return out;
	}
	case 0xE: {
		const i32 off = (static_cast<i32>(instr & 0x7FFu) ^ 0x400) - 0x400;
		std::snprintf(out, sizeof out, "b 0x%08X", addr + 4 + static_cast<u32>(off * 2));
		return out;
	}
	default: { // 0xF: BL halves / undefined.
		const unsigned h = (instr >> 11) & 3;
		if (h == 2 || h == 3) {
			std::snprintf(out, sizeof out, "bl%s 0x%03X", h == 2 ? "(1)" : "(2)", instr & 0x7FFu);
			return out;
		}
		std::snprintf(out, sizeof out, ".hword 0x%04X", instr);
		return out;
	}
	}
}

} // namespace gba
