// Joypad: P1 register (0xFF00) multiplexing.
// https://gbdev.io/pandocs/Joypad_Input.html

#include "gb/joypad.h"
#include "gb/mmu.h"

namespace gb
{

Joypad::Joypad()
{
	reset();
}

void Joypad::reset()
{
	p1_ = 0xFF;
	select_buttons_ = false;
	select_dpad_ = false;
	key_state_ = 0;
}

void Joypad::set_key(Key key, bool pressed)
{
	const u8 bit = 1u << static_cast<u8>(key);
	const bool was = (key_state_ & bit) != 0;
	if (pressed)
		key_state_ |= bit;
	else
		key_state_ &= ~bit;
	// Press edge on a selected group raises the Joypad interrupt (bit 4).
	// https://gbdev.io/pandocs/Interrupts.html
	if (pressed && !was && mmu_) {
		const u8 k = static_cast<u8>(key);
		const bool in_group = (select_buttons_ && k >= 4) || (select_dpad_ && k < 4);
		if (in_group)
			mmu_->request_interrupt(4);
	}
}

void Joypad::release_all()
{
	key_state_ = 0;
}

u8 Joypad::read_p1() const
{
	u8 v = 0xC0 | (p1_ & 0x30); // Bits 7-6 read 1; bits 5-4 are the selects.
	u8 keys = 0x0F;
	if (select_buttons_) {
		keys &= ~((key_state_ >> 4) & 0x0F); // A B Select Start -> bits 0-3.
	}
	if (select_dpad_) {
		keys &= ~(key_state_ & 0x0F); // Right Left Up Down -> bits 0-3.
	}
	return v | keys;
}

void Joypad::write_p1(u8 value)
{
	p1_ = value;
	select_buttons_ = (value & 0x20) == 0;
	select_dpad_ = (value & 0x10) == 0;
}

} // namespace gb
