// GBA DMA: 4 channels, immediate/blanking/FIFO triggers, repeat, IRQs.

#include "gba/dma.h"
#include "gba/bus.h"
#include "gba/irq.h"
#include "gba/scheduler.h"
#include <cstdio>
#include <cstdlib>

namespace gba
{

GbaDma::GbaDma()
{
	reset();
}

void GbaDma::reset()
{
	for (auto &ch : channels_)
		ch = Channel{};
	pending_cycles_ = 0;
	for (unsigned i = 0; i < kChannels; i++) {
		pending_[i] = false;
		deadline_[i] = 0;
		gen_[i] = 0;
	}
}

u32 GbaDma::maskAddr(unsigned ch, u32 addr, bool dest)
{
	// GBATEK: MSBs ignored — 27 bits internal, 28 bits any-memory.
	// DMA0 is internal-only; others take any-memory sources (except SRAM,
	// refused at transfer time); DMA3 alone takes any-memory destinations.
	if (ch == 0)
		return addr & 0x07FFFFFFu;
	if (!dest)
		return addr & 0x0FFFFFFFu;
	if (ch == 3)
		return addr & 0x0FFFFFFFu;
	return addr & 0x07FFFFFFu;
}

void GbaDma::latch(unsigned ch)
{
	Channel &c = channels_[ch];
	c.src = maskAddr(ch, c.sad, false);
	c.dst = maskAddr(ch, c.dad, true);
	// CNT_L is 14-bit on DMA0-2 (upper bits ignored), full 16-bit on
	// DMA3 (GBATEK; mGBA masks with 0x3FFF on reload for channels 0-2).
	const u16 count = (ch == 3) ? c.count : static_cast<u16>(c.count & 0x3FFFu);
	c.remaining = (count == 0) ? maxCount(ch) : count;
	c.armed = true;
}

u16 GbaDma::read(u32 addr)
{
	const unsigned ch = (addr - kIoBase) / 12;
	if (ch >= kChannels)
		return 0xFFFF;
	// Same reasoning as write(): the CPU only reaches a register once the
	// block is over, so a completion still in flight has to settle first.
	// Pokemon Emerald polls CNT_H right after its init DMA and expects
	// Enable to read back CLEARED (hardware clears it on completion).
	if (pending_[ch])
		onCompletion(ch, gen_[ch]);
	const Channel &c = channels_[ch];
	switch ((addr - kIoBase) % 12) {
	case 0:
		return static_cast<u16>(c.sad & 0xFFFFu);
	case 2:
		return static_cast<u16>(c.sad >> 16);
	case 4:
		return static_cast<u16>(c.dad & 0xFFFFu);
	case 6:
		return static_cast<u16>(c.dad >> 16);
	case 8:
		return c.count;
	case 10:
		return c.control;
	default:
		return 0xFFFF;
	}
}

void GbaDma::write(u32 addr, u16 value)
{
	const unsigned ch = (addr - kIoBase) / 12;
	if (ch >= kChannels)
		return;
	Channel &c = channels_[ch];
	// A DMA transfer occupies the bus for its whole block, so the CPU cannot
	// touch the channel until the block is over: settle a completion that is
	// still in flight before applying the write. Without this the visible
	// channel state lags hardware - Golden Sun reprograms DMA3 immediately
	// after a transfer, and because Enable was still set (the deferred
	// completion had not cleared it yet) the new block was dropped, leaving
	// its 5 KiB IWRAM payload zeroed and the CPU sliding through 0x00000000
	// as if it were code.
	if (pending_[ch])
		onCompletion(ch, gen_[ch]);
	switch ((addr - kIoBase) % 12) {
	case 0:
		c.sad = (c.sad & 0xFFFF0000u) | value;
		return;
	case 2:
		c.sad = (c.sad & 0x0000FFFFu) | (static_cast<u32>(value) << 16);
		return;
	case 4:
		c.dad = (c.dad & 0xFFFF0000u) | value;
		return;
	case 6:
		c.dad = (c.dad & 0x0000FFFFu) | (static_cast<u32>(value) << 16);
		return;
	case 8:
		c.count = value;
		return;
	case 10: {
		const bool was = (c.control & kEnable) != 0;
		// CNT_H low 5 bits read as 0; DRQ (bit 11) exists on DMA3 only
		// (mGBA masks DMA0-2 with 0xF7E0, DMA3 with 0xFFE0).
		c.control = static_cast<u16>(value & (ch == 3 ? 0xFFE0u : 0xF7E0u));
		const bool now = (c.control & kEnable) != 0;
		if (!now) {
			c.armed = false; // Manual stop (only meaningful pre-fire).
			cancelPending(ch);
			return;
		}
		if (!was) {
			latch(ch);
			const auto timing = static_cast<Timing>((c.control & kTimingMask) >> 12);
			if (timing == Timing::Immediate)
				transfer(ch, false);
		}
		return;
	}
	default:
		return;
	}
}

void GbaDma::transfer(unsigned ch, bool fifo)
{
	Channel &c = channels_[ch];
	if (bus_ == nullptr || !c.armed)
		return;
	// No pending-completion guard: a channel re-armed before its completion
	// fired transfers again (that is what hardware does), and the completion
	// of a superseded transfer is voided by the generation counter.
	if (pending_[ch]) {
		pending_[ch] = false;
		++gen_[ch];
	}
#ifndef GB4ME_RELEASE
	if (const char *env = std::getenv("GB4ME_GBA_DMA_TRACE")) {
		(void) env;
		std::printf("gba-dma: ch%u %s src=%08X dst=%08X count=%04X ctrl=%04X\n", ch,
					fifo ? "fifo" : (c.control & kWord32 ? "32" : "16"), c.src, c.dst,
					fifo ? 4 : c.remaining, c.control);
	}
#endif
	const bool wide = ((c.control & kWord32) != 0) || fifo;
	const unsigned dst_ctl = (c.control & kDstMask) >> 5;
	const unsigned src_ctl = (c.control & kSrcMask) >> 7;
	u32 units = fifo ? 4 : c.remaining;
	bool seq = false;
	const u64 cycle_base = pending_cycles_;
	// EEPROM serial port (0x0D000000, DMA3): one halfword per bit, bit 0
	// significant (GBATEK "Using DMA"). SRAM transfers run through the
	// bus like any other region (slow 8-bit timing via accessCycles).
	while (units != 0) {
		const bool e_dst = !fifo && bus_->eepromActive() && (c.dst >> 24) == 0x0Du;
		const bool e_src = !fifo && bus_->eepromActive() && (c.src >> 24) == 0x0Du;
		if (wide) {
			u32 v = 0;
			if (e_src) {
				// Decomposed 32-bit source: two serial bits.
				const u16 lo = bus_->eepromDmaRead();
				const u16 hi = bus_->eepromDmaRead();
				v = lo | (static_cast<u32>(hi) << 16);
				pending_cycles_ += bus_->accessCycles(c.src, 4, seq);
			} else {
				v = bus_->read32(c.src & ~3u);
				pending_cycles_ += bus_->accessCycles(c.src, 4, seq);
			}
			if (e_dst) {
				// Decomposed 32-bit dest: two serial bits. Halfword
				// counts include the current half (engine sync).
				const u32 half_left = units * 2;
				bus_->eepromDmaWrite(static_cast<u16>(v & 0xFFFFu), half_left);
				bus_->eepromDmaWrite(static_cast<u16>(v >> 16), half_left - 1);
				pending_cycles_ += bus_->accessCycles(c.dst, 4, seq);
			} else {
				bus_->write32(c.dst & ~3u, v);
				pending_cycles_ += bus_->accessCycles(c.dst, 4, seq);
			}
		} else {
			u16 v = 0;
			if (e_src) {
				v = bus_->eepromDmaRead();
				pending_cycles_ += bus_->accessCycles(c.src, 2, seq);
			} else {
				v = bus_->read16(c.src & ~1u);
				pending_cycles_ += bus_->accessCycles(c.src, 2, seq);
			}
			if (e_dst) {
				// `units` includes the current halfword (engine sync).
				bus_->eepromDmaWrite(v, units);
				pending_cycles_ += bus_->accessCycles(c.dst, 2, seq);
			} else {
				bus_->write16(c.dst & ~1u, v);
				pending_cycles_ += bus_->accessCycles(c.dst, 2, seq);
			}
		}
		seq = true;
		const u32 step = wide ? 4u : 2u;
		switch (src_ctl) {
		case 0:
			c.src += step;
			break; // Increment.
		case 1:
			c.src -= step;
			break; // Decrement.
		case 2:
			break; // Fixed.
		default:
			c.src += step;
			break; // Prohibited: increment.
		}
		if (!fifo) {
			switch (dst_ctl) {
			case 0:
				c.dst += step;
				break; // Increment.
			case 1:
				c.dst -= step;
				break; // Decrement.
			case 2:
				break; // Fixed.
			case 3:
				c.dst += step;
				break; // Inc; reload on repeat.
			}
			c.remaining--;
		}
		--units;
	}
	// FIFO blocks complete like any other transfer (mGBA clears Enable
	// for non-repeat channels after the 4-word block; repeat channels
	// stay armed — remaining/dst-reload in finish() are no-ops on the
	// FIFO path since it ignores remaining and sound DMA uses fixed dst).
	//
	// A transfer owns the bus for its whole block, so the time is charged to
	// the clock HERE - at the end of the instruction that started it - and the
	// completion side effects follow immediately. Deferring them to a
	// scheduler event instead leaves the CPU running through cycles the
	// hardware spends stalled: the PPU/timers advance, but the game keeps
	// executing and writing video state the DMA was supposed to overwrite
	// (Pokemon Emerald's opening DMA3 loses its 5 KiB IWRAM payload that
	// way). Charging the block here also means the completion IRQ lands at
	// the same emulated instant it did when it ran inline.
	const u64 latency = pending_cycles_ - cycle_base;
	if (scheduler_ != nullptr && latency != 0)
		scheduler_->step(scheduler_->now() + latency);
	finish(ch);
}

void GbaDma::finish(unsigned ch)
{
	Channel &c = channels_[ch];
	const bool repeat = (c.control & kRepeat) != 0;
	if ((c.control & kIrq) != 0 && irq_ != nullptr) {
		irq_->raise(GbaIrq::kDma0 << ch);
	}
	if (repeat) {
		// Re-arm the counter (and DAD in increment/reload mode); Enable
		// stays set until software stops the channel. Count width matches
		// latch() (14-bit on DMA0-2).
		const u16 count = (ch == 3) ? c.count : static_cast<u16>(c.count & 0x3FFFu);
		c.remaining = (count == 0) ? maxCount(ch) : count;
		if (((c.control & kDstMask) >> 5) == 3) {
			c.dst = maskAddr(ch, c.dad, true);
		}
	} else {
		c.control &= ~kEnable;
		c.armed = false;
	}
}

void GbaDma::scheduleCompletion(unsigned ch, u64 latency)
{
	pending_[ch] = true;
	const u32 gen = ++gen_[ch];
	deadline_[ch] = scheduler_->now() + latency;
	scheduler_->schedule(latency, [this, ch, gen] { onCompletion(ch, gen); });
}

void GbaDma::onCompletion(unsigned ch, u32 gen)
{
	if (gen != gen_[ch])
		return; // Stopped or reprogrammed under this event.
	pending_[ch] = false;
	finish(ch);
}

void GbaDma::cancelPending(unsigned ch)
{
	// Bump the generation so a scheduled completion becomes a no-op.
	++gen_[ch];
	pending_[ch] = false;
}

void GbaDma::rearmEvents()
{
	if (scheduler_ == nullptr)
		return;
	for (unsigned ch = 0; ch < kChannels; ch++) {
		if (!pending_[ch])
			continue;
		const u64 now = scheduler_->now();
		const u64 delay = deadline_[ch] > now ? deadline_[ch] - now : 0;
		const u32 gen = gen_[ch];
		scheduler_->schedule(delay, [this, ch, gen] { onCompletion(ch, gen); });
	}
}
void GbaDma::trigger(Timing t)
{
	for (unsigned ch = 0; ch < kChannels; ch++) {
		Channel &c = channels_[ch];
		if (!c.armed || (c.control & kEnable) == 0)
			continue;
		const auto timing = static_cast<Timing>((c.control & kTimingMask) >> 12);
		if (timing != t)
			continue;
		if (t == Timing::Special) {
			if (ch == 0 || ch == 3)
				continue; // DMA0 prohibited; DMA3 = capture.
			continue;     // FIFO requests arrive via triggerFifo (Phase 8).
		}
		transfer(ch, false);
	}
}

void GbaDma::triggerFifo(unsigned ch)
{
	if (ch != 1 && ch != 2)
		return;
	Channel &c = channels_[ch];
	if (!c.armed || (c.control & kEnable) == 0)
		return;
	const auto timing = static_cast<Timing>((c.control & kTimingMask) >> 12);
	if (timing != Timing::Special)
		return;
	// No repeat requirement (mGBA services FIFO requests whenever the
	// channel is armed + enabled + Special; completion semantics come
	// from finish() after the block).
	transfer(ch, true);
}

void GbaDma::save(StateBuffer &out) const
{
	for (unsigned i = 0; i < kChannels; i++) {
		const auto &c = channels_[i];
		out.write(c.sad);
		out.write(c.dad);
		out.write(c.count);
		out.write(c.control);
		out.write(c.src);
		out.write(c.dst);
		out.write(c.remaining);
		out.write(c.armed);
		out.write(pending_[i]);
		out.write(deadline_[i]);
		out.write(gen_[i]);
	}
	out.write(pending_cycles_);
}

void GbaDma::load(const StateBuffer &in)
{
	for (unsigned i = 0; i < kChannels; i++) {
		auto &c = channels_[i];
		in.read(c.sad);
		in.read(c.dad);
		in.read(c.count);
		in.read(c.control);
		in.read(c.src);
		in.read(c.dst);
		in.read(c.remaining);
		in.read(c.armed);
		if (!in.read(pending_[i]))
			pending_[i] = false;
		if (!in.read(deadline_[i]))
			deadline_[i] = 0;
		if (!in.read(gen_[i]))
			gen_[i] = 0;
	}
	in.read(pending_cycles_);
}

} // namespace gba
