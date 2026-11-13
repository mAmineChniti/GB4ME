#pragma once

#include "gb/types.h"
#include <cstddef>

namespace gba
{

// jsmolka/gba-tests verdict reader. Those ROMs report on screen (mode 4):
// "All tests passed" at (56,76), or "Failed test NNN" at (60,76), drawn
// with their 8px bitmap font (black ink on white). This matches those two
// templates pixel-exactly against a framebuffer - no OCR, no guessing - so
// anything else (mid-run screens, other homebrew) reads as Unknown.
// Glyph bits below are that font (lib/glyphs.asm, MIT, tonc-derived),
// subset to the verdict alphabet: row r = byte r, bit k (LSB) = pixel k.
struct TestGlyph {
	char ch = 0;
	u8 rows[8] = {};
};

const TestGlyph *find_test_glyph(char ch);

enum class GbaTestVerdictKind : u8 { Unknown = 0, Pass = 1, Fail = 2 };

struct GbaTestVerdict {
	GbaTestVerdictKind kind = GbaTestVerdictKind::Unknown;
	int failed_test = 0; // Valid only for Fail (0-999 as printed).
};

GbaTestVerdict parse_gba_test_verdict(const u32 *pixels, u32 w, u32 h);

} // namespace gba
