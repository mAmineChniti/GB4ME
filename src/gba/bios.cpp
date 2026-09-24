// HLE BIOS core: reset, startup, SWI dispatcher, IntrWait wait logic.
// Routine groups live in bios_system/memory/math/decomp/affine.cpp.
// Contracts: GBATEK "BIOS Functions"; mGBA bios.c observed behavior.

#include "gba/bios.h"
#include "gba/bus.h"
#include "gba/cpu.h"

namespace gba
{

HleBios::HleBios()
{
	reset();
}

void HleBios::reset()
{
	waiting_ = false;
	wait_mask_ = 0;
	wait_any_ = false;
	reboot_requested_ = false;
}

void HleBios::save(StateBuffer &out) const
{
	out.write(waiting_);
	out.write(wait_mask_);
	out.write(wait_any_);
	out.write(reboot_requested_);
}

void HleBios::load(const StateBuffer &in)
{
	in.read(waiting_);
	in.read(wait_mask_);
	in.read(wait_any_);
	in.read(reboot_requested_);
}

void HleBios::installVectors(GbaBus &bus)
{
	// HLE exception vectors + IRQ trampoline, following the GBATEK
	// "BIOS Interrupt handling" protocol exactly (same instruction
	// sequence any correct implementation converges on; HLE addresses).
	// Bytes, not behavior, differ from any other BIOS — this table is the
	// minimum executable surface games can observe (vectors + IRQ entry).
	auto put32 = [&](u32 off, u32 word) {
		bus.writeBios(off, static_cast<u8>(word & 0xFFu));
		bus.writeBios(off + 1, static_cast<u8>((word >> 8) & 0xFFu));
		bus.writeBios(off + 2, static_cast<u8>((word >> 16) & 0xFFu));
		bus.writeBios(off + 3, static_cast<u8>(word >> 24));
	};
	constexpr u32 kLoop = 0xEAFFFFFEu; // B . (deterministic hang).
	put32(0x00, kLoop);                // Reset (HLE boots via applyStartup).
	put32(0x04, kLoop);                // Undefined (native trap instead).
	put32(0x08, kLoop);                // SWI (native dispatch instead).
	put32(0x0C, kLoop);                // Prefetch Abort (no MMU on GBA).
	put32(0x10, kLoop);                // Data Abort (no MMU on GBA).
	put32(0x14, kLoop);                // 26-bit overflow (not on 32-bit).
	put32(0x18, 0xEA000042u);          // IRQ: B 0x128.
	put32(0x1C, kLoop);                // FIQ (unwired on retail GBA).
	put32(0x128, 0xE92D500Fu);         // STMFD SP!, {R0-R3, R12, LR}.
	put32(0x12C, 0xE3A00301u);         // MOV R0, #0x04000000.
	put32(0x130, 0xE28FE000u);         // ADD LR, PC, #0 (retadr = 0x138).
	put32(0x134, 0xE510F004u);         // LDR PC, [R0, #-4] (=[0x3007FFC]).
	put32(0x138, 0xE8BD500Fu);         // LDMFD SP!, {R0-R3, R12, LR}.
	put32(0x13C, 0xE25EF004u);         // SUBS PC, LR, #4 (IRQ return).
}

void HleBios::applyStartup(Arm7Tdmi &cpu, GbaBus &bus)
{
	installVectors(bus);
	// Observable post-BIOS state (GBATEK + mGBA GBAReset flow):
	// - R0-R12 zeroed, banked SPs at their BIOS areas, System mode,
	//   ARM state, IRQs masked (games enable them via IME).
	// - [0x3007E00..0x3007FFF] zeroed (stacks + BIOS flags).
	// - IE/IF/IME cleared; POSTFLG boot bit clear.
	for (u8 i = 0; i < 13; i++)
		cpu.setReg(i, 0);
	cpu.switchMode(CpuMode::Supervisor);
	cpu.setReg(13, kSpSvc);
	cpu.setSpsr(0);
	cpu.switchMode(CpuMode::Irq);
	cpu.setReg(13, kSpIrq);
	cpu.setSpsr(0);
	cpu.switchMode(CpuMode::System);
	cpu.setReg(13, kSpUsr);
	cpu.setReg(14, 0);
	// CPSR after BIOS hand-off: System mode, ARM state, I and F clear
	// (mGBA ARMSetPrivilegeMode + ARMReset leave MODE_SYSTEM = 1Fh; the
	// hardware IRQ gate is IME, which stays 0 until software enables it).
	cpu.setCpsr(0x0000001Fu);
	cpu.setState(CpuState::Arm);
	for (u32 a = kClearBase; a < 0x03008000u; a++)
		bus.write8(a, 0);
	bus.write16(0x04000200, 0); // IE.
	bus.write16(0x04000202, 0); // IF.
	// IME starts cleared (hardware/mGBA: I/O reset zeroes it; games enable
	// it explicitly, and IntrWait/Halt set it when they wait).
	bus.write16(0x04000208, 0); // IME = 0.
	// POSTFLG: genuine BIOS initializes this to 01h after initial reset
	// (GBATEK "4000300h - POSTFLG"; mGBA gba.c sets POSTFLG = 1).
	bus.write8(0x04000300, 1); // POSTFLG: further-boot flag set.
	// Enter the ROM at its header branch (32-bit ARM B at 0x08000000).
	// Fallback to the base address for malformed images (documented).
	const u32 entry = bus.read32(0x08000000);
	u32 target = 0x08000000;
	if ((entry & 0x0F000000u) == 0x0A000000u) {
		u32 off = entry & 0xFFFFFFu;
		if (off & 0x800000u)
			off |= 0xFF000000u; // Sign-extend 24.
		target = 0x08000008u + (off << 2);
	}
	cpu.setPc(target);
}

HleBios::Outcome HleBios::handleSwi(u8 num, Arm7Tdmi &cpu, GbaBus &bus)
{
	// Number-group routing. The service groups must be tested before the
	// low-number range, otherwise 06h-0Ch (Div..CpuFastSet) would be
	// swallowed by system() and never reach math()/memory():
	//   00h-05h system   (SoftReset..VBlankIntrWait)
	//   06h-0Ah math     (Div/DivArm/Sqrt/ArcTan/ArcTan2)
	//   0Bh-0Ch memory   (CpuSet/CpuFastSet)
	//   0Dh     system   (GetBiosChecksum)
	//   0Eh-0Fh affine   (BgAffineSet/ObjAffineSet)
	//   10h-18h decomp   (BitUnPack/LZ77/Huffman/RL/Diff filters)
	//   19h+    system   (SoundBias, sound driver, MultiBoot, stubs)
	if ((num >= kDiv && num <= kArcTan2) || num == kMidiKey2Freq)
		return math(num, cpu, bus);
	if (num == kCpuSet || num == kCpuFastSet)
		return memory(num, cpu, bus);
	if (num == kBitUnPack || (num >= kLz77UnCompWram && num <= kDiff16bitUnFilter)) {
		return decomp(num, cpu, bus);
	}
	if (num == kBgAffineSet || num == kObjAffineSet)
		return affine(num, cpu, bus);
	return system(num, cpu, bus); // Soft/Hard reset, waits, checksum, sound, stubs.
}

void HleBios::intrWait(u32 r0, u32 r1, Arm7Tdmi &cpu, GbaBus &bus)
{
	// GBATEK IntrWait: force IME=1; r0=0 returns at once if an old flag is
	// set (clearing it in [0x3007FF8]), r0=1 discards old flags first.
	// CPSR.I and IME are don't-care for the wait itself.
	bus.write16(0x04000208, 1); // IME.
	
	const u16 mask = static_cast<u16>(r1 & 0xFFFFu);
	u16 flags = bus.read16(kIrqFlagsAddr);
	if (r0 == 0) {
		if ((flags & mask) != 0) {
			bus.write16(kIrqFlagsAddr, flags & ~mask);
			return; // Old flag satisfied; no halt.
		}
	} else {
		bus.write16(kIrqFlagsAddr, flags & ~mask);
	}
	waiting_ = true;
	wait_mask_ = mask;
	// IntrWait with a zero mask is defined to wait for ANY enabled IRQ.
	// Previously wait_mask_ was set to 0 and wait_any_ stayed false, so
	// `(fired & wait_mask_)` could never be non-zero and the call hung
	// forever.
	wait_any_ = (mask == 0);
	cpu.halt();
}

bool HleBios::pollWait(Arm7Tdmi &cpu, GbaBus &bus)
{
	if (!waiting_)
		return false;
	const u16 fired = bus.read16(0x04000200) & bus.read16(0x04000202); // IE & IF.
	if (wait_any_) {
		if (fired != 0) {
			waiting_ = false;
			cpu.wake();
			return true;
		}
		return false;
	}
	if ((fired & wait_mask_) != 0) {
		const u16 flags = bus.read16(kIrqFlagsAddr);
		bus.write16(kIrqFlagsAddr, flags & ~wait_mask_);
		waiting_ = false;
		cpu.wake();
		return true;
	}
	return false;
}

} // namespace gba
