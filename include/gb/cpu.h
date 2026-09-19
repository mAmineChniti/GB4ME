#pragma once

#include "types.h"

namespace gb {

class MMU;  // Bus object. CPU must access memory ONLY via MMU::read/write.
            // https://gbdev.io/pandocs/Memory_Map.html

enum class Flag : u8 {
    Z = 7,
    N = 6,
    H = 5,
    C = 4
};

struct Registers {
    // Stored as bytes; pairs compose via accessors (layout: F is low byte of
    // AF so flag bits land at Z=7..C=4, matching hardware).
    u8 a = 0, f = 0;
    u8 b = 0, c = 0;
    u8 d = 0, e = 0;
    u8 h = 0, l = 0;
    u16 sp = 0;
    u16 pc = 0;

    u16 af() const { return make_u16(a, f & 0xF0); }
    u16 bc() const { return make_u16(b, c); }
    u16 de() const { return make_u16(d, e); }
    u16 hl() const { return make_u16(h, l); }
    void set_af(u16 v) { a = hi_byte(v); f = lo_byte(v) & 0xF0; }
    void set_bc(u16 v) { b = hi_byte(v); c = lo_byte(v); }
    void set_de(u16 v) { d = hi_byte(v); e = lo_byte(v); }
    void set_hl(u16 v) { h = hi_byte(v); l = lo_byte(v); }

    bool get_flag(Flag flag) const {
        return (f & (1 << static_cast<u8>(flag))) != 0;
    }

    void set_flag(Flag flag, bool value) {
        if (value) f |= (1 << static_cast<u8>(flag));
        else       f &= ~(1 << static_cast<u8>(flag));
    }

    void set_z(bool v) { set_flag(Flag::Z, v); }
    void set_n(bool v) { set_flag(Flag::N, v); }
    void set_h(bool v) { set_flag(Flag::H, v); }
    void set_c(bool v) { set_flag(Flag::C, v); }

    bool get_z() const { return get_flag(Flag::Z); }
    bool get_n() const { return get_flag(Flag::N); }
    bool get_c() const { return get_flag(Flag::C); }
    bool get_h() const { return get_flag(Flag::H); }
};

// LR35902 CPU core.
// https://github.com/aquova/gb-book (chapter order: CPU first)
// https://gbdev.io/pandocs/CPU_Instruction_Set.html
class CPU {
public:
    CPU();
    ~CPU() = default;

    void set_mmu(MMU* mmu) { mmu_ = mmu; }
    void reset();
    void reset(HardwareMode mode);
    // Execute one instruction (including interrupt service). Returns
    // M-cycles elapsed (1 M-cycle = 4 T-cycles).
    u32 step();

    Registers regs;
    bool ime = false;

    // ---- GBC-Ready Rule 1: clock speed is a variable, never hardcoded ----
    // https://gbdev.io/pandocs/CGB_Registers.html#ff4d--key1-cgb-mode-only-prepare-speed-switch
    // DMG always runs at kDefaultDmgClockHz. GBC toggles this via KEY1.
    // All cycle timing must derive from clock_speed_hz.
    // GBC: this will branch on double_speed mode (KEY1 register).
    u32 clock_speed_hz = kDefaultDmgClockHz;
    bool double_speed = false;  // GBC only. Always false in DMG mode (Rule 9).

    bool halted = false;
    bool stopped = false;
    u64 cycles = 0;  // Total M-cycles executed.
    u32 cycles_last_instruction = 0;

    void request_interrupt(u8 interrupt) { (void)interrupt; /* handled via IF in MMU */ }
    void enable_ime() { ime = true; }
    void disable_ime() { ime = false; }

private:
    MMU* mmu_ = nullptr;
    bool ei_delay_ = false;  // EI takes effect after the following instruction.

    u8 fetch();
    u16 fetch16();
    u8 read8(u16 addr);
    void write8(u16 addr, u8 value);

    void execute(u8 opcode);
    void execute_cb(u8 opcode);
    void service_interrupts();

    void push16(u16 value);
    u16 pop16();

    // ---- Opcode groups (kept separate for readability, one group per set) ----
    // Load group
    void op_ld_r_r(u8& dst, u8 src);
    void op_ld_r_n(u8& dst);
    void op_ld_r_hl(u8& dst);
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
    void inc_8(u8& reg);
    void dec_8(u8& reg);
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
};

} // namespace gb
