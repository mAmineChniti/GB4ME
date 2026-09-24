#pragma once

#include "types.h"

namespace gb
{

class MMU; // Bus. Joypad raises interrupts via MMU::request_interrupt.

// Joypad: P1 register multiplexing.
// https://gbdev.io/pandocs/Joypad_Input.html
class Joypad
{
  public:
	enum class Key : u8 {
		Right = 0,
		Left = 1,
		Up = 2,
		Down = 3,
		A = 4,
		B = 5,
		Select = 6,
		Start = 7
	};

	Joypad();
	~Joypad() = default;

	void reset();
	void set_mmu(MMU *mmu)
	{
		mmu_ = mmu;
	}
	void set_key(Key key, bool pressed);
	// Drops every held key (window focus loss: the matching key-up events
	// never arrive, so a pressed button would otherwise stick).
	void release_all();

	u8 read_p1() const;
	void write_p1(u8 value);
	// Direct (unmultiplexed) state of one key, for the input-manager tests:
	// read_p1 only shows a key while its P1 select line is low.
	bool key_down(Key key) const
	{
		return (key_state_ & static_cast<u8>(1u << static_cast<u8>(key))) != 0;
	}

  private:
	MMU *mmu_ = nullptr;

	u8 p1_ = 0xFF;
	bool select_buttons_ = false;
	bool select_dpad_ = false;
	u8 key_state_ = 0; // Bit set = pressed.
};

} // namespace gb
