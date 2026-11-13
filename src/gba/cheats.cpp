#include "gba/cheats.h"

#include <cctype>
#include <fstream>

namespace gba
{

std::string Cheats::trim(const std::string &s)
{
	size_t a = 0;
	while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
		a++;
	size_t b = s.size();
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		b--;
	return s.substr(a, b - a);
}

bool Cheats::parse_hex(const std::string &s, u32 &out)
{
	if (s.empty() || s.size() > 8)
		return false;
	u32 v = 0;
	for (char c : s) {
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= static_cast<u32>(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= static_cast<u32>(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= static_cast<u32>(c - 'A' + 10);
		else
			return false;
	}
	out = v;
	return true;
}

bool Cheats::add_line(const std::string &line)
{
	std::string t = trim(line);
	const size_t hash = t.find('#');
	if (hash != std::string::npos)
		t = trim(t.substr(0, hash));
	if (t.empty())
		return false;
	const size_t eq = t.find('=');
	if (eq == std::string::npos)
		return false;
	const std::string a = trim(t.substr(0, eq));
	const std::string v = trim(t.substr(eq + 1));
	// GBA addresses are 5-8 hex digits (4-digit GB lines go to the GB core).
	if (a.size() < 5 || a.size() > 8 || v.empty() || v.size() > 8)
		return false;
	u32 addr = 0, val = 0;
	if (!parse_hex(a, addr) || !parse_hex(v, val))
		return false;
	u8 width = 1;
	if (v.size() <= 2)
		width = 1;
	else if (v.size() <= 4)
		width = 2;
	else
		width = 4;
	if (width == 1 && val > 0xFF)
		return false;
	if (width == 2 && val > 0xFFFF)
		return false;
	entries_.push_back(Cheat{addr, val, width, true, t});
	return true;
}

size_t Cheats::load_file(const std::string &path)
{
	clear();
	std::ifstream f(path);
	if (!f)
		return 0;
	std::string line;
	while (std::getline(f, line))
		add_line(line);
	return entries_.size();
}

size_t Cheats::active_count() const
{
	if (!global_on_)
		return 0;
	size_t n = 0;
	for (const auto &c : entries_)
		n += c.enabled ? 1 : 0;
	return n;
}

u8 Cheats::apply8(u32 addr, u8 orig) const
{
	if (!global_on_ || entries_.empty())
		return orig;
	for (const auto &c : entries_) {
		if (c.enabled && c.width == 1 && c.addr == addr)
			return static_cast<u8>(c.value);
	}
	return orig;
}

u16 Cheats::apply16(u32 addr, u16 orig) const
{
	if (!global_on_ || entries_.empty())
		return orig;
	addr &= ~1u;
	for (const auto &c : entries_) {
		if (c.enabled && c.width == 2 && (c.addr & ~1u) == addr)
			return static_cast<u16>(c.value);
	}
	return orig;
}

u32 Cheats::apply32(u32 addr, u32 orig) const
{
	if (!global_on_ || entries_.empty())
		return orig;
	addr &= ~3u;
	for (const auto &c : entries_) {
		if (c.enabled && c.width == 4 && (c.addr & ~3u) == addr)
			return c.value;
	}
	return orig;
}

} // namespace gba
