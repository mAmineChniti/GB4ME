#pragma once

#include "gba/state.h"
#include <cstdint>
#include <string>

namespace gba
{

// GBA cartridge GPIO + SII real-time clock (GBATEK "GBA Cart I/O Port").
// Only some cartridges wire these pins (Pokemon R/S/E, Boktai 1/2, ...);
// the bus routes 0x080000C4-0x080000C8 here only when present, otherwise
// that window stays ROM. Behaviour follows GBATEK's register/pin docs and
// the SII 3-wire protocol as implemented by mGBA's cart GPIO (independently
// written here): P0=SCK, P1=SIO, P2=CS. Command byte: low nibble magic
// (must be 6), bits 6-4 command (0 reset, 2 datetime, 3 force-IRQ,
// 4 control, 6 time), bit 7 read flag.
//
// The clock itself runs on host local time. A game that *writes* the
// datetime/time (Boktai's date changer) shifts that host-relative clock by
// a signed second offset instead of pinning a fixed reading, so the
// emulated time keeps running afterwards. The offset is battery-backed:
// saveToFile/loadFromFile keep it in a `<rom>.rtc` sidecar next to the
// save, mirroring the GB MBC3 RTC.
class GpioRtc
{
  public:
	static constexpr u32 kDataAddr = 0x080000C4u;
	static constexpr u32 kDirAddr = 0x080000C6u;
	static constexpr u32 kCtrlAddr = 0x080000C8u;
	static constexpr u32 kEndAddr = 0x080000CAu;

	GpioRtc();

	void reset(); // Power-on: write-only mode, RTC control = 0x40 (24h).
	void setPresent(bool present)
	{
		present_ = present;
	}
	bool present() const
	{
		return present_;
	}

	// Byte/halfword register access. Only 16-bit writes act (GBATEK: STRB
	// opcodes are ignored on the ROM bus); callers align beforehand.
	u8 read(u32 addr) const;
	void write(u32 addr, u16 value);

	// Battery-backed clock offset (`<rom>.rtc` sidecar).
	void saveToFile(const std::string &path) const;
	void loadFromFile(const std::string &path);
	// A different cartridge is a different battery: drop the offset the
	// previous one installed. reset() deliberately keeps it, because a
	// soft reset must not rewind a clock the game has already set.
	void clearOffset()
	{
		offset_ = 0;
	}
	// Offset applied to host local time, in seconds (test/diagnostic hook).
	std::int64_t offset() const
	{
		return offset_;
	}

	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

  private:
	// Pin helpers (mGBA protocol: outputs keep driven bits, inputs take
	// the `pins` value; the chip then re-evaluates).
	void outputPins(unsigned pins);
	void readPins(); // Evaluate the RTC chip after a pin-affecting write.
	void beginCommand();
	void processByte();
	unsigned outputBit();
	void updateClock();
	// Apply a game-written date/time payload (BCD, `bytes` long, seconds
	// first) by rebasing the emulated clock.
	void applyWrite(unsigned command);
	void abortTransfer();
	static u8 bcd(unsigned value);
	static unsigned cmdBytes(unsigned command);
	// Current emulated time (host local time + the stored offset) split
	// into calendar fields, so a time-only write keeps the date the chip
	// is currently showing instead of snapping back to the host date.
	void clockNow(int *year, int *month, int *day, int *weekday, int *hour, int *min,
				  int *sec) const;
	static bool fromBcd(unsigned value, int *out);

	bool present_ = false;
	u8 pin_state_ = 0; // Live 4-bit pin levels.
	u8 direction_ = 0; // 1 = GBA drives the pin.
	u8 write_latch_ = 0;
	bool read_write_ = false; // CONTROL bit 0: registers readable.

	// SII RTC serial engine.
	u8 rtc_bits_ = 0;         // Shift register / current byte.
	u8 rtc_bits_read_ = 0;    // Bits clocked in the current byte.
	u8 rtc_remaining_ = 0;    // Payload bytes remaining in the command.
	bool rtc_active_ = false; // A command byte was accepted (CS held).
	u8 rtc_command_ = 0;      // Full command byte (number + read flag).
	u8 rtc_control_ = 0x40;   // Bit 6 = 24h mode, bit 7 = power failure.
	u8 rtc_time_[7]{};        // Latched BCD datetime (year-100 .. second).
	bool rtc_sck_edge_ = true;
	bool rtc_sio_out_ = true;
	// Bytes clocked in by a date/time *write*, seconds first (7 or 3).
	u8 rtc_write_[7]{};
	u8 rtc_write_len_ = 0;
	// Signed seconds added to host time; 0 = "follow the host clock".
	std::int64_t offset_ = 0;
};

} // namespace gba
