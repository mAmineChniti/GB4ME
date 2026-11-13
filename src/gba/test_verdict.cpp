// jsmolka/gba-tests verdict reader: template matching for the mode-4
// verdict screens ("All tests passed" at (56,76), "Failed test NNN" at
// (60,76), 8px glyphs, black ink on white). Pure pixels-in/pixels-out:
// headless-testable, no device needed.

#include "gba/test_verdict.h"

namespace gba
{

namespace
{

// Verdict-alphabet glyphs: 8x8 1-bit rows (row r = byte r, LSB = leftmost
// pixel) taken from the font table inside shipping jsmolka/gba-tests ROMs
// (tonc-derived, MIT). NOTE: the repo's current lib/glyphs.asm has drifted
// for several lowercase glyphs, so these are the ROM bytes, verified
// pixel-for-pixel against a rendered verdict screen. Anything outside this
// subset reads as Unknown.
constexpr TestGlyph kGlyphs[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, // 0x00000000,0x00000000
	{'0', {0x3C, 0x66, 0x76, 0x7E, 0x6E, 0x66, 0x3C, 0x00}}, // 0x7E76663C,0x003C666E
	{'1', {0x18, 0x1C, 0x1E, 0x18, 0x18, 0x18, 0x18, 0x00}}, // 0x181E1C18,0x00181818
	{'2', {0x3C, 0x66, 0x60, 0x30, 0x18, 0x0C, 0x7E, 0x00}}, // 0x3060663C,0x007E0C18
	{'3', {0x3C, 0x66, 0x60, 0x38, 0x60, 0x66, 0x3C, 0x00}}, // 0x3860663C,0x003C6660
	{'4', {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x30, 0x00}}, // 0x33363C38,0x0030307F
	{'5', {0x7E, 0x06, 0x3E, 0x60, 0x60, 0x66, 0x3C, 0x00}}, // 0x603E067E,0x003C6660
	{'6', {0x38, 0x0C, 0x06, 0x3E, 0x66, 0x66, 0x3C, 0x00}}, // 0x3E060C38,0x003C6666
	{'7', {0x7E, 0x60, 0x60, 0x30, 0x18, 0x18, 0x18, 0x00}}, // 0x3060607E,0x00181818
	{'8', {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}}, // 0x3C66663C,0x003C6666
	{'9', {0x3C, 0x66, 0x66, 0x7C, 0x60, 0x30, 0x1C, 0x00}}, // 0x7C66663C,0x001C3060
	{'A', {0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}}, // 0x7E66663C,0x00666666
	{'F', {0x7E, 0x06, 0x06, 0x1E, 0x06, 0x06, 0x06, 0x00}}, // 0x1E06067E,0x00060606
	{'a', {0x00, 0x00, 0x3C, 0x60, 0x7C, 0x66, 0x7C, 0x00}}, // 0x603C0000,0x007C667C
	{'d', {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00}}, // 0x667C6060,0x007C6666
	{'e', {0x00, 0x00, 0x3C, 0x66, 0x7E, 0x06, 0x3C, 0x00}}, // 0x663E0606,0x00666666
	{'i', {0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x30, 0x00}}, // 0x7F370000,0x0063636B
	{'l', {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x30, 0x00}}, // 0x663E0000,0x06063E66
	{'p', {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x06}}, // 0x0C3E0C0C,0x00380C0C
	{'s', {0x00, 0x00, 0x3C, 0x06, 0x3C, 0x60, 0x3E, 0x00}}, // 0x63630000,0x00367F6B
	{'t', {0x0C, 0x0C, 0x3E, 0x0C, 0x0C, 0x0C, 0x38, 0x00}}, // 0x36630000,0x0063361C
};

bool ink(u32 px)
{
	const unsigned r = (px >> 16) & 0xFFu;
	const unsigned g = (px >> 8) & 0xFFu;
	const unsigned b = px & 0xFFu;
	return (r + g + b) / 3 < 128;
}

bool match_cell(const u32 *pixels, u32 w, u32 h, u32 x, u32 y, const u8 rows[8])
{
	if (x > w || y > h || 8 > w - x || 8 > h - y)
		return false;
	for (u32 r = 0; r < 8; r++) {
		for (u32 c = 0; c < 8; c++) {
			const bool want = ((rows[r] >> c) & 1) != 0;
			if (ink(pixels[(y + r) * w + (x + c)]) != want)
				return false;
		}
	}
	return true;
}

bool match_text(const u32 *pixels, u32 w, u32 h, u32 x, u32 y, const char *s)
{
	for (u32 i = 0; s[i] != '\0'; i++) {
		const TestGlyph *g = find_test_glyph(s[i]);
		if (g == nullptr)
			return false;
		if (!match_cell(pixels, w, h, x + 8 * i, y, g->rows))
			return false;
	}
	return true;
}

bool match_digit(const u32 *pixels, u32 w, u32 h, u32 x, u32 y, int *out)
{
	for (char d = '0'; d <= '9'; d++) {
		const TestGlyph *g = find_test_glyph(d);
		if (g != nullptr && match_cell(pixels, w, h, x, y, g->rows)) {
			*out = d - '0';
			return true;
		}
	}
	return false;
}

} // namespace

const TestGlyph *find_test_glyph(char ch)
{
	for (const auto &g : kGlyphs) {
		if (g.ch == ch)
			return &g;
	}
	return nullptr;
}

GbaTestVerdict parse_gba_test_verdict(const u32 *pixels, u32 w, u32 h)
{
	GbaTestVerdict r;
	if (pixels == nullptr)
		return r;
	if (match_text(pixels, w, h, 56, 76, "All tests passed")) {
		r.kind = GbaTestVerdictKind::Pass;
		return r;
	}
	if (match_text(pixels, w, h, 60, 76, "Failed test ")) {
		int hun = 0, ten = 0, one = 0;
		if (match_digit(pixels, w, h, 60 + 8 * 12, 76, &hun) &&
			match_digit(pixels, w, h, 60 + 8 * 13, 76, &ten) &&
			match_digit(pixels, w, h, 60 + 8 * 14, 76, &one)) {
			r.kind = GbaTestVerdictKind::Fail;
			r.failed_test = hun * 100 + ten * 10 + one;
		}
	}
	return r;
}

} // namespace gba
