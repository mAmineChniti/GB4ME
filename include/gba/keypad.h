#pragma once

#include "gba/state.h"

namespace gba
{

class GbaIrq;

// GBA keypad (Phase 7): 10 buttons, active-low KEYINPUT, KEYCNT interrupt
// control. KEYCNT evaluation matches mGBA GBATestKeypadIRQ exactly,
// including firing on an empty AND mask.
// https://problemkaputt.de/gbatek.htm#gbakeypadinput
class GbaKeypad
{
  public:
	// Bit numbers match KEYINPUT/KEYCNT bit positions.
	enum class Key : u8 {
		A = 0,
		B = 1,
		Select = 2,
		Start = 3,
		Right = 4,
		Left = 5,
		Up = 6,
		Down = 7,
		R = 8,
		L = 9,
	};

	static constexpr u32 kKeyInputAddr = 0x04000130;
	static constexpr u32 kKeyCntAddr = 0x04000132;

	GbaKeypad();

	void reset();
	void setIrq(GbaIrq *irq)
	{
		irq_ = irq;
	}

	void setKey(Key key, bool pressed);
	// Drops every held key (window focus loss: the matching key-up events
	// never arrive, so a pressed button would otherwise stick).
	void releaseAll();
	bool pressed(Key key) const
	{
		return (pressed_ & (1u << static_cast<u8>(key))) != 0;
	}

	u16 readInput() const; // KEYINPUT: 0 = pressed, upper bits read 1.
	u16 readCnt() const
	{
		return keycnt_;
	}
	void writeCnt(u16 value);

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

  private:
	GbaIrq *irq_ = nullptr;
	u16 pressed_ = 0; // Bit set = pressed (inverse of KEYINPUT bits).
	u16 keycnt_ = 0;

	void evaluate();
};

} // namespace gba
