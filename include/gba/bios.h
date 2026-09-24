#pragma once

#include "gb/types.h"
#include "gba/state.h"

namespace gba
{

class Arm7Tdmi;
class GbaBus;

// Built-in HLE BIOS (no external file, no direct boot — the ONLY GBA
// startup path). Behavior contracts follow GBATEK ("BIOS Functions") with
// mGBA's bios.c as the observable-behavior reference; everything here is
// implemented independently (no copied code, no Nintendo binary).
// https://problemkaputt.de/gbatek.htm#biosfunctions
class HleBios
{
  public:
	// SWI numbers (GBATEK + mGBA GBA_SWI_*).
	static constexpr u8 kSoftReset = 0x00;
	static constexpr u8 kRegisterRamReset = 0x01;
	static constexpr u8 kHalt = 0x02;
	static constexpr u8 kStop = 0x03;
	static constexpr u8 kIntrWait = 0x04;
	static constexpr u8 kVBlankIntrWait = 0x05;
	static constexpr u8 kDiv = 0x06;
	static constexpr u8 kDivArm = 0x07;
	static constexpr u8 kSqrt = 0x08;
	static constexpr u8 kArcTan = 0x09;
	static constexpr u8 kArcTan2 = 0x0A;
	static constexpr u8 kCpuSet = 0x0B;
	static constexpr u8 kCpuFastSet = 0x0C;
	static constexpr u8 kGetBiosChecksum = 0x0D;
	static constexpr u8 kBgAffineSet = 0x0E;
	static constexpr u8 kObjAffineSet = 0x0F;
	static constexpr u8 kBitUnPack = 0x10;
	static constexpr u8 kLz77UnCompWram = 0x11;
	static constexpr u8 kLz77UnCompVram = 0x12;
	static constexpr u8 kHuffUnComp = 0x13;
	static constexpr u8 kRlUnCompWram = 0x14;
	static constexpr u8 kRlUnCompVram = 0x15;
	static constexpr u8 kDiff8bitUnFilterWram = 0x16;
	static constexpr u8 kDiff8bitUnFilterVram = 0x17;
	static constexpr u8 kDiff16bitUnFilter = 0x18;
	static constexpr u8 kSoundBias = 0x19;
	static constexpr u8 kSoundDriverInit = 0x1A;
	static constexpr u8 kSoundDriverMode = 0x1B;
	static constexpr u8 kSoundDriverMain = 0x1C;
	static constexpr u8 kSoundDriverVSync = 0x1D;
	static constexpr u8 kSoundChannelClear = 0x1E;
	static constexpr u8 kMidiKey2Freq = 0x1F;
	static constexpr u8 kMusicPlayerOpen = 0x20;
	static constexpr u8 kMusicPlayerStart = 0x21;
	static constexpr u8 kMusicPlayerStop = 0x22;
	static constexpr u8 kMusicPlayerContinue = 0x23;
	static constexpr u8 kMusicPlayerFadeOut = 0x24;
	static constexpr u8 kMultiBoot = 0x25;
	static constexpr u8 kHardReset = 0x26;
	static constexpr u8 kCustomHalt = 0x27;
	static constexpr u8 kSoundDriverVSyncOff = 0x28;
	static constexpr u8 kSoundDriverVSyncOn = 0x29;
	static constexpr u8 kSoundDriverGetJumpList = 0x2A;

	// Genuine BIOS checksum returned by SWI 0Dh (GBATEK: BAAE187Fh on GBA).
	static constexpr u32 kBiosChecksum = 0xBAAE187F;

	// IWRAM system area (GBATEK "Default memory usage").
	static constexpr u32 kIrqFlagsAddr = 0x03007FF8; // IntrWait check flags.
	static constexpr u32 kBootFlagAddr = 0x03007FFA; // SoftReset return flag.
	static constexpr u32 kSpSvc = 0x03007FE0;
	static constexpr u32 kSpIrq = 0x03007FA0;
	static constexpr u32 kSpUsr = 0x03007F00;
	static constexpr u32 kClearBase = 0x03007E00; // SoftReset zeroes ..0x3007FFF.

	// What the dispatcher did; the core performs the MOVS return itself
	// unless the service took over control entirely (Soft/HardReset).
	enum class Result : u8 {
		Return,   // Core performs returnFromSwi().
		NoReturn, // Service already set PC/CPSR (reset paths).
	};

	HleBios();

	void reset();
	// The ONLY boot path: post-BIOS startup state, then enter the ROM at
	// its header branch target (ARM state, System mode).
	void applyStartup(Arm7Tdmi &cpu, GbaBus &bus);
	// Installs the HLE exception vectors + IRQ trampoline into the BIOS
	// backing (GBATEK "BIOS Interrupt handling" protocol, HLE addresses).
	// Idempotent; called by applyStartup.
	void installVectors(GbaBus &bus);
	// SWI dispatcher. Called after the CPU performs full SWI exception
	// entry. Returns {result, cycles consumed}.
	struct Outcome {
		Result result = Result::Return;
		u32 cycles = 0;
	};
	Outcome handleSwi(u8 num, Arm7Tdmi &cpu, GbaBus &bus);

	// IntrWait-family wait state (woken by the Phase 5 IRQ controller via
	// pollWait; until then the CPU stays halted).
	bool waiting() const
	{
		return waiting_;
	}
	// True when re-entered boot is requested (HardReset): the core must
	// reset() + applyStartup() again. Skips the ~2s logo intro by design.
	bool rebootRequested() const
	{
		return reboot_requested_;
	}
	void clearReboot()
	{
		reboot_requested_ = false;
	}
	// Wake check for the IRQ controller: if a waited interrupt fired,
	// update [0x3007FF8], wake the CPU and report true.
	bool pollWait(Arm7Tdmi &cpu, GbaBus &bus);
	// Public for the selftest harness (SWI 04h/05h entry point).
	using IntrWaitFn = void (HleBios::*)(u32, u32, Arm7Tdmi &, GbaBus &);
	IntrWaitFn intrWaitFn()
	{
		return &HleBios::intrWait;
	}

	// Save state serialization (IntrWait-family wait state).
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

  private:
	bool waiting_ = false;
	u16 wait_mask_ = 0;
	bool wait_any_ = false; // Halt/Stop/CustomHalt: any (IE & IF).
	bool reboot_requested_ = false;

	void intrWait(u32 r0, u32 r1, Arm7Tdmi &cpu, GbaBus &bus);

	// Subdispatchers (one TU each, following repo file conventions).
	Outcome system(u8 num, Arm7Tdmi &cpu, GbaBus &bus);
	Outcome memory(u8 num, Arm7Tdmi &cpu, GbaBus &bus);
	Outcome math(u8 num, Arm7Tdmi &cpu, GbaBus &bus);
	Outcome decomp(u8 num, Arm7Tdmi &cpu, GbaBus &bus);
	Outcome affine(u8 num, Arm7Tdmi &cpu, GbaBus &bus);
};

} // namespace gba
