#pragma once

#include "gb/types.h"
#include <algorithm>
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
		const u8 *bytes = reinterpret_cast<const u8 *>(&value);
		data_.insert(data_.end(), bytes, bytes + sizeof(T));
	}

	template <typename T> bool read(T &out) const
	{
		static_assert(std::is_trivially_copyable_v<T>, "Only trivially copyable types supported");
		if (pos_ + sizeof(T) > data_.size())
			return false;
		std::copy(data_.begin() + pos_, data_.begin() + pos_ + sizeof(T),
				  reinterpret_cast<u8 *>(&out));
		pos_ += sizeof(T);
		return true;
	}

	void writeBytes(const u8 *src, size_t len) const
	{
		data_.insert(data_.end(), src, src + len);
	}

	bool readBytes(u8 *dst, size_t len) const
	{
		if (pos_ + len > data_.size())
			return false;
		std::copy(data_.begin() + pos_, data_.begin() + pos_ + len, dst);
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
	}
	size_t position() const
	{
		return pos_;
	}

  private:
	mutable std::vector<u8> data_;
	mutable size_t pos_ = 0;
};

constexpr u32 kStateMagic = 0x47423453; // "GB4S"
constexpr u32 kStateVersion = 2; // v2 adds cartridge GPIO/RTC state.

} // namespace gba
