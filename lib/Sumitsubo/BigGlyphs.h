#pragma once
// The characters shown large on a kanji card, drawn by the desktop at their real size in four
// levels of grey (deck/big.bin, run-length coded; see device.py, write_big). The device has no memory to draw
// outlines itself, and its 18-point letters enlarged have ragged edges.
#include <cstdint>
#include <string>
#include <vector>

#include "Store.h"

namespace sumitsubo {
constexpr const char* BIG_FILE = "/sumitsubo/deck/big.bin";

struct BigGlyph {
  uint32_t codepoint = 0;
  int width = 0, height = 0, left = 0, top = 0, advance = 0;
  std::string pixels;  // two bits each, row by row
  // 0 white, 1 light grey, 2 dark grey, 3 black
  uint8_t level(const int x, const int y) const {
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
    return (static_cast<uint8_t>(pixels[i >> 2]) >> ((3 - (i & 3)) * 2)) & 3;
  }
};

class BigGlyphs {
 public:
  bool open(Store& store);                             // false: no file, the caller enlarges its own font
  bool ready() const { return count > 0; }
  int em() const { return emPx; }
  int lineHeight() const { return lineH; }
  const BigGlyph* find(uint32_t codepoint);            // nullptr when the file does not have it; kept until a few more are asked for

 private:
  Store* store = nullptr;
  uint32_t count = 0;
  int emPx = 0, lineH = 0;
  std::vector<BigGlyph> recent;                        // the word on the card: read once, drawn several times
};

uint32_t firstCodepoint(const std::string& utf8);
}  // namespace sumitsubo
