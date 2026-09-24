// GBA core: owns bus/scheduler/cartridge/CPU/HLE BIOS, wires them together.

#include "gba/core.h"
#include "gba/state.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace gba
{

GameBoyAdvance::GameBoyAdvance()
{
	cpu_.setBus(&bus_);
	bus_.setIrq(&irq_);
	bus_.setTimers(&timers_);
	bus_.setDma(&dma_);
	bus_.setPpu(&ppu_);
	bus_.setKeypad(&keypad_);
	bus_.setAudio(&audio_);
	bus_.setCartridge(&cartridge_);
	irq_.setCpu(&cpu_);
	timers_.setIrq(&irq_);
	// Timer 0/1 overflows drive Direct Sound FIFO consumption.
	timers_.setOverflowHook([this](unsigned timer) { audio_.onTimerOverflow(timer); });
	dma_.setBus(&bus_);
	dma_.setIrq(&irq_);
	ppu_.setIrq(&irq_);
	ppu_.setDma(&dma_);
	keypad_.setIrq(&irq_);
	audio_.setDma(&dma_);
	// SWI path: CPU exception entry already done; log the service, run it,
	// then perform the MOVS return unless the service took over control.
	// IRQ observer (env GB4ME_IRQ_TRACE): logs every IRQ entry with the
	// handler address and whether the CPU had been in BIOS recently.
	// Debug aid for boot-timing problems; zero cost when unset.
#ifndef GB4ME_RELEASE
	irq_trace_ = std::getenv("GB4ME_IRQ_TRACE") != nullptr;
#endif
	cpu_.setSwiHandler([this](u8 num) {
		if (num < swi_histogram_.size())
			swi_histogram_[num]++;
		const HleBios::Outcome out = bios_.handleSwi(num, cpu_, bus_);
		if (out.result == HleBios::Result::Return)
			cpu_.returnFromSwi();
	});
	swi_histogram_.fill(0);
	reset();
}

bool GameBoyAdvance::load(const std::string &path)
{
	Cartridge cart;
	if (!cart.load(path))
		return false;
	rom_path_ = path;
	return loadFromBytes(cart.rom());
}

bool GameBoyAdvance::loadFromBytes(const std::vector<u8> &data)
{
	if (!cartridge_.loadFromBytes(data)) {
		loaded_ = false;
		return false;
	}
	if (!rom_path_.empty()) {
		cartridge_.loadFromFile(rom_path_ + ".sav");
	}
	reset();
	loaded_ = true;
	return true;
}

void GameBoyAdvance::reset()
{
	// Order: bus (clears RAM/backing) -> devices -> attach image ->
	// CPU/BIOS reset -> HLE startup enters the ROM. HLE BIOS startup
	// takes over the CPU state and jumps to the ROM entry.
	bus_.reset();
	irq_.reset();
	timers_.reset();
	dma_.reset();
	ppu_.reset();
	keypad_.reset();
	audio_.reset();
	scheduler_.reset();
	if (cartridge_.loaded()) {
		bus_.attachRom(cartridge_.rom().data(), cartridge_.rom().size());
	} else {
		bus_.detachRom();
	}
	cpu_.reset();
	bios_.reset();
	swi_histogram_.fill(0);
	if (cartridge_.loaded())
		bios_.applyStartup(cpu_, bus_);
}

void GameBoyAdvance::step(u32 cycles)
{
	u64 target = scheduler_.now() + cycles;
	while (scheduler_.now() < target) {
		if (bios_.rebootRequested()) {
			bios_.clearReboot();
			reset();
			continue;
		}
		// Poll HLE waits (the IRQ controller drives IF; IntrWait-family
		// calls stay halted until their flags fire).
		bios_.pollWait(cpu_, bus_);
		if (cpu_.halted()) {
			// Halted CPUs burn wall-clock while devices keep running, so
			// PPU/timer IRQs can wake us (VBlankIntrWait/etc.). Slice in
			// ~scanline steps; exit early on wake or budget end.
			// (Stop behaves like Halt here — timers keep ticking — because
			// its narrowed wake set is not modeled yet; documented.)
			while (scheduler_.now() < target && cpu_.halted()) {
				bios_.pollWait(cpu_, bus_);
				if (!cpu_.halted())
					break;
				if (!bios_.waiting()) {
					// Plain Halt/Stop (no IntrWait mask): GBATEK wakes the CPU
					// as long as (IE AND IF) != 0 — IME and CPSR.I don't gate
					// waking (only exception entry). pollWait above only wakes
					// IntrWait-family waits, so check the plain condition here;
					// otherwise a bare Halt sleeps forever.
					const u16 fired = static_cast<u16>(bus_.read16(0x04000200) &
													   bus_.read16(0x04000202));
					if (fired != 0) {
						cpu_.wake();
						break;
					}
				}
				u64 left = target - scheduler_.now();
				const u64 slice = left < 1024 ? left : 1024;
				const u32 s32 = static_cast<u32>(slice);
				timers_.step(s32);
				ppu_.step(s32);
				audio_.step(s32);
				scheduler_.step(scheduler_.now() + slice);
			}
			// Final-slice IRQs (e.g. VBlank raised on the last slice)
			// would otherwise wait a full extra step to wake us.
			bios_.pollWait(cpu_, bus_);
			continue;
		}
		const u32 cpu_cycles = cpu_.step();
		// DMA transfers inside the step consumed bus time (CPU stalled);
		// timers, PPU and audio observe CPU + DMA time together.
		const u64 dma_cycles = dma_.takePendingCycles();
		const u64 delta = static_cast<u64>(cpu_cycles) + dma_cycles;
		irq_.tick(delta);
		const u32 step32 = delta > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<u32>(delta);
		timers_.step(step32);
		ppu_.step(step32);
		audio_.step(step32);
		scheduler_.step(scheduler_.now() + delta);
	}
}

bool GameBoyAdvance::saveState(const std::string &path) const
{
	StateBuffer buf;
	buf.write(kStateMagic);
	buf.write(kStateVersion);
	bus_.save(buf);
	cartridge_.save(buf);
	cpu_.save(buf);
	ppu_.save(buf);
	dma_.save(buf);
	timers_.save(buf);
	irq_.save(buf);
	audio_.save(buf);
	keypad_.save(buf);
	bios_.save(buf);
	scheduler_.save(buf);
	std::ofstream file(path, std::ios::binary);
	if (!file)
		return false;
	const auto &data = buf.data();
	file.write(reinterpret_cast<const char *>(data.data()),
			   static_cast<std::streamsize>(data.size()));
	return true;
}

bool GameBoyAdvance::loadState(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return false;
	std::vector<u8> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	StateBuffer buf;
	buf.data(data);
	u32 magic = 0;
	if (!buf.read(magic) || magic != kStateMagic)
		return false;
	u32 version = 0;
	if (!buf.read(version) || version != kStateVersion)
		return false;
	bus_.load(buf);
	cartridge_.load(buf);
	cpu_.load(buf);
	ppu_.load(buf);
	dma_.load(buf);
	timers_.load(buf);
	irq_.load(buf);
	audio_.load(buf);
	keypad_.load(buf);
	// BIOS wait state postdates v1 states; absent fields keep defaults
	// (a v1 state saved mid-wait resumes running instead of waiting).
	bios_.load(buf);
	scheduler_.load(buf);
	return true;
}

} // namespace gba
