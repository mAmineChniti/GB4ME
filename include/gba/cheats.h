#pragma once

#include "gb/types.h"
#include <string>
#include <vector>

namespace gba
{

// Raw cheats for GBA (Action Replay / CodeBreaker encryption deliberately
// out of scope). `<rom>.cheats` line format (hex, `#` comments):
//   02000000=FF        8-bit patch at 0x02000000
//   02000000=1234      16-bit patch (3-4 value digits)
//   02000000=12345678  32-bit patch (5-8 value digits)
// Address must be 5-8 hex digits (GB 16-bit `C000=FF` lines are ignored here,
// and GBA lines are ignored by the GB parser, so one file serves both cores).
// Applied in GbaBus reads, never in the IO region or savestates.
struct Cheat {
	u32 addr = 0;
	u32 value = 0;
	u8 width = 1; // 1, 2 or 4 bytes
	bool enabled = true;
	std::string code;
};

class Cheats
{
  public:
	bool add_line(const std::string &line);
	size_t load_file(const std::string &path);
	void clear()
	{
		entries_.clear();
	}
	bool empty() const
	{
		return entries_.empty();
	}
	size_t size() const
	{
		return entries_.size();
	}
	size_t active_count() const;
	const std::vector<Cheat> &entries() const
	{
		return entries_;
	}
	void set_enabled(bool v)
	{
		global_on_ = v;
	}
	bool enabled() const
	{
		return global_on_;
	}
	u8 apply8(u32 addr, u8 orig) const;
	u16 apply16(u32 addr, u16 orig) const;
	u32 apply32(u32 addr, u32 orig) const;

  private:
	static bool parse_hex(const std::string &s, u32 &out);
	static std::string trim(const std::string &s);
	std::vector<Cheat> entries_;
	bool global_on_ = true;
};

} // namespace gba
