// GBA cartridge GPIO + SII RTC (GBATEK "GBA Cart I/O Port (GPIO)").
// Behavioural model of the register/pin/protocol docs: 4-bit DATA (with
// direction mask), 4-bit DIRECTION, 1-bit CONTROL (write-only vs
// read/write), and the SII 3-wire serial engine (SCK/SIO/CS) serving the
// reset/datetime/time/control commands off host local time.

#include "gba/gpio.h"

#include <ctime>

namespace gba
{

namespace
{

// Command numbers (bits 6-4 of the command byte).
constexpr unsigned kCmdReset = 0;
constexpr unsigned kCmdDatetime = 2;
constexpr unsigned kCmdForceIrq = 3;
constexpr unsigned kCmdControl = 4;
constexpr unsigned kCmdTime = 6;

} // namespace

GpioRtc::GpioRtc()
{
	reset();
}

void GpioRtc::reset()
{
	pin_state_ = 0;
	direction_ = 0;
	write_latch_ = 0;
	read_write_ = false;
	rtc_bits_ = 0;
	rtc_bits_read_ = 0;
	rtc_remaining_ = 0;
	rtc_active_ = false;
	rtc_command_ = 0;
	rtc_control_ = 0x40; // 24-hour mode, no power failure (mGBA reset).
	for (u8 &b : rtc_time_)
		b = 0;
	rtc_sck_edge_ = true;
	rtc_sio_out_ = true;
}

u8 GpioRtc::read(u32 addr) const
{
	// Write-only mode reads back 00h (GBATEK); the upper byte of every
	// 16-bit register is unpopulated.
	if (!read_write_)
		return 0x00;
	switch (addr) {
	case kDataAddr:
		return pin_state_ & 0x0Fu;
	case kDirAddr:
		return direction_ & 0x0Fu;
	case kCtrlAddr:
		return read_write_ ? 0x01u : 0x00u;
	default:
		return 0x00;
	}
}

void GpioRtc::write(u32 addr, u16 value)
{
	switch (addr) {
	case kDataAddr:
		write_latch_ = static_cast<u8>(value & 0x0Fu);
		pin_state_ &= static_cast<u8>(~direction_);
		pin_state_ |= static_cast<u8>(write_latch_ & direction_);
		readPins();
		return;
	case kDirAddr:
		direction_ = static_cast<u8>(value & 0x0Fu);
		pin_state_ &= static_cast<u8>(~direction_);
		pin_state_ |= static_cast<u8>(write_latch_ & direction_);
		readPins();
		return;
	case kCtrlAddr:
		read_write_ = (value & 0x01u) != 0;
		return;
	default:
		return;
	}
}

void GpioRtc::outputPins(unsigned pins)
{
	pin_state_ &= direction_;
	pin_state_ |= static_cast<u8>((pins & ~direction_) & 0x0Fu);
}

void GpioRtc::readPins()
{
	// Latch the previous SIO level for the race the hardware has when SIO
	// changes on a rising SCK edge, then keep only it: the RTC pulls every
	// other pin low.
	outputPins(pin_state_ & 2);
	if ((pin_state_ & 4) == 0) {
		// CS low aborts the transfer; SIO idles high.
		rtc_bits_ = 0;
		rtc_bits_read_ = 0;
		rtc_remaining_ = 0;
		rtc_active_ = false;
		rtc_command_ = 0;
		rtc_sck_edge_ = true;
		rtc_sio_out_ = true;
		outputPins(2);
		return;
	}
	const bool sck = (pin_state_ & 1) != 0;
	const bool sio = (pin_state_ & 2) != 0;
	if (!rtc_active_) {
		// Receiving the command byte (sampled while SCK is low).
		outputPins(2);
		if (!sck) {
			rtc_bits_ &= static_cast<u8>(~(1u << rtc_bits_read_));
			if (sio)
				rtc_bits_ |= static_cast<u8>(1u << rtc_bits_read_);
		}
		if (!rtc_sck_edge_ && sck) {
			if (++rtc_bits_read_ == 8)
				beginCommand();
		}
	} else if ((rtc_command_ & 0x80u) == 0) {
		// Receiving a command payload byte; a pin that moved with SCK
		// contributes 0 (hardware race behaviour).
		outputPins(2);
		if (!sck) {
			rtc_bits_ &= static_cast<u8>(~(1u << rtc_bits_read_));
			if (sio)
				rtc_bits_ |= static_cast<u8>(1u << rtc_bits_read_);
		}
		if (!rtc_sck_edge_ && sck) {
			if ((((rtc_bits_ >> rtc_bits_read_) & 1u) ^ (sio ? 1u : 0u)) != 0)
				rtc_bits_ &= static_cast<u8>(~(1u << rtc_bits_read_));
			if (++rtc_bits_read_ == 8)
				processByte();
		}
	} else {
		// Sending payload bytes: SCK falling edge presents the next bit.
		if (rtc_sck_edge_ && !sck) {
			rtc_sio_out_ = outputBit() != 0;
			if (++rtc_bits_read_ == 8) {
				if (rtc_remaining_ > 0)
					--rtc_remaining_;
				if (rtc_remaining_ == 0)
					rtc_remaining_ = cmdBytes((rtc_command_ >> 4) & 7u);
				rtc_bits_read_ = 0;
			}
		}
		outputPins(rtc_sio_out_ ? 2u : 0u);
	}
	rtc_sck_edge_ = sck;
}

void GpioRtc::beginCommand()
{
	const u8 raw = rtc_bits_; // Full command byte, read flag included.
	rtc_bits_ = 0;
	rtc_bits_read_ = 0;
	if ((raw & 0x0Fu) != 0x06)
		return; // Not an RTC command; stay idle.
	const unsigned command = (raw >> 4) & 7u;
	rtc_command_ = raw;
	rtc_active_ = true;
	rtc_remaining_ = cmdBytes(command);
	switch (command) {
	case kCmdReset:
		rtc_control_ = 0;
		break;
	case kCmdDatetime:
	case kCmdTime:
		updateClock();
		break;
	default:
		break;
	}
}

unsigned GpioRtc::cmdBytes(unsigned command)
{
	switch (command) {
	case kCmdDatetime:
		return 7;
	case kCmdTime:
		return 3;
	case kCmdControl:
		return 1;
	default:
		return 0;
	}
}

void GpioRtc::processByte()
{
	if (((rtc_command_ >> 4) & 7u) == kCmdControl)
		rtc_control_ = rtc_bits_;
	// Other commands take no payload (reset/force-IRQ strobes, date/time
	// and force-IRQ are read-only here).
	rtc_bits_ = 0;
	rtc_bits_read_ = 0;
	if (rtc_remaining_ > 0)
		--rtc_remaining_;
	if (rtc_remaining_ == 0)
		rtc_remaining_ = cmdBytes((rtc_command_ >> 4) & 7u);
}

unsigned GpioRtc::outputBit()
{
	u8 byte = 0xFF;
	switch ((rtc_command_ >> 4) & 7u) {
	case kCmdControl:
		byte = rtc_control_;
		break;
	case kCmdDatetime:
	case kCmdTime:
		if (rtc_remaining_ <= 7)
			byte = rtc_time_[7 - rtc_remaining_];
		break;
	default:
		break;
	}
	return (byte >> rtc_bits_read_) & 1u;
}

void GpioRtc::updateClock()
{
	std::time_t t = std::time(nullptr);
	std::tm date{};
#if defined(_WIN32)
	localtime_s(&date, &t);
#else
	localtime_r(&t, &date);
#endif
	rtc_time_[0] = bcd(static_cast<unsigned>(date.tm_year - 100)); // year - 2000
	rtc_time_[1] = bcd(static_cast<unsigned>(date.tm_mon + 1));
	rtc_time_[2] = bcd(static_cast<unsigned>(date.tm_mday));
	rtc_time_[3] = bcd(static_cast<unsigned>(date.tm_wday));
	if ((rtc_control_ & 0x40u) != 0) {
		rtc_time_[4] = bcd(static_cast<unsigned>(date.tm_hour)); // 24h
	} else {
		rtc_time_[4] = bcd(static_cast<unsigned>(date.tm_hour % 12)); // 12h
	}
	rtc_time_[5] = bcd(static_cast<unsigned>(date.tm_min));
	rtc_time_[6] = bcd(static_cast<unsigned>(date.tm_sec));
}

u8 GpioRtc::bcd(unsigned value)
{
	return static_cast<u8>(((value / 10) << 4) | (value % 10));
}

void GpioRtc::save(StateBuffer &out) const
{
	out.write(present_);
	out.write(pin_state_);
	out.write(direction_);
	out.write(write_latch_);
	out.write(read_write_);
	out.write(rtc_bits_);
	out.write(rtc_bits_read_);
	out.write(rtc_remaining_);
	out.write(rtc_active_);
	out.write(rtc_command_);
	out.write(rtc_control_);
	out.writeBytes(rtc_time_, sizeof rtc_time_);
	out.write(rtc_sck_edge_);
	out.write(rtc_sio_out_);
}

void GpioRtc::load(const StateBuffer &in)
{
	in.read(present_);
	in.read(pin_state_);
	in.read(direction_);
	in.read(write_latch_);
	in.read(read_write_);
	in.read(rtc_bits_);
	in.read(rtc_bits_read_);
	in.read(rtc_remaining_);
	in.read(rtc_active_);
	in.read(rtc_command_);
	in.read(rtc_control_);
	in.readBytes(rtc_time_, sizeof rtc_time_);
	in.read(rtc_sck_edge_);
	in.read(rtc_sio_out_);
}

} // namespace gba
