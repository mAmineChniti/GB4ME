#pragma once

#include "gb/types.h"
#include "gba/state.h"
#include <array>
#include <functional>

namespace gba
{

class GbaBus;

// ARM7TDMI CPU modes (CPSR M4-0). GBA software uses User/System for game
// code, IRQ for interrupts and Supervisor for SWIs (BIOS).
// https://problemkaputt.de/gbatek.htm#armcpuregisterset
enum class CpuMode : u8 {
	User = 0x10,
	Fiq = 0x11,
	Irq = 0x12,
	Supervisor = 0x13,
	Abort = 0x17,
	Undefined = 0x1B,
	System = 0x1F,
};

enum class CpuState : u8 {
	Arm,
	Thumb,
};

enum class CpuException : u8 {
	Reset,
	Undefined,
	Swi,
	PrefetchAbort,
	DataAbort,
	Irq,
	Fiq,
};

// CPSR bit positions.
constexpr u8 kFlagN = 31, kFlagZ = 30, kFlagC = 29, kFlagV = 28;
constexpr u8 kFlagI = 7, kFlagF = 6, kFlagT = 5;

// ARM7TDMI core (Phase 2: registers, exceptions, timing hooks).
// Decoders live in cpu_arm.cpp / cpu_thumb.cpp; shared helpers here.
// Memory goes through GbaBus only. Cycle counts use S=N=I=1 until the bus
// grows waitstates (Phase 3); the accounting shape (S/N split) is already
// in place so waitstates slot in without touching decoders.
class Arm7Tdmi
{
  public:
	// HLE BIOS installs this in Phase 4. The CPU performs full SWI
	// exception entry first, then invokes the handler with the 8-bit SWI
	// number (ARM: bits 23-16, Thumb: bits 7-0 — GBATEK SWI comment field).
	// The handler performs exceptionReturn()/returnFromSwi() itself.
	using SwiHandler = std::function<void(u8)>;

	explicit Arm7Tdmi(GbaBus *bus = nullptr);

	void setBus(GbaBus *bus)
	{
		bus_ = bus;
	}
	GbaBus *bus()
	{
		return bus_;
	}
	void reset(); // SVC, ARM state, I+F set, PC=0 (BIOS entry).
	void setSwiHandler(SwiHandler h)
	{
		swi_handler_ = std::move(h);
	}

	void requestIrq()
	{
		irq_pending_ = true;
	}
	void clearIrq()
	{
		irq_pending_ = false;
	}
	bool irqPending() const
	{
		return irq_pending_;
	}

	// Low-power wait (Halt/Stop/IntrWait-family). While halted, step()
	// burns one cycle per call and only wakes for an IRQ with CPSR.I
	// clear (the GBA IRQ controller / HLE wait logic clears the halt).
	// Halt wake-up ignores IME by design (GBATEK); the wait primitives
	// below model the rest.
	void halt()
	{
		halted_ = true;
	}
	void wake()
	{
		halted_ = false;
	}
	bool halted() const
	{
		return halted_;
	}

	// Fetch, decode and execute one instruction, then take a pending IRQ.
	// Returns cycles elapsed (S/N accounting, S=N=1 for now).
	u32 step();

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

	// ---- State (tests, HLE BIOS, debugger) ----
	CpuMode mode() const;
	CpuState state() const
	{
		return state_;
	}
	u32 cpsr() const
	{
		return cpsr_;
	}
	void setCpsr(u32 v);      // Handles banked switch when M4-0 changes.
	u32 spsr() const;         // Current mode's SPSR (0 in User/System).
	void setSpsr(u32 v);      // No-op in User/System.
	u32 reg(u8 i) const;      // Banked; r15 returns the pipeline PC value.
	void setReg(u8 i, u32 v); // Banked; r15 branches (no state change).
	u32 pc() const
	{
		return pc_;
	} // Next fetch address.
	void setPc(u32 addr); // Branch (aligns to current state).
	u32 sp() const
	{
		return reg(13);
	}
	u32 lr() const
	{
		return reg(14);
	}
	u64 cycles() const
	{
		return cycles_;
	}
	u32 lastInstrAddr() const
	{
		return instr_addr_;
	}

