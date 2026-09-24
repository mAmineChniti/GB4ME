// GBA keypad: active-low input + KEYCNT interrupt evaluation.

#include "gba/keypad.h"
#include "gba/irq.h"

namespace gba
{

GbaKeypad::GbaKeypad()
{
	reset();
}

void GbaKeypad::reset()
{
	pressed_ = 0; // All released (KEYINPUT reads 0x03FF).
	keycnt_ = 0;
}

void GbaKeypad::setKey(Key key, bool pressed)
{
	const u16 bit = 1u << static_cast<u8>(key);
	const bool was = (pressed_ & bit) != 0;
	if (pressed)
		pressed_ |= bit;
	else
		pressed_ &= ~bit;
	if (pressed != was)
		evaluate();
}

void GbaKeypad::releaseAll()
{
	pressed_ = 0;
}

u16 GbaKeypad::readInput() const
{
	// Idle value is 0x03FF (upper bits read 0, matching mGBA's reset
	// default); pressed buttons read 0 (active-low).
	return static_cast<u16>(~pressed_ & 0x03FFu);
}

void GbaKeypad::writeCnt(u16 value)
{
	keycnt_ = value & 0xC3FFu;
	evaluate(); // Level behavior: a newly matching condition fires at once.
}

void GbaKeypad::evaluate()
{
	if (irq_ == nullptr)
		return;
	if ((keycnt_ & 0x4000u) == 0)
		return; // IRQ disabled.
	const u16 sel = keycnt_ & 0x03FFu;
	if ((keycnt_ & 0x8000u) != 0) {
		// AND: all selected pressed (fires even on an empty mask, matching
		// mGBA GBATestKeypadIRQ).
		if (sel == (pressed_ & sel))
			irq_->raise(GbaIrq::kKeypad);
	} else {
		// OR: any selected pressed.
		if ((pressed_ & sel) != 0)
			irq_->raise(GbaIrq::kKeypad);
	}
}

void GbaKeypad::save(StateBuffer &out) const
{
	out.write(pressed_);
	out.write(keycnt_);
}

void GbaKeypad::load(const StateBuffer &in)
{
	in.read(pressed_);
	in.read(keycnt_);
}

} // namespace gba
