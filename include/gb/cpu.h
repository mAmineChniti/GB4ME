#pragma once

#include "state.h"
#include "types.h"

namespace gb
{

class MMU; // Bus object. CPU must access memory ONLY via MMU::read/write.
		   // https://gbdev.io/pandocs/Memory_Map.html

enum class Flag : u8 { Z = 7, N = 6, H = 5, C = 4 };

struct Registers {
	// Stored as bytes; pairs compose via accessors (layout: F is low byte of
	// AF so flag bits land at Z=7..C=4, matching hardware).
	u8 a = 0, f = 0;
	u8 b = 0, c = 0;
	u8 d = 0, e = 0;
	u8 h = 0, l = 0;
	u16 sp = 0;
	u16 pc = 0;

	u16 af() const
	{
		return make_u16(a, f & 0xF0);
	}
	u16 bc() const
	{
		return make_u16(b, c);
	}
	u16 de() const
	{
		return make_u16(d, e);
	}
	u16 hl() const
	{
		return make_u16(h, l);
	}
	void set_af(u16 v)
	{
		a = hi_byte(v);
		f = lo_byte(v) & 0xF0;
	}
	void set_bc(u16 v)
	{
		b = hi_byte(v);
		c = lo_byte(v);
	}
	void set_de(u16 v)
	{
		d = hi_byte(v);
		e = lo_byte(v);
	}
	void set_hl(u16 v)
	{
		h = hi_byte(v);
		l = lo_byte(v);
	}

	bool get_flag(Flag flag) const
	{
		return (f & (1 << static_cast<u8>(flag))) != 0;
	}

	void set_flag(Flag flag, bool value)
	{
		if (value)
			f |= (1 << static_cast<u8>(flag));
		else
			f &= ~(1 << static_cast<u8>(flag));
	}

	void set_z(bool v)
	{
		set_flag(Flag::Z, v);
	}
	void set_n(bool v)
	{
		set_flag(Flag::N, v);
	}
	void set_h(bool v)
	{
		set_flag(Flag::H, v);
	}
	void set_c(bool v)
	{
		set_flag(Flag::C, v);
	}

	bool get_z() const
	{
		return get_flag(Flag::Z);
	}
	bool get_n() const
	{
		return get_flag(Flag::N);
	}
	bool get_c() const
	{
		return get_flag(Flag::C);
	}
	bool get_h() const
	{
		return get_flag(Flag::H);
	}
};

// LR35902 CPU core.
// https://github.com/aquova/gb-book (chapter order: CPU first)
// https://gbdev.io/pandocs/CPU_Instruction_Set.html
class CPU
{
  public:
	CPU();
	~CPU() = default;

	void set_mmu(MMU *mmu)
	{
		mmu_ = mmu;
	}
	void reset();
	void reset(HardwareMode mode);
	// Execute one instruction (including interrupt service). Returns
	// M-cycles elapsed (1 M-cycle = 4 T-cycles).
	u32 step();
	// Called by the MMU when a write to $FF46 starts an OAM DMA.
	void add_dma_stall(u32 mcycles)
	{
		dma_stall_ += mcycles;
	}
	u32 dma_stall() const
	{
		return dma_stall_;
	}

	// Per-M-cycle hook. The emulator installs a callback that advances the
	// timer/PPU/APU by one M-cycle; the CPU then drives them from the
	// instruction's own shape - one M-cycle per byte fetched (so the operand
	// reads and the IO writes land on the M-cycle hardware uses) plus the
	// read/write split of read-modify-write instructions. Without a hook the
	// core behaves exactly as before (callers bill step()'s return value).
	// Blargg mem_timing measures this placement against TIMA.
	using CycleHook = void (*)(void *ctx);
	void set_cycle_hook(CycleHook hook, void *ctx)
	{
		cycle_hook_ = hook;
		cycle_ctx_ = ctx;
	}

	Registers regs;
	bool ime = false;

	// ---- GBC-Ready Rule 1: clock speed is a variable, never hardcoded ----
	// https://gbdev.io/pandocs/CGB_Registers.html#ff4d--key1-cgb-mode-only-prepare-speed-switch
	// DMG always runs at kDefaultDmgClockHz. GBC toggles this via KEY1.
	// All cycle timing must derive from clock_speed_hz.
	// GBC: this will branch on double_speed mode (KEY1 register).
	u32 clock_speed_hz = kDefaultDmgClockHz;
	bool double_speed = false; // GBC only. Always false in DMG mode (Rule 9).

	bool halted = false;
	bool stopped = false;
	// CGB speed-switch stall: M-cycles remaining of the 2050 M-cycle freeze
	// after STOP (Pan Docs KEY1). Test-visible; drained by step().
	u32 speed_stall_ = 0;
	// OAM DMA: the CPU is off the bus for 160 M-cycles from the $FF46 write
	// (Pan Docs "OAM DMA Transfer": 160 reads x 4 T-cycles). Unlike the STOP
	// speed switch this does *not* freeze DIV - the timer and PPU keep running.
	u32 dma_stall_ = 0;
	u64 cycles = 0; // Total M-cycles executed.
	u32 cycles_last_instruction = 0;