	// Recent-instruction ring (Phase 10 debugger precursor): the last
	// executed instructions, newest first via traceBack(0). Recorded
	// unconditionally — one small store per step — so fault dumps and the
	// future debugger can show how the CPU got somewhere.
	struct TraceEntry {
		u32 addr = 0;  // instr_addr_ (fetch address).
		u32 instr = 0; // Fetched word (THUMB: low 16 bits).
		u32 cpsr = 0;  // CPSR during execution.
		bool thumb = false;
	};
	static constexpr size_t kTraceSize = 4096;
	TraceEntry traceBack(size_t n) const
	{
		return trace_[(trace_head_ + kTraceSize - 1 - (n % kTraceSize)) % kTraceSize];
	}
	size_t traceSize() const
	{
		return kTraceSize;
	}
	bool traceFrozen() const
	{
		return trace_frozen_;
	}

	// Branch ring: non-sequential PC changes only (B/BL/BX/LDR-PC/LDM-PC/
	// exception entry+return/SWI). Answers "how did execution get here"
	// even across very long linear slides. Frozen alongside the trace.
	struct BranchEntry {
		u32 from = 0; // instr_addr_ of the branching instruction.
		u32 to = 0;   // pc_ after execution (incl. any IRQ entry).
		u32 cpsr = 0;
		bool thumb = false;
	};
	static constexpr size_t kBranchSize = 4096;
	BranchEntry branchBack(size_t n) const
	{
		return branches_[(branch_head_ + kBranchSize - 1 - (n % kBranchSize)) % kBranchSize];
	}
	size_t branchSize() const
	{
		return kBranchSize;
	}

	bool getFlag(u8 bit) const
	{
		return (cpsr_ & (1u << bit)) != 0;
	}
	void setFlag(u8 bit, bool v);

	// ARM condition field evaluation (cond == 0xF never executes, ARMv4).
	bool condition(u8 cond) const;

	// ---- Exceptions (GBATEK "Actions performed by CPU when entering...") ----
	// LR offsets use $ = faulting instruction address, N = next fetch:
	// SWI/Undef: $+4 (ARM) / $+2 (Thumb), return MOVS. PAbt: $+4, return
	// SUBS#4. DAbt: $+8, return SUBS#8. IRQ/FIQ: N+4, return SUBS#4.
	void enterException(CpuException e);
	// PC = LR_current - adjust; CPSR = SPSR_current. Ignored in
	// User/System (no SPSR). Used by HLE BIOS returns.
	void exceptionReturn(u32 lr_adjust);
	void returnFromSwi()
	{
		exceptionReturn(0);
	} // MOVS PC,LR.
	void switchMode(CpuMode m);
	// Full SPSR -> CPSR restore (mode, T/state, flags). Used by DP/LDM
	// with Rd=15 + S and by exception handlers. No-op in User/System.
	void restoreCpsrFromSpsr();
	// Change ARM/THUMB state without touching mode (BX semantics).
	void setState(CpuState s);

	// PSR transfer helpers shared by ARM (MRS/MSR) and later debugger.
	u32 readPsr(bool spsr) const;
	// Write with field mask (MSR f/s/x/c bits 19-16). Control writes are
	// ignored in User mode; the T bit is never writable here.
	void writePsr(bool use_spsr, u32 value, u8 field_mask);

