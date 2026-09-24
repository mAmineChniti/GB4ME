// GBA interrupt controller: level-triggered IRQ delivery.

#include "gba/irq.h"
#include "gba/bus.h"
#include "gba/cpu.h"
#include <cstdio>
#include <cstdlib>

namespace gba
{

GbaIrq::GbaIrq()
{
	reset();
}

void GbaIrq::reset()
{
	ie_ = 0;
	if_ = 0;
	ime_ = 0;
#ifndef GB4ME_RELEASE
	trace_ = std::getenv("GB4ME_IRQ_TRACE") != nullptr;
#endif
	now_ = 0;
	update();
}

void GbaIrq::writeIe(u16 v)
{
	ie_ = v & 0x3FFFu;
	update();
}

void GbaIrq::writeIf(u16 v)
{
	if_ &= ~v; // Writing 1 acknowledges (clears) the flag.
	update();
}

void GbaIrq::writeIme(u16 v)
{
	ime_ = v & 1u;
	update();
}

void GbaIrq::raise(u16 mask)
{
	if_ |= mask & 0x3FFFu;
	update();
}

void GbaIrq::tick(u64 cycles)
{
	now_ += cycles;
}

void GbaIrq::update()
{
	if (cpu_ == nullptr)
		return;
	if (ime_ != 0 && (ie_ & if_) != 0) {
#ifndef GB4ME_RELEASE
		if (trace_) {
			const u32 handler = cpu_->bus() != nullptr ? cpu_->bus()->read32(0x03007FFCu) : 0;
			std::printf("gba-irq: now=%llu IE=%04X IF=%04X handler=%08X\n",
						(unsigned long long) now_, ie_, if_, handler);
		}
#endif
		cpu_->requestIrq();
	} else {
		cpu_->clearIrq();
	}
}

void GbaIrq::save(StateBuffer &out) const
{
	out.write(ie_);
	out.write(if_);
	out.write(ime_);
}

void GbaIrq::load(const StateBuffer &in)
{
	in.read(ie_);
	in.read(if_);
	in.read(ime_);
}

} // namespace gba
