#pragma once

#include "types.h"
#include <string>
#include <vector>

namespace gb
{

// GameShark / raw cheats for GB/CGB (Pro Action Replay-style raw patches).
// Supported `<rom>.cheats` line formats (hex, case-insensitive, `#` comments):
//   C000=FF      raw patch: reads from 0xC000 return 0xFF
//   01YYXXXX     GameShark type 01: value YY at address XXXX (e.g. 0199C2A5)
// Only type 01 (8-bit write, no compare) is supported; anything else is
// rejected so a mistyped code can never silently do the wrong thing.
// Applied in MMU::read (the bus choke point), enabled flag per entry plus a
// global toggle (C key) in the frontend. Never part of savestates.
struct Cheat {
	u16 addr = 0;
	u8 value = 0;
	bool enabled = true;
	std::string code;
};

class Cheats
{
  public:
	// Parses one line (already trimmed, comments stripped). Returns true and
	// appends on success; false leaves the list unchanged.
	bool add_line(const std::string &line);
	// Loads a `<rom>.cheats` file (missing file = empty list, not an error).
	// Returns the number of entries loaded.
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
	// Override for a bus read: returns the cheat value when an enabled
	// entry matches, otherwise the original byte.
	u8 apply(u16 addr, u8 orig) const;

  private:
	static bool parse_hex(const std::string &s, u32 &out);
	static std::string trim(const std::string &s);
	std::vector<Cheat> entries_;
	bool global_on_ = true;
};

} // namespace gb