	// Shifter + data-bus accessors. Public so the ARM/THUMB decoders (which
	// live in their own TUs) and later the HLE BIOS/debugger can share the
	// exact GBA unaligned semantics in one place.
	struct ShiftResult {
		u32 value;
		bool carry;
	};
	ShiftResult lsl(u32 v, u32 amount, bool carry_in) const;
	ShiftResult lsr(u32 v, u32 amount, bool carry_in, bool imm) const;
	ShiftResult asr(u32 v, u32 amount, bool carry_in, bool imm) const;
	ShiftResult ror(u32 v, u32 amount, bool carry_in, bool imm) const;

	// GBA unaligned semantics (GBATEK "Memory Alignments"): LDR word
	// rotates, stores align down. Each access accumulates its bus cycles
	// (WAITCNT-backed, Phase 3) into the cycle counter; `seq` selects S
	// vs N timings where they differ. Abort-capable in a later phase; the
	// bus never aborts yet.
	u32 loadWord(u32 addr, bool seq = false);
	void storeWord(u32 addr, u32 value, bool seq = false);
	u32 loadHalf(u32 addr, bool sign_extend, bool seq = false);
	void storeHalf(u32 addr, u16 value, bool seq = false);
	u8 loadByte(u32 addr, bool seq = false);
	void storeByte(u32 addr, u8 value, bool seq = false);

  private:
	friend u32 executeArm(Arm7Tdmi &cpu, u32 instr, u32 instr_addr);
	friend u32 executeThumb(Arm7Tdmi &cpu, u16 instr, u32 instr_addr);

	GbaBus *bus_ = nullptr;
	SwiHandler swi_handler_;

	// Shared r0-r7; r8-r12 have a FIQ bank; r13/r14/SPSR bank per mode.
	u32 r_[8]{};
	u32 r8_usr_[5]{}, r8_fiq_[5]{};
	u32 r13_usr_ = 0, r14_usr_ = 0;
	struct Bank {
		u32 r13 = 0, r14 = 0, spsr = 0;
	};
	Bank fiq_{}, irq_{}, svc_{}, abt_{}, und_{};

	u32 cpsr_ = 0;
	u32 pc_ = 0;         // Next fetch address.
	u32 instr_addr_ = 0; // Address of the instruction being executed.
	std::array<TraceEntry, kTraceSize> trace_{};
	size_t trace_head_ = 0;
	std::array<BranchEntry, kBranchSize> branches_{};
	size_t branch_head_ = 0;
	// Sticky once an abort-class exception fires: freezes the ring so the
	// fault spin (B-loop at the HLE vector) can't overwrite the history
	// that led to the fault. Cleared by reset().
	bool trace_frozen_ = false;
	// Selective MMIO tracer (env GB4ME_IO_TRACE): logs CPU bus accesses to
	// I/O, GPIO/RTC and save regions with the executing instruction
	// address. Zero cost when unset (checked once per access).
	bool io_trace_ = false;
	// Opcode fetches are sequential (S-cycle) except after a branch,
	// exception, or state change, which refetches non-sequentially.
	bool fetch_seq_ = false;
	CpuState state_ = CpuState::Arm;
	bool irq_pending_ = false;
	bool halted_ = false;
	u64 cycles_ = 0;

	u32 &ref8(u8 i); // r8-r12 with FIQ banking.
	const u32 &ref8(u8 i) const;
	u32 &ref13();
	u32 &ref14();
	Bank *bankFor(CpuMode m);
	// Tracks BIOS-region execution for the bus protection flag: active
	// while the fetch address is inside BIOS (vectors, trampoline, SWI
	// window). Called on every PC change so all drivers stay correct.
	void syncBiosActive();
	// Pipeline PC value seen by reads of r15: ($+8)&~3 ARM, ($+4)&~1 Thumb.
	u32 pcRead() const;
	u32 fetchArm();
	u16 fetchThumb();
	void checkIrq();
};

u32 executeArm(Arm7Tdmi &cpu, u32 instr, u32 instr_addr);
u32 executeThumb(Arm7Tdmi &cpu, u16 instr, u32 instr_addr);

} // namespace gba
