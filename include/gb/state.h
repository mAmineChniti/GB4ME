#pragma once

#include "types.h"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <vector>

namespace gb
{

// Serialisation buffer for `GB4S` savestates, the GB/CGB counterpart of
// `gba/state.h`. A fixed-size byte buffer with a read cursor: every device
// writes its private state in a fixed order and reads it back the same way,
// so the format is positional, not tagged - cheap to write, and a version
// byte in the header is what keeps old files loadable.
//
// Deliberately a copy of the GBA writer rather than a shared header: the GBA
// side is covered by frame-exact A/B tests against real ROMs, and one
// gb->gba include in nine of its files is a worse trade than 50 duplicated
// lines. The two must stay in step - same magic, same layout, so a state
// written by either core could be told apart by a version byte if the
// savestate UI ever unifies them.
class StateBuffer
{
  public:
	template <typename T> void write(const T &value) const
	{
		static_assert(std::is_trivially_copyable_v<T>, "Only trivially copyable types supported");
		// memcpy, not std::copy: the reinterpret_cast pointer form makes
		// GCC emit a bogus -Warray-bounds in sanitizer builds.
		const u8 *bytes = reinterpret_cast<const u8 *>(&value);
		data_.insert(data_.end(), bytes, bytes + sizeof(T));
	}

	template <typename T> bool read(T &out) const
	{
		static_assert(std::is_trivially_copyable_v<T>, "Only trivially copyable types supported");
		if (sizeof(T) > data_.size() - pos_) {
			ok_ = false;
			return false;
		}
		std::memcpy(&out, data_.data() + pos_, sizeof(T));
		pos_ += sizeof(T);
		return true;
	}

	void writeBytes(const u8 *src, size_t len) const
	{
		data_.insert(data_.end(), src, src + len);
	}

	bool readBytes(u8 *dst, size_t len) const
	{
		// pos_ <= data_.size() is an invariant; comparing the remaining
		// length instead of pos_ + len cannot overflow on a huge input.
		if (len > data_.size() - pos_) {
			ok_ = false;
			return false;
		}
		std::memcpy(dst, data_.data() + pos_, len);
		pos_ += len;
		return true;
	}

	// Fixed-capacity storage (WRAM/VRAM/OAM/HRAM, nested or not) is written
	// raw with a length prefix, so a state from a build that had a different
	// internal layout is caught by the size check instead of running off
	// the end of the buffer.
	template <typename T, size_t N> void writeArray(const std::array<T, N> &arr) const
	{
		write<u32>(static_cast<u32>(N));
		writeBytes(reinterpret_cast<const u8 *>(arr.data()), N * sizeof(T));
	}

	template <typename T, size_t N> bool readArray(std::array<T, N> &arr) const
	{
		u32 size = 0;
		if (!read(size))
			return false;
		if (size != N)
			return false;
		return readBytes(reinterpret_cast<u8 *>(arr.data()), N * sizeof(T));
	}

	// The same, for the CGB's 8-banked WRAM (a std::array of std::array).
	template <typename T, size_t N, size_t M>
	void writeArray2(const std::array<std::array<T, N>, M> &arr) const
	{
		write<u32>(static_cast<u32>(M));
		writeBytes(reinterpret_cast<const u8 *>(arr.data()), M * N * sizeof(T));
	}

	template <typename T, size_t N, size_t M>
	bool readArray2(std::array<std::array<T, N>, M> &arr) const
	{
		u32 size = 0;
		if (!read(size))
			return false;
		if (size != M)
			return false;
		return readBytes(reinterpret_cast<u8 *>(arr.data()), M * N * sizeof(T));
	}

	// std::vector-backed storage (cartridge RAM) can change size with the
	// ROM, so its length travels with it.
	template <typename T> void writeVector(const std::vector<T> &vec) const
	{
		write<u32>(static_cast<u32>(vec.size()));
		if (!vec.empty())
			writeBytes(reinterpret_cast<const u8 *>(vec.data()), vec.size() * sizeof(T));
	}

	template <typename T> bool readVector(std::vector<T> &out) const
	{
		u32 size = 0;
		if (!read(size))
			return false;
		// Validate against the bytes actually present *before* resizing:
		// a corrupt length must not turn a small state file into a huge
		// allocation. Dividing avoids any size * sizeof(T) overflow.
		static_assert(sizeof(T) != 0, "zero-sized element");
		if (size > (data_.size() - pos_) / sizeof(T)) {
			ok_ = false;
			return false;
		}
		out.resize(size);
		if (size == 0)
			return true;
		return readBytes(reinterpret_cast<u8 *>(out.data()), size * sizeof(T));
	}

	const std::vector<u8> &data() const
	{
		return data_;
	}
	void data(const std::vector<u8> &d) const
	{
		data_ = d;
		pos_ = 0;
		ok_ = true;
	}
	size_t position() const
	{
		return pos_;
	}
	// True until a typed/byte read runs out of input. Component loaders are
	// void, so the buffer itself records a short/corrupt state.
	bool good() const
	{
		return ok_;
	}
	// A well-formed state is consumed exactly: trailing bytes mean the file
	// was not written by this layout.
	bool at_end() const
	{
		return pos_ == data_.size();
	}

  private:
	mutable std::vector<u8> data_;
	mutable size_t pos_ = 0;
	mutable bool ok_ = true;
};

constexpr u32 kStateMagic = 0x47423453; // "GB4S"
// v1: the initial GB/CGB state (cpu/timer/joypad/mmu/ppu/apu/cartridge).
// Bump when a field is ADDED; removing or reordering a field needs a new
// major layout too, since the format is positional.
// v2: CPU::double_speed_phase_ (the CPU-driven per-M-cycle hook keeps its
// half-rate phase for CGB double speed).
// v3: PPU::stat_line_ (the STAT interrupt line level, so a state reloaded
// mid-mode does not fake a rising edge).
// v4: Timer::tima_reload_ (the TIMA overflow reload phase), so a reload
// interrupted by a save does not lose or duplicate its timer interrupt.
// v5: MMU::serial_count_/serial_master_clock_/serial_mask_ (the serial port is
// now divider-clocked, so a transfer can be in flight when a state is saved).
// v6: CPU::dma_stall_ (the OAM DMA bus stall the CPU is still inside).
// 7: APU's wave_form_just_read flag (the DMG wave-RAM bus window is the one
// cycle the DAC fetched on, not the four cycles before it).
constexpr u32 kStateVersion = 7;

} // namespace gb
