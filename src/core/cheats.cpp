#include "gb/cheats.h"

#include <cctype>
#include <fstream>

namespace gb
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
	// Raw ADDR=VALUE (ADDR 1-4 hex, VALUE 1-2 hex).
	const size_t eq = t.find('=');
	if (eq != std::string::npos) {
		const std::string a = trim(t.substr(0, eq));
		const std::string v = trim(t.substr(eq + 1));
		if (a.empty() || a.size() > 4 || v.empty() || v.size() > 2)
			return false;
		u32 addr = 0, val = 0;
		if (!parse_hex(a, addr) || !parse_hex(v, val) || addr > 0xFFFF || val > 0xFF)
			return false;
		entries_.push_back(Cheat{static_cast<u16>(addr), static_cast<u8>(val), true, t});
		return true;
	}
	// GameShark 8 hex digits, type 01 only: 01YYXXXX.
	std::string h;
	for (char c : t) {
		if (c == '-' || c == ' ' || c == ':')
			continue;
		h += c;
	}
	if (h.size() != 8)
		return false;
	u32 raw = 0;
	if (!parse_hex(h, raw))
		return false;
	if (((raw >> 24) & 0xFF) != 0x01)
		return false;
	const u8 value = static_cast<u8>((raw >> 16) & 0xFF);
	const u16 addr = static_cast<u16>(raw & 0xFFFF);
	entries_.push_back(Cheat{addr, value, true, t});
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

u8 Cheats::apply(u16 addr, u8 orig) const
{
	if (!global_on_ || entries_.empty())
		return orig;
	for (const auto &c : entries_) {
		if (c.enabled && c.addr == addr)
			return c.value;
	}
	return orig;
}

} // namespace gb