	void request_interrupt(u8 interrupt)
	{
		(void) interrupt; /* handled via IF in MMU */
	}
	void enable_ime()
	{
		ime = true;
	}
	void disable_ime()
	{
		ime = false;
	}

	// Savestate. `ei_delay_` and `halt_bug_` are not registers but change
	// what the NEXT instruction does, so leaving them out of a state turns
	// a reload into a subtly different machine; they are saved with the
	// visible registers for that reason.
	void save(StateBuffer &buf) const;
	void load(StateBuffer &buf);

  private:
	MMU *mmu_ = nullptr;
	bool ei_delay_ = false; // EI takes effect after the following instruction.
	// HALT bug: HALT with IME=0 and a pending interrupt skips the next
	// PC increment (next opcode byte is read twice).
	// https://gbdev.io/pandocs/halt.html#halt-bug
	bool halt_bug_ = false;

	u8 fetch();
	u16 fetch16();
	u8 read8(u16 addr);
	void write8(u16 addr, u8 value);
	// Read-modify-write write-back: hardware reads on the second-to-last
	// M-cycle and writes on the last, and a register target (TIMA, an LCD
	// register) can change in between, so the write-back needs its own
	// M-cycle rather than sharing the read's.
	void rmw_write8(u16 addr, u8 value);
	void tick(u32 mcycles = 1);
	void bill_remaining();

	CycleHook cycle_hook_ = nullptr;
	void *cycle_ctx_ = nullptr;
	u32 billed_ = 0; // M-cycles already pushed through the hook this step.
	// CGB double speed (KEY1): the CPU runs at 2x while the PPU/APU stay on
	// the original clock, so the hook only sees every other M-cycle. The timer
	// keeps the full CPU rate and compensates internally
	// (Timer::set_double_speed). Serialized with the rest.
	u8 double_speed_phase_ = 0;

	void bios_swi(u8 n);
	void execute(u8 opcode);
	void execute_cb(u8 opcode);
	void service_interrupts();

	void push16(u16 value);
	// POP rp is one of the instructions that triggers the DMG OAM bug, with SP
	// after the first increment (Blargg's `LD SP,$FDFF : POP BC` case only
	// fires if the address is the post-increment $FE00). RET/RETI must not.
	u16 pop16(bool oam_bug = false);
	// DMG OAM corruption (Blargg "oam_bug"): an instruction whose internal
	// 16-bit increment/decrement performs a bus write can land in the PPU's
	// OAM window with a register's value as the address. `addr` is the value on
	// the bus *before* the change, which is what the readme's examples need
	// (`DEC DE` with DE=$FE00 triggers, so it cannot be the decremented value).
	// The write happens at the same point in the instruction as the real
	// accesses around it, so the trigger goes exactly there.
	void oam_bug_write(u16 addr);

	// ---- Opcode groups (kept separate for readability, one group per set) ----
	// Load group
	void op_ld_r_r(u8 &dst, u8 src);
	void op_ld_r_n(u8 &dst);
	void op_ld_r_hl(u8 &dst);
	void op_ld_hl_r(u8 src);
	// ALU group
	void add_a(u8 value);
	void adc_a(u8 value);
	void sub_a(u8 value);
	void sbc_a(u8 value);
	void and_a(u8 value);
	void or_a(u8 value);
	void xor_a(u8 value);
	void cp_a(u8 value);
	void inc_8(u8 &reg);
	void dec_8(u8 &reg);
	void inc_hl_mem();
	void dec_hl_mem();
	void add_hl(u16 value);
	void add_sp_e(i8 value);
	// Misc ALU / flags
	void daa();
	void cpl();
	void scf();
	void ccf();
	// Rotates
	void rlca();
	void rrca();
	void rla();
	void rra();
	// CB helpers
	u8 op_rlc(u8 v);
	u8 op_rrc(u8 v);
	u8 op_rl(u8 v);
	u8 op_rr(u8 v);
	u8 op_sla(u8 v);
	u8 op_sra(u8 v);
	u8 op_swap(u8 v);
	u8 op_srl(u8 v);
	void op_bit(u8 bit, u8 v);
	// Dispatch tables: the ALU group (ADD ADC SUB SBC AND XOR OR CP) and the
	// CB rotate/shift group (RLC RRC RL RR SLA SRA SWAP SRL) are both
	// selected by the y field. Same callees as the switches they replace.
	using AluFn = void (CPU::*)(u8);
	static constexpr AluFn kAlu[8] = {&CPU::add_a, &CPU::adc_a, &CPU::sub_a, &CPU::sbc_a,
									  &CPU::and_a, &CPU::xor_a, &CPU::or_a,  &CPU::cp_a};
	using RotFn = u8 (CPU::*)(u8);
	static constexpr RotFn kRot[8] = {&CPU::op_rlc, &CPU::op_rrc, &CPU::op_rl,   &CPU::op_rr,
									  &CPU::op_sla, &CPU::op_sra, &CPU::op_swap, &CPU::op_srl};
};

} // namespace gb
