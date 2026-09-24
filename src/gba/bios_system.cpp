// HLE BIOS system services: reset, halt/wait, checksum, sound stubs.
// Contracts: GBATEK "BIOS System Calls / Halt Functions / Misc"; mGBA
// bios.c register-level behavior. I/O addresses from the GBATEK I/O map.

#include "gba/bios.h"
#include "gba/bus.h"
#include "gba/cpu.h"

namespace gba
{

namespace
{

// I/O register addresses (GBATEK "GBA I/O Map").
constexpr u32 kDispcnt = 0x04000000;
constexpr u32 kSiocnt = 0x04000128;
constexpr u32 kSiomltSend = 0x0400012A;
constexpr u32 kRcnt = 0x04000134;
constexpr u32 kJoycnt = 0x04000140;
constexpr u32 kJoyRecv = 0x04000150;
constexpr u32 kJoyTrans = 0x04000154;
constexpr u32 kSoundBias = 0x04000088;
constexpr u32 kIme = 0x04000208;
constexpr u32 kIf = 0x04000202;
constexpr u32 kIe = 0x04000200;

void write16(GbaBus &bus, u32 addr, u16 v)
{
	// Routed 16-bit access (reaches IRQ/timer/DMA units, not just the
	// backing store).
	bus.write16(addr, v);
}

void write32(GbaBus &bus, u32 addr, u32 v)
{
	bus.write32(addr, v);
}

} // namespace

HleBios::Outcome HleBios::system(u8 num, Arm7Tdmi &cpu, GbaBus &bus)
{
	switch (num) {
	case kSoftReset: {
		// GBATEK SoftReset: clear stacks/flags, init SPs, zero R0-R12
		// + LR/SPSR of svc+irq, enter System mode, BX to the flag
		// address (00h = ROM 08000000h, else RAM 02000000h), ARM state.
		// The flag byte at 03007FFAh is read BEFORE the area is cleared
		// (reference BIOS/HLE reads it into a register first; reading it
		// afterwards would always see the freshly zeroed 0).
		const u8 flag = bus.read8(kBootFlagAddr);
		for (u32 a = kClearBase; a < 0x03008000u; a++)
			bus.write8(a, 0);
		cpu.switchMode(CpuMode::Supervisor);
		cpu.setReg(13, kSpSvc);
		cpu.setSpsr(0);
		cpu.setReg(14, 0);
		cpu.switchMode(CpuMode::Irq);
		cpu.setReg(13, kSpIrq);
		cpu.setSpsr(0);
		cpu.setReg(14, 0);
		for (u8 i = 0; i < 13; i++) {
			cpu.switchMode(CpuMode::System);
			cpu.setReg(i, 0);
		}
		cpu.switchMode(CpuMode::System);
		cpu.setState(CpuState::Arm);
		cpu.setCpsr(0x0000001Fu | (1u << kFlagI) | (1u << kFlagF));
		const u32 target = (flag == 0) ? 0x08000000u : 0x02000000u;
		cpu.setReg(14, target);
		cpu.setPc(target); // BX R14 with an even address stays ARM.
		return {Result::NoReturn, 16};
	}
	case kRegisterRamReset: {
		const u32 flags = cpu.reg(0);
		// Do not touch DISPCNT - games control it themselves
		if ((flags & 0x01) != 0) {
			for (u32 a = 0x02000000u; a < 0x02040000u; a++)
				bus.write8(a, 0);
		}
		if ((flags & 0x02) != 0) {
			// IWRAM except the top 200h (stacks/flags).
			for (u32 a = 0x03000000u; a < kClearBase; a++)
				bus.write8(a, 0);
		}
		if ((flags & 0x04) != 0) {
			for (u32 a = 0x05000000u; a < 0x05000400u; a++)
				bus.write8(a, 0);
		}
		if ((flags & 0x08) != 0) {
			for (u32 a = 0x06000000u; a < 0x06018000u; a++)
				bus.write8(a, 0);
		}
		if ((flags & 0x10) != 0) {
			for (u32 a = 0x07000000u; a < 0x07000400u; a++)
				bus.write8(a, 0);
		}
		if ((flags & 0x20) != 0) { // SIO -> general-purpose mode.
			write16(bus, kSiocnt, 0x0000);
			write16(bus, kRcnt, 0x8000);
			write16(bus, kSiomltSend, 0x0000);
			write16(bus, kJoycnt, 0x0000);
			write32(bus, kJoyRecv, 0x00000000u);
			write32(bus, kJoyTrans, 0x00000000u);
		}
		if ((flags & 0x40) != 0) { // Sound registers.
			static constexpr u32 kSoundRegs[] = {
				0x04000060u, 0x04000062u, 0x04000064u, 0x04000068u, 0x0400006Cu,
				0x04000070u, 0x04000072u, 0x04000074u, 0x04000078u, 0x0400007Cu,
				0x04000080u, 0x04000082u, 0x04000084u,
			};
			for (u32 r : kSoundRegs)
				write16(bus, r, 0x0000);
			write16(bus, kSoundBias, 0x0200);
			for (u32 a = 0x04000090u; a < 0x040000A0u; a++)
				bus.write8(a, 0);
		}
		if ((flags & 0x80) != 0) {             // All other registers.
			write16(bus, 0x04000004u, 0x0000); // DISPSTAT.
			write16(bus, 0x04000006u, 0x0000); // VCOUNT.
			for (u32 r = 0x04000008u; r < 0x04000060u; r += 2) {
				// BG2PA/BG3PA reset to 0100h, rest to 0.
				const u16 v = (r == 0x04000020u || r == 0x04000030u) ? 0x0100 : 0x0000;
				write16(bus, r, v);
			}
			for (u32 r = 0x040000B0u; r < 0x04000100u; r += 2)
				write16(bus, r, 0); // DMA.
			for (u32 r = 0x04000100u; r < 0x04000110u; r += 2)
				write16(bus, r, 0);            // Timers.
			write16(bus, 0x04000200u, 0x0000); // IE.
			write16(bus, 0x04000202u, 0xFFFF); // IF (ack).
			write16(bus, 0x04000204u, 0x0000); // WAITCNT.
			write16(bus, kIme, 0x0000);        // IME.
			bus.setWaitcnt(0);
		}
		return {Result::Return, 16};
	}
	case kHalt: {
		// GBATEK: halt until (IE & IF); CPSR.I and IME are don't-care for
		// HALT itself — it does NOT enable interrupts (only IntrWait forces
		// IME=1). Registers unchanged.
		cpu.halt();
		return {Result::Return, 8};
	}
	case kStop: {
		// Very-low-power stop; wakes on joypad/cart/SIO only. Without
		// the wake-set model the wait would deadlock, so this halts like
		// Halt (documented simplification). HALT does not enable IRQs.
		cpu.halt();
		return {Result::Return, 8};
	}
	case kCustomHalt:
		// Debugger-oriented halt variant; exact parameter contract is
		// undocumented in GBATEK, so this waits like Halt (documented).
		// HALT does not enable IRQs (same as kHalt).
		cpu.halt();
		return {Result::Return, 8};
	case kIntrWait:
		intrWait(cpu.reg(0), cpu.reg(1), cpu, bus);
		return {Result::Return, 8};
	case kVBlankIntrWait:
		intrWait(1, 0x0001, cpu, bus); // = IntrWait(1, VBlank mask).
		return {Result::Return, 8};
	case kHardReset:
		// Reboots through the HLE startup path (skips the ~2s logo
		// intro by design; documented simplification).
		reboot_requested_ = true;
		return {Result::NoReturn, 8};
	case kGetBiosChecksum:
		cpu.setReg(0, kBiosChecksum); // Genuine GBA checksum (GBATEK).
		// Register side effects observed from the real BIOS via mGBA's
		// HLE notes: r1=1, r3=0x4000 (BIOS region size).
		cpu.setReg(1, 1);
		cpu.setReg(3, 0x4000);
		return {Result::Return, 8};
	case kSoundBias: {
		// Level 000h for r0=0 else 200h; upper bits preserved.
		const u16 old = bus.read16(kSoundBias);
		const u16 level = (cpu.reg(0) == 0) ? 0x0000 : 0x0200;
		write16(bus, kSoundBias, (old & 0xFC00u) | level);
		return {Result::Return, 8};
	}
	case kSoundChannelClear:
		// Clears direct-sound channels; with no mixer yet (Phase 8)
		// there is no state to clear (documented).
		return {Result::Return, 8};
	case kMultiBoot:
		// No link hardware: report failure (r0=1, GBATEK).
		cpu.setReg(0, 1);
		return {Result::Return, 8};
	case kSoundDriverInit:
	case kSoundDriverMode:
	case kSoundDriverMain:
	case kSoundDriverVSync:
	case kSoundDriverVSyncOff:
	case kSoundDriverVSyncOn:
	case kMusicPlayerOpen:
	case kMusicPlayerStart:
	case kMusicPlayerStop:
	case kMusicPlayerContinue:
	case kMusicPlayerFadeOut:
		// BIOS sound driver + music player: used by ~2-3 games (per the
		// mGBA HLE article); full mixer arrives in Phase 8. No-op with
		// registers preserved (documented limitation).
		return {Result::Return, 4};
	case kSoundDriverGetJumpList:
		// Would return a table of BIOS sound entry points; with no
		// mixer there is nothing valid to point at, so return null
		// (documented; affects only BIOS-sound-driver games).
		cpu.setReg(0, 0);
		return {Result::Return, 4};
	default:
		// Unknown SWI: leave state untouched (documented; real BIOS
		// would wander — HLE stays deterministic instead).
		return {Result::Return, 4};
	}
}

} // namespace gba
