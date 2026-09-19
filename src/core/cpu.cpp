// LR35902 CPU core.
// Structure follows the Game Boy Programming Manual decode blocks
// (x = opcode>>6, y = (opcode>>3)&7, z = opcode&7, p = y>>1, q = y&1),
// which groups implementations by category instead of one flat 256-case
// match. See also https://github.com/aquova/gb-book and the generated
// tables in https://github.com/meganesu/generate-gb-opcodes (cycle counts
// cross-checked against that project's per-mnemonic data).
//
// https://gbdev.io/pandocs/CPU_Instruction_Set.html
// https://gbdev.io/pandocs/Interrupts.html
// All cycle counts below are M-cycles (1 M-cycle = 4 T-cycles).

#include "gb/cpu.h"
#include "gb/mmu.h"

namespace gb {

namespace {

// Register index tables for the y/z decode fields.
// Order: B, C, D, E, H, L, (HL), A.
constexpr u8 kUnused = 0xFF;

}  // namespace

CPU::CPU() {
    reset();
}

void CPU::reset() {
    reset(HardwareMode::DMG);
}

void CPU::reset(HardwareMode mode) {
    // Post-boot-ROM state without executing boot ROM — freebios/direct boot.
    // https://gbdev.io/pandocs/Power_Up_Sequence.html
    // SameBoy bootroms.c direct-boot tables.
    if (is_cgb_mode(mode)) {
        regs.set_af(0x1180);
        regs.set_bc(0x0000);
        regs.set_de(0xFF56);
        regs.set_hl(0x000D);
    } else {
        regs.set_af(0x01B0);
        regs.set_bc(0x0013);
        regs.set_de(0x00D8);
        regs.set_hl(0x014D);
    }
    regs.sp = 0xFFFE;
    regs.pc = 0x0100;
    ime = false;
    ei_delay_ = false;
    halted = false;
    stopped = false;
    cycles = 0;
    cycles_last_instruction = 0;
    // GBC: clock_speed_hz switches to 2x in double-speed mode (KEY1).
    clock_speed_hz = kDefaultDmgClockHz;
    double_speed = false;
}

u8 CPU::fetch() {
    return mmu_->read(regs.pc++);
}

u16 CPU::fetch16() {
    u8 lo = fetch();
    u8 hi = fetch();
    return make_u16(hi, lo);
}

u8 CPU::read8(u16 addr) {
    return mmu_->read(addr);
}

void CPU::write8(u16 addr, u8 value) {
    mmu_->write(addr, value);
}

void CPU::push16(u16 value) {
    regs.sp -= 2;
    write8(regs.sp, lo_byte(value));
    write8(regs.sp + 1, hi_byte(value));
}

u16 CPU::pop16() {
    u8 lo = read8(regs.sp);
    u8 hi = read8(regs.sp + 1);
    regs.sp += 2;
    return make_u16(hi, lo);
}

// ---------------- ALU helpers (8-bit) ----------------

void CPU::add_a(u8 value) {
    u16 res = static_cast<u16>(regs.a) + value;
    regs.set_z(lo_byte(res) == 0);
    regs.set_n(false);
    regs.set_h(((regs.a & 0xF) + (value & 0xF)) > 0xF);
    regs.set_c(res > 0xFF);
    regs.a = lo_byte(res);
}

void CPU::adc_a(u8 value) {
    u8 c = regs.get_c() ? 1 : 0;
    u16 res = static_cast<u16>(regs.a) + value + c;
    regs.set_z(lo_byte(res) == 0);
    regs.set_n(false);
    regs.set_h(((regs.a & 0xF) + (value & 0xF) + c) > 0xF);
    regs.set_c(res > 0xFF);
    regs.a = lo_byte(res);
}

void CPU::sub_a(u8 value) {
    u16 res = static_cast<u16>(regs.a) - value;
    regs.set_z(lo_byte(res) == 0);
    regs.set_n(true);
    regs.set_h((regs.a & 0xF) < (value & 0xF));
    regs.set_c(regs.a < value);
    regs.a = lo_byte(res);
}

void CPU::sbc_a(u8 value) {
    u8 c = regs.get_c() ? 1 : 0;
    u16 res = static_cast<u16>(regs.a) - value - c;
    regs.set_z(lo_byte(res) == 0);
    regs.set_n(true);
    regs.set_h((regs.a & 0xF) < ((value & 0xF) + c));
    regs.set_c(static_cast<u16>(regs.a) < static_cast<u16>(value) + c);
    regs.a = lo_byte(res);
}

void CPU::and_a(u8 value) {
    regs.a &= value;
    regs.set_z(regs.a == 0);
    regs.set_n(false);
    regs.set_h(true);
    regs.set_c(false);
}

void CPU::or_a(u8 value) {
    regs.a |= value;
    regs.set_z(regs.a == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(false);
}

void CPU::xor_a(u8 value) {
    regs.a ^= value;
    regs.set_z(regs.a == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(false);
}

void CPU::cp_a(u8 value) {
    u8 a = regs.a;
    sub_a(value);
    regs.a = a;  // CP sets flags like SUB but keeps A.
}

void CPU::inc_8(u8& reg) {
    regs.set_h((reg & 0xF) == 0xF);
    reg++;
    regs.set_z(reg == 0);
    regs.set_n(false);
}

void CPU::dec_8(u8& reg) {
    regs.set_h((reg & 0xF) == 0x0);
    reg--;
    regs.set_z(reg == 0);
    regs.set_n(true);
}

void CPU::inc_hl_mem() {
    u8 v = read8(regs.hl());
    regs.set_h((v & 0xF) == 0xF);
    v++;
    write8(regs.hl(), v);
    regs.set_z(v == 0);
    regs.set_n(false);
}

void CPU::dec_hl_mem() {
    u8 v = read8(regs.hl());
    regs.set_h((v & 0xF) == 0x0);
    v--;
    write8(regs.hl(), v);
    regs.set_z(v == 0);
    regs.set_n(true);
}

void CPU::add_hl(u16 value) {
    u32 res = static_cast<u32>(regs.hl()) + value;
    regs.set_n(false);
    regs.set_h(((regs.hl() & 0xFFF) + (value & 0xFFF)) > 0xFFF);
    regs.set_c(res > 0xFFFF);
    regs.set_hl(static_cast<u16>(res));
}

// ADD SP,e / LD HL,SP+e share flag logic: Z=0, N=0, H/C from low nibble/byte.
// https://gbdev.io/pandocs/CPU_Instruction_Set.html (see ADD SP,s8 notes)
void CPU::add_sp_e(i8 value) {
    u16 e = static_cast<u16>(static_cast<i16>(value));
    u16 res = regs.sp + e;
    regs.set_z(false);
    regs.set_n(false);
    regs.set_h(((regs.sp ^ e ^ res) & 0x10) != 0);
    regs.set_c(((regs.sp ^ e ^ res) & 0x100) != 0);
    regs.sp = res;
}

void CPU::daa() {
    u8 a = regs.a;
    u8 adjust = 0;
    bool carry = regs.get_c();
    if (!regs.get_n()) {
        if (regs.get_h() || (a & 0xF) > 0x9) adjust |= 0x06;
        if (regs.get_c() || a > 0x99) {
            adjust |= 0x60;
            carry = true;
        }
        a += adjust;
    } else {
        if (regs.get_h()) adjust |= 0x06;
        if (regs.get_c()) adjust |= 0x60;
        a -= adjust;
    }
    regs.a = a;
    regs.set_z(a == 0);
    regs.set_h(false);
    regs.set_c(carry);
}

void CPU::cpl() {
    regs.a = ~regs.a;
    regs.set_n(true);
    regs.set_h(true);
}

void CPU::scf() {
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(true);
}

void CPU::ccf() {
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(!regs.get_c());
}

void CPU::rlca() {
    bool c = (regs.a & 0x80) != 0;
    regs.a = static_cast<u8>((regs.a << 1) | (c ? 1 : 0));
    regs.set_z(false);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
}

void CPU::rrca() {
    bool c = (regs.a & 0x01) != 0;
    regs.a = static_cast<u8>((regs.a >> 1) | (c ? 0x80 : 0));
    regs.set_z(false);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
}

void CPU::rla() {
    bool c = (regs.a & 0x80) != 0;
    regs.a = static_cast<u8>((regs.a << 1) | (regs.get_c() ? 1 : 0));
    regs.set_z(false);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
}

void CPU::rra() {
    bool c = (regs.a & 0x01) != 0;
    regs.a = static_cast<u8>((regs.a >> 1) | (regs.get_c() ? 0x80 : 0));
    regs.set_z(false);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
}

// ---------------- CB-prefix helpers ----------------

u8 CPU::op_rlc(u8 v) {
    bool c = (v & 0x80) != 0;
    v = static_cast<u8>((v << 1) | (c ? 1 : 0));
    regs.set_z(v == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
    return v;
}

u8 CPU::op_rrc(u8 v) {
    bool c = (v & 0x01) != 0;
    v = static_cast<u8>((v >> 1) | (c ? 0x80 : 0));
    regs.set_z(v == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
    return v;
}

u8 CPU::op_rl(u8 v) {
    bool c = (v & 0x80) != 0;
    v = static_cast<u8>((v << 1) | (regs.get_c() ? 1 : 0));
    regs.set_z(v == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
    return v;
}

u8 CPU::op_rr(u8 v) {
    bool c = (v & 0x01) != 0;
    v = static_cast<u8>((v >> 1) | (regs.get_c() ? 0x80 : 0));
    regs.set_z(v == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
    return v;
}

u8 CPU::op_sla(u8 v) {
    bool c = (v & 0x80) != 0;
    v = static_cast<u8>(v << 1);
    regs.set_z(v == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
    return v;
}

u8 CPU::op_sra(u8 v) {
    bool c = (v & 0x01) != 0;
    v = static_cast<u8>((v >> 1) | (v & 0x80));
    regs.set_z(v == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
    return v;
}

u8 CPU::op_swap(u8 v) {
    v = static_cast<u8>(((v & 0xF) << 4) | ((v & 0xF0) >> 4));
    regs.set_z(v == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(false);
    return v;
}

u8 CPU::op_srl(u8 v) {
    bool c = (v & 0x01) != 0;
    v = static_cast<u8>(v >> 1);
    regs.set_z(v == 0);
    regs.set_n(false);
    regs.set_h(false);
    regs.set_c(c);
    return v;
}

void CPU::op_bit(u8 bit, u8 v) {
    regs.set_z(((v >> bit) & 1) == 0);
    regs.set_n(false);
    regs.set_h(true);
}

// ---------------- Load-group helpers ----------------

void CPU::op_ld_r_r(u8& dst, u8 src) {
    dst = src;
}

void CPU::op_ld_r_n(u8& dst) {
    dst = fetch();
}

void CPU::op_ld_r_hl(u8& dst) {
    dst = read8(regs.hl());
}

void CPU::op_ld_hl_r(u8 src) {
    write8(regs.hl(), src);
}

// ---------------- Interrupt service ----------------

void CPU::service_interrupts() {
    constexpr u16 kVectors[5] = {0x40, 0x48, 0x50, 0x58, 0x60};
    u8 pending = mmu_->read_ie() & (mmu_->read_if() & 0x1F);
    if (!ime || pending == 0) return;
    ime = false;
    for (u8 b = 0; b < 5; b++) {
        if (pending & (1u << b)) {
            mmu_->write_if(mmu_->read_if() & ~(1u << b));
            push16(regs.pc);
            regs.pc = kVectors[b];
            cycles_last_instruction = 5;
            cycles += 5;
            return;
        }
    }
}

// ---------------- Main step ----------------

u32 CPU::step() {
    // EI enables interrupts after the following instruction.
    if (ei_delay_) {
        ime = true;
        ei_delay_ = false;
    }

    if (halted) {
        u8 pending = mmu_->read_ie() & (mmu_->read_if() & 0x1F);
        if (pending == 0) {
            cycles += 1;  // Stay halted, burn 1 M-cycle.
            cycles_last_instruction = 1;
            return 1;
        }
        halted = false;
        // NOTE: HALT bug (no IME + pending) quirks not emulated; DMG games
        // and Blargg tests do not depend on it.
        if (!ime) {
            u8 opcode = fetch();
            execute(opcode);
            cycles += cycles_last_instruction;
            return cycles_last_instruction;
        }
    }

    // Serve interrupts before the next instruction (5 M-cycles).
    {
        u8 pending = mmu_->read_ie() & (mmu_->read_if() & 0x1F);
        if (ime && pending != 0) {
            service_interrupts();
            return cycles_last_instruction;
        }
    }

    u8 opcode = fetch();
    execute(opcode);
    cycles += cycles_last_instruction;
    return cycles_last_instruction;
}

namespace {

// Condition table: NZ, Z, NC, C.
bool check_cc(u8 cc, const Registers& regs) {
    switch (cc) {
        case 0: return !regs.get_z();
        case 1: return regs.get_z();
        case 2: return !regs.get_c();
        case 3: return regs.get_c();
        default: return false;
    }
}

}  // namespace

void CPU::execute(u8 opcode) {
    const u8 x = opcode >> 6;
    const u8 y = (opcode >> 3) & 0x7;
    const u8 z = opcode & 0x7;
    const u8 p = (y >> 1) & 0x3;
    const u8 q = y & 0x1;

    // Mutable register file view for y/z-indexed access.
    u8* r8[8] = {&regs.b, &regs.c, &regs.d, &regs.e,
                 &regs.h, &regs.l, nullptr, &regs.a};

    switch (x) {
        case 0: {
            switch (z) {
                case 0: {  // Relative jumps / misc.
                    switch (y) {
                        case 0: cycles_last_instruction = 1; return;  // NOP
                        case 1: {  // LD (nn),SP (5)
                            u16 nn = fetch16();
                            write8(nn, lo_byte(regs.sp));
                            write8(nn + 1, hi_byte(regs.sp));
                            cycles_last_instruction = 5;
                            return;
                        }
                        case 2: { // STOP (2-byte) — also KEY1 speed switch on CGB
                            // https://gbdev.io/pandocs/CGB_Registers.html#ff4d--key1-cgb-mode-only-prepare-speed-switch
                            // The second byte is 0x00 by convention. In CGB mode with
                            // KEY1 bit0 set, STOP toggles double_speed instead of halting.
                            // GBC: this will branch on double_speed mode (KEY1 register).
                            u8 stop_param = fetch(); (void)stop_param; // consume 0x00
                            if (mmu_ && mmu_->is_cgb() && mmu_->prepare_speed_switch()) {
                                mmu_->do_speed_switch(this);
                                cycles_last_instruction = 1;
                                return;
                            }
                            // DMG STOP or CGB STOP without switch: halt CPU until button.
                            // Minimal model: treat as NOP (Blargg tests use STOP only for speed switch).
                            // To avoid soft-lock, we just skip it; joypad interrupt can still wake HALT.
                            stopped = true;
                            cycles_last_instruction = 1;
                            return;
                        }
                        case 3: {  // JR e (3)
                            i8 e = static_cast<i8>(fetch());
                            regs.pc = static_cast<u16>(regs.pc + e);
                            cycles_last_instruction = 3;
                            return;
                        }
                        default: {  // JR cc,e (3/2)
                            i8 e = static_cast<i8>(fetch());
                            if (check_cc(y - 4, regs)) {
                                regs.pc = static_cast<u16>(regs.pc + e);
                                cycles_last_instruction = 3;
                            } else {
                                cycles_last_instruction = 2;
                            }
                            return;
                        }
                    }
                }
                case 1: {  // 16-bit LD / ADD HL.
                    if (q == 0) {  // LD rr,nn (3)
                        u16 nn = fetch16();
                        switch (p) {
                            case 0: regs.set_bc(nn); break;
                            case 1: regs.set_de(nn); break;
                            case 2: regs.set_hl(nn); break;
                            case 3: regs.sp = nn; break;
                        }
                        cycles_last_instruction = 3;
                    } else {  // ADD HL,rr (2)
                        switch (p) {
                            case 0: add_hl(regs.bc()); break;
                            case 1: add_hl(regs.de()); break;
                            case 2: add_hl(regs.hl()); break;
                            case 3: add_hl(regs.sp); break;
                        }
                        cycles_last_instruction = 2;
                    }
                    return;
                }
                case 2: {  // LD (rr),A / LD A,(rr), incl. HLI/HLD.
                    if (q == 0) {
                        switch (p) {
                            case 0: write8(regs.bc(), regs.a); break;              // LD (BC),A
                            case 1: write8(regs.de(), regs.a); break;              // LD (DE),A
                            case 2: write8(regs.hl(), regs.a); regs.set_hl(regs.hl() + 1); break;  // LD (HL+),A
                            case 3: write8(regs.hl(), regs.a); regs.set_hl(regs.hl() - 1); break;  // LD (HL-),A
                        }
                        cycles_last_instruction = 2;
                    } else {
                        switch (p) {
                            case 0: regs.a = read8(regs.bc()); break;              // LD A,(BC)
                            case 1: regs.a = read8(regs.de()); break;              // LD A,(DE)
                            case 2: regs.a = read8(regs.hl()); regs.set_hl(regs.hl() + 1); break;  // LD A,(HL+)
                            case 3: regs.a = read8(regs.hl()); regs.set_hl(regs.hl() - 1); break;  // LD A,(HL-)
                        }
                        cycles_last_instruction = 2;
                    }
                    return;
                }
                case 3: {  // INC rr / DEC rr (2).
                    u16 v;
                    switch (p) {
                        case 0: v = regs.bc(); break;
                        case 1: v = regs.de(); break;
                        case 2: v = regs.hl(); break;
                        default: v = regs.sp; break;
                    }
                    v = (q == 0) ? v + 1 : v - 1;
                    switch (p) {
                        case 0: regs.set_bc(v); break;
                        case 1: regs.set_de(v); break;
                        case 2: regs.set_hl(v); break;
                        default: regs.sp = v; break;
                    }
                    cycles_last_instruction = 2;
                    return;
                }
                case 4: {  // INC r[y] (1) / INC (HL) (3).
                    if (y == 6) {
                        inc_hl_mem();
                        cycles_last_instruction = 3;
                    } else {
                        inc_8(*r8[y]);
                        cycles_last_instruction = 1;
                    }
                    return;
                }
                case 5: {  // DEC r[y] (1) / DEC (HL) (3).
                    if (y == 6) {
                        dec_hl_mem();
                        cycles_last_instruction = 3;
                    } else {
                        dec_8(*r8[y]);
                        cycles_last_instruction = 1;
                    }
                    return;
                }
                case 6: {  // LD r[y],n (2) / LD (HL),n (3).
                    u8 n = fetch();
                    if (y == 6) {
                        write8(regs.hl(), n);
                        cycles_last_instruction = 3;
                    } else {
                        *r8[y] = n;
                        cycles_last_instruction = 2;
                    }
                    return;
                }
                default: {  // z == 7: single-byte ALU/misc.
                    switch (y) {
                        case 0: rlca(); break;
                        case 1: rrca(); break;
                        case 2: rla(); break;
                        case 3: rra(); break;
                        case 4: daa(); break;
                        case 5: cpl(); break;
                        case 6: scf(); break;
                        case 7: ccf(); break;
                    }
                    cycles_last_instruction = 1;
                    return;
                }
            }
        }
        case 1: {
            if (opcode == 0x76) {  // HALT
                halted = true;
                cycles_last_instruction = 1;
                return;
            }
            // LD r[y],r[z] (1; 2 if (HL) involved).
            if (y == 6) {
                op_ld_hl_r(*r8[z]);
                cycles_last_instruction = 2;
            } else if (z == 6) {
                op_ld_r_hl(*r8[y]);
                cycles_last_instruction = 2;
            } else {
                op_ld_r_r(*r8[y], *r8[z]);
                cycles_last_instruction = 1;
            }
            return;
        }
        case 2: {
            // ALU[y] r[z]: ADD ADC SUB SBC AND XOR OR CP.
            u8 v = (z == 6) ? read8(regs.hl()) : *r8[z];
            switch (y) {
                case 0: add_a(v); break;
                case 1: adc_a(v); break;
                case 2: sub_a(v); break;
                case 3: sbc_a(v); break;
                case 4: and_a(v); break;
                case 5: xor_a(v); break;
                case 6: or_a(v); break;
                case 7: cp_a(v); break;
            }
            cycles_last_instruction = (z == 6) ? 2 : 1;
            return;
        }
        default: {  // x == 3: control / 16-bit misc.
            switch (z) {
                case 0: {
                    switch (y) {
                        case 0: case 1: case 2: case 3: {  // RET cc (5/2)
                            if (check_cc(y, regs)) {
                                regs.pc = pop16();
                                cycles_last_instruction = 5;
                            } else {
                                cycles_last_instruction = 2;
                            }
                            return;
                        }
                        case 4: {  // LDH (n),A (3)
                            u8 n = fetch();
                            write8(make_u16(0xFF, n), regs.a);
                            cycles_last_instruction = 3;
                            return;
                        }
                        case 5: {  // ADD SP,e (4)
                            i8 e = static_cast<i8>(fetch());
                            // add_sp_e overwrites SP; flags per spec.
                            {
                                u16 e16 = static_cast<u16>(static_cast<i16>(e));
                                u16 res = regs.sp + e16;
                                regs.set_z(false);
                                regs.set_n(false);
                                regs.set_h(((regs.sp ^ e16 ^ res) & 0x10) != 0);
                                regs.set_c(((regs.sp ^ e16 ^ res) & 0x100) != 0);
                                regs.sp = res;
                            }
                            cycles_last_instruction = 4;
                            return;
                        }
                        case 6: {  // LDH A,(n) (3)
                            u8 n = fetch();
                            regs.a = read8(make_u16(0xFF, n));
                            cycles_last_instruction = 3;
                            return;
                        }
                        default: {  // LD HL,SP+e (3)
                            i8 e = static_cast<i8>(fetch());
                            u16 e16 = static_cast<u16>(static_cast<i16>(e));
                            u16 res = regs.sp + e16;
                            regs.set_z(false);
                            regs.set_n(false);
                            regs.set_h(((regs.sp ^ e16 ^ res) & 0x10) != 0);
                            regs.set_c(((regs.sp ^ e16 ^ res) & 0x100) != 0);
                            regs.set_hl(res);
                            cycles_last_instruction = 3;
                            return;
                        }
                    }
                }
                case 1: {
                    if (q == 0) {  // POP rp[p] (3); AF masked.
                        u16 v = pop16();
                        switch (p) {
                            case 0: regs.set_bc(v); break;
                            case 1: regs.set_de(v); break;
                            case 2: regs.set_hl(v); break;
                            case 3: regs.set_af(v & 0xFFF0); break;
                        }
                        cycles_last_instruction = 3;
                        return;
                    }
                    switch (p) {
                        case 0: regs.pc = pop16(); cycles_last_instruction = 4; return;  // RET
                        case 1:  // RETI
                            regs.pc = pop16();
                            ime = true;
                            ei_delay_ = false;
                            cycles_last_instruction = 4;
                            return;
                        case 2: regs.pc = regs.hl(); cycles_last_instruction = 1; return;  // JP HL
                        default: regs.sp = regs.hl(); cycles_last_instruction = 2; return;  // LD SP,HL
                    }
                }
                case 2: {
                    switch (y) {
                        case 0: case 1: case 2: case 3: {  // JP cc,nn (4/3)
                            u16 nn = fetch16();
                            if (check_cc(y, regs)) {
                                regs.pc = nn;
                                cycles_last_instruction = 4;
                            } else {
                                cycles_last_instruction = 3;
                            }
                            return;
                        }
                        case 4:  // LD (C),A (2)
                            write8(make_u16(0xFF, regs.c), regs.a);
                            cycles_last_instruction = 2;
                            return;
                        case 5: {  // LD (nn),A (4)
                            u16 nn = fetch16();
                            write8(nn, regs.a);
                            cycles_last_instruction = 4;
                            return;
                        }
                        case 6:  // LD A,(C) (2)
                            regs.a = read8(make_u16(0xFF, regs.c));
                            cycles_last_instruction = 2;
                            return;
                        default: {  // LD A,(nn) (4)
                            u16 nn = fetch16();
                            regs.a = read8(nn);
                            cycles_last_instruction = 4;
                            return;
                        }
                    }
                }
                case 3: {
                    switch (y) {
                        case 0: {  // JP nn (4)
                            regs.pc = fetch16();
                            cycles_last_instruction = 4;
                            return;
                        }
                        case 1: {  // CB prefix (2/4 set by execute_cb)
                            u8 cb = fetch();
                            execute_cb(cb);
                            return;
                        }
                        case 6:  // DI (1)
                            ime = false;
                            ei_delay_ = false;
                            cycles_last_instruction = 1;
                            return;
                        case 7:  // EI (1, delayed)
                            ei_delay_ = true;
                            cycles_last_instruction = 1;
                            return;
                        default:  // Undefined: D3 DB DD E3 E4 EB EC ED F4 FC FD.
                            cycles_last_instruction = 1;
                            return;
                    }
                }
                case 4: {  // CALL cc,nn (6/3). y=4..7 are undefined.
                    if (y > 3) {
                        cycles_last_instruction = 1;
                        return;
                    }
                    u16 nn = fetch16();
                    if (check_cc(y, regs)) {
                        push16(regs.pc);
                        regs.pc = nn;
                        cycles_last_instruction = 6;
                    } else {
                        cycles_last_instruction = 3;
                    }
                    return;
                }
                case 5: {
                    if (q == 0) {  // PUSH rp[p] (4); AF high byte only.
                        switch (p) {
                            case 0: push16(regs.bc()); break;
                            case 1: push16(regs.de()); break;
                            case 2: push16(regs.hl()); break;
                            case 3: push16(regs.af() & 0xFFF0); break;
                        }
                        cycles_last_instruction = 4;
                        return;
                    }
                    if (p == 0) {  // CALL nn (6)
                        u16 nn = fetch16();
                        push16(regs.pc);
                        regs.pc = nn;
                        cycles_last_instruction = 6;
                        return;
                    }
                    // Undefined: DD ED FD.
                    cycles_last_instruction = 1;
                    return;
                }
                case 6: {  // ALU[y] n (2).
                    u8 n = fetch();
                    switch (y) {
                        case 0: add_a(n); break;
                        case 1: adc_a(n); break;
                        case 2: sub_a(n); break;
                        case 3: sbc_a(n); break;
                        case 4: and_a(n); break;
                        case 5: xor_a(n); break;
                        case 6: or_a(n); break;
                        case 7: cp_a(n); break;
                    }
                    cycles_last_instruction = 2;
                    return;
                }
                default: {  // RST y*8 (4).
                    push16(regs.pc);
                    regs.pc = static_cast<u16>(y * 8);
                    cycles_last_instruction = 4;
                    return;
                }
            }
        }
    }
}

void CPU::execute_cb(u8 opcode) {
    const u8 x = opcode >> 6;
    const u8 y = (opcode >> 3) & 0x7;
    const u8 z = opcode & 0x7;

    u8* r8[8] = {&regs.b, &regs.c, &regs.d, &regs.e,
                 &regs.h, &regs.l, nullptr, &regs.a};
    const bool is_hl = (z == 6);

    switch (x) {
        case 0: {  // Rotates/shifts, y selects operation.
            u8 v = is_hl ? read8(regs.hl()) : *r8[z];
            u8 r = 0;
            switch (y) {
                case 0: r = op_rlc(v); break;
                case 1: r = op_rrc(v); break;
                case 2: r = op_rl(v); break;
                case 3: r = op_rr(v); break;
                case 4: r = op_sla(v); break;
                case 5: r = op_sra(v); break;
                case 6: r = op_swap(v); break;
                default: r = op_srl(v); break;
            }
            if (is_hl) write8(regs.hl(), r);
            else *r8[z] = r;
            cycles_last_instruction = is_hl ? 4 : 2;
            return;
        }
        case 1: {  // BIT b,y (2; 3 for (HL)).
            u8 v = is_hl ? read8(regs.hl()) : *r8[z];
            op_bit(y, v);
            cycles_last_instruction = is_hl ? 3 : 2;
            return;
        }
        case 2: {  // RES b (2; 4 for (HL)).
            if (is_hl) write8(regs.hl(), read8(regs.hl()) & ~(1u << y));
            else *r8[z] &= ~(1u << y);
            cycles_last_instruction = is_hl ? 4 : 2;
            return;
        }
        default: {  // SET b (2; 4 for (HL)).
            if (is_hl) write8(regs.hl(), read8(regs.hl()) | (1u << y));
            else *r8[z] |= (1u << y);
            cycles_last_instruction = is_hl ? 4 : 2;
            return;
        }
    }
}

}  // namespace gb
