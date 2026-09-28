// GBA cartridge GPIO + SII RTC (GBATEK "GBA Cart I/O Port (GPIO)").
// Behavioural model of the register/pin/protocol docs: 4-bit DATA (with
// direction mask), 4-bit DIRECTION, 1-bit CONTROL (write-only vs
// read/write), and the SII 3-wire serial engine (SCK/SIO/CS) serving the
// reset/datetime/time/control commands off host local time.

#include "gba/gpio.h"

#include <cstdio>
#include <ctime>
#include <fstream>

namespace gba
{

namespace
{

// Command numbers (bits 6-4 of the command byte).
constexpr unsigned kCmdReset = 0;
constexpr unsigned kCmdDatetime = 2;
constexpr unsigned kCmdControl = 4;
constexpr unsigned kCmdTime = 6;

// Days from 1970-01-01 (Howard Hinnant's civil algorithm). Used instead of
// timegm()/mktime() so the emulated RTC stays independent of the host
// timezone and DST rules.
std::int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
	year -= month <= 2;
	const int era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(year - era * 400);
	const unsigned doy = (153u * (month + (month > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
	const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
	return static_cast<std::int64_t>(era) * 146097 + static_cast<int>(doe) - 719468;
}

// Seconds for a broken-down local wall time, as if it were UTC. The
// offset is a difference of two such values, so any constant bias
// cancels; only the day/hour arithmetic has to be right.
std::int64_t epochLike(int year, int month, int day, int hour, int minute, int second)
{
	return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
		   hour * 3600 + minute * 60 + second;
}

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
	rtc_write_len_ = 0;
	for (u8 &b : rtc_write_)
		b = 0;
	// offset_ survives a reset: the clock is battery-backed, so a soft
	// reset must not undo a game-set time.
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
		abortTransfer();
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

void GpioRtc::abortTransfer()
{
	rtc_bits_ = 0;
	rtc_bits_read_ = 0;
	rtc_remaining_ = 0;
	rtc_active_ = false;
	rtc_command_ = 0;
	rtc_write_len_ = 0;
	rtc_sck_edge_ = true;
	rtc_sio_out_ = true;
	outputPins(2);
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
	rtc_write_len_ = 0;
	switch (command) {
	case kCmdReset:
		rtc_control_ = 0;
		break;
	case kCmdDatetime:
	case kCmdTime:
		// A read latches the running clock; a write collects the payload
		// and rebases the clock once it is complete.
		if ((raw & 0x80u) != 0)
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
	const unsigned command = (rtc_command_ >> 4) & 7u;
	const bool reading = (rtc_command_ & 0x80u) != 0;
	if (command == kCmdControl) {
		rtc_control_ = rtc_bits_;
	} else if (!reading && (command == kCmdDatetime || command == kCmdTime) &&
			   rtc_write_len_ < sizeof rtc_write_) {
		// Date/time write payload, same register order as a read
		// (year, month, day, day-of-week, hour, minute, second).
		rtc_write_[rtc_write_len_++] = rtc_bits_;
	}
	const bool last = rtc_remaining_ <= 1;
	// Reset/force-IRQ strobes take no payload; a control write is
	// applied above, a date/time write once its last byte is in.
	rtc_bits_ = 0;
	rtc_bits_read_ = 0;
	if (rtc_remaining_ > 0)
		--rtc_remaining_;
	if (!reading && last && rtc_write_len_ == cmdBytes(command))
		applyWrite(command);
	if (rtc_remaining_ == 0)
		rtc_remaining_ = cmdBytes(command);
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
	// Host local time, shifted by the battery-backed offset a game may
	// have written. localtime_r on the shifted epoch keeps DST behaviour
	// consistent with the host clock.
	const std::time_t t =
		static_cast<std::time_t>(static_cast<std::int64_t>(std::time(nullptr)) + offset_);
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

bool GpioRtc::fromBcd(unsigned value, int *out)
{
	const int hi = static_cast<int>((value >> 4) & 0x0Fu);
	const int lo = static_cast<int>(value & 0x0Fu);
	if (hi > 9 || lo > 9)
		return false;
	*out = hi * 10 + lo;
	return true;
}

void GpioRtc::clockNow(int *year, int *month, int *day, int *weekday, int *hour, int *min,
					   int *sec) const
{
	const std::time_t t =
		static_cast<std::time_t>(static_cast<std::int64_t>(std::time(nullptr)) + offset_);
	std::tm date{};
#if defined(_WIN32)
	localtime_s(&date, &t);
#else
	localtime_r(&t, &date);
#endif
	*year = date.tm_year + 1900;
	*month = date.tm_mon + 1;
	*day = date.tm_mday;
	*weekday = date.tm_wday;
	*hour = date.tm_hour;
	*min = date.tm_min;
	*sec = date.tm_sec;
}

void GpioRtc::applyWrite(unsigned command)
{
	// Payload uses the same register order as a date/time read: year,
	// month, day, day-of-week, hour, minute, second. A time-only write
	// (command 6) sends the last three and keeps the host date.
	int second = 0, minute = 0, hour = 0, day = 0, month = 0, year = 0;
	int hy = 0, hm = 0, hd = 0, hwd = 0, hh = 0, hmi = 0, hs = 0;
	clockNow(&hy, &hm, &hd, &hwd, &hh, &hmi, &hs);
	if (command == kCmdTime) {
		year = hy;
		month = hm;
		day = hd;
		if (!fromBcd(rtc_write_[0], &hour) || !fromBcd(rtc_write_[1], &minute) ||
			!fromBcd(rtc_write_[2], &second))
			return; // Malformed BCD: keep the running clock.
	} else {
		int year_bcd = 0;
		if (!fromBcd(rtc_write_[0], &year_bcd) || !fromBcd(rtc_write_[1], &month) ||
			!fromBcd(rtc_write_[2], &day) || !fromBcd(rtc_write_[4], &hour) ||
			!fromBcd(rtc_write_[5], &minute) || !fromBcd(rtc_write_[6], &second))
			return;
		year = year_bcd + 2000;
	}
	if (second > 59 || minute > 59 || hour > 23 || month < 1 || month > 12 || day < 1 || day > 31)
		return;
	if ((rtc_control_ & 0x40u) == 0 && hour >= 1 && hour <= 12) {
		// 12-hour mode: the register holds 1-12 and the chip keeps the
		// current AM/PM, so borrow the host clock's half of the day.
		hour = (hh >= 12) ? hour + 12 : hour;
	}
	// Smallest shift that lands the running clock on the written time.
	// The base is the *emulated* now, so this composes with an offset a
	// previous write already installed.
	offset_ +=
		epochLike(year, month, day, hour, minute, second) - epochLike(hy, hm, hd, hh, hmi, hs);
	rtc_write_len_ = 0;
	updateClock();
}

void GpioRtc::saveToFile(const std::string &path) const
{
	if (!present_)
		return;
	std::ofstream file(path, std::ios::trunc);
	if (!file)
		return;
	file << "GB4ME-GPARTC-1 " << offset_ << '\n';
}

void GpioRtc::loadFromFile(const std::string &path)
{
	if (!present_)
		return;
	std::ifstream file(path);
	if (!file)
		return;
	std::string tag;
	long long off = 0;
	if (!(file >> tag >> off) || tag != "GB4ME-GPARTC-1")
		return;
	// A game may set any date, so allow a century either way; outside
	// that the sidecar is corrupt.
	constexpr long long kMaxOffset = 100LL * 365LL * 24LL * 3600LL;
	if (off < -kMaxOffset || off > kMaxOffset)
		return;
	offset_ = off;
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
	// Battery-backed clock state (newer states only).
	out.write(rtc_write_len_);
	out.writeBytes(rtc_write_, sizeof rtc_write_);
	out.write(offset_);
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
	u8 len = 0;
	if (in.read(len))
		rtc_write_len_ = len;
	in.readBytes(rtc_write_, sizeof rtc_write_);
	std::int64_t off = 0;
	if (in.read(off))
		offset_ = off;
}

} // namespace gba
