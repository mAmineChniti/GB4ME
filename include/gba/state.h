#pragma once

#include "gb/types.h"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <vector>

namespace gba
{

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
constexpr u32 kStateVersion = 2;        // v2 adds cartridge GPIO/RTC state.

} // namespace gba
