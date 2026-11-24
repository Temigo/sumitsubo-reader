#include "BigGlyphs.h"

namespace sumitsubo {
namespace {
uint32_t u32(const std::string& s, const size_t at) {
  return static_cast<uint8_t>(s[at]) | static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3])) << 24;
}
constexpr size_t KEEP = 8;
}  // namespace

uint32_t firstCodepoint(const std::string& s) {
  if (s.empty()) return 0;
  const auto b = [&](const size_t i) { return i < s.size() ? static_cast<uint8_t>(s[i]) : 0u; };
  const uint32_t c = b(0);
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) return (c & 0x1F) << 6 | (b(1) & 0x3F);
  if ((c & 0xF0) == 0xE0) return (c & 0x0F) << 12 | (b(1) & 0x3F) << 6 | (b(2) & 0x3F);
  return (c & 0x07) << 18 | (b(1) & 0x3F) << 12 | (b(2) & 0x3F) << 6 | (b(3) & 0x3F);
}

bool BigGlyphs::open(Store& s) {
  store = &s;
  count = 0;
  recent.clear();
  std::string head;
  if (!s.readAt(BIG_FILE, 0, 12, head) || head.compare(0, 4, "SBG2") != 0) return false;
  emPx = static_cast<uint8_t>(head[4]) | static_cast<uint8_t>(head[5]) << 8;
  lineH = static_cast<uint8_t>(head[6]) | static_cast<uint8_t>(head[7]) << 8;
  count = u32(head, 8);
  if (emPx < 8 || emPx > 255 || lineH < emPx / 2 || count > 100000) count = 0;
  return count > 0;
}

const BigGlyph* BigGlyphs::find(const uint32_t cp) {
  if (!count || !store) return nullptr;
  for (const auto& g : recent)
    if (g.codepoint == cp) return g.width ? &g : nullptr;
  BigGlyph g;
  g.codepoint = cp;  // kept even when missing, so the card does not search the file on every draw
  uint32_t lo = 0, hi = count;
  std::string buf;
  while (lo < hi) {  // the index is sorted: a dozen short reads
    const uint32_t mid = lo + (hi - lo) / 2;
    if (!store->readAt(BIG_FILE, 12 + mid * 8, 8, buf)) break;
    const uint32_t at = u32(buf, 0);
    if (at == cp) {
      const uint32_t offset = u32(buf, 4);
      std::string rec;
      if (store->readAt(BIG_FILE, offset, 7, rec)) {
        const int w = static_cast<uint8_t>(rec[0]), h = static_cast<uint8_t>(rec[1]);
        const size_t coded = static_cast<uint8_t>(rec[5]) | static_cast<size_t>(static_cast<uint8_t>(rec[6])) << 8;
        const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::string runs;
        if (w && h && store->readAt(BIG_FILE, offset + 7, coded, runs)) {
          // runs of one level, back into two bits a pixel
          g.pixels.assign((pixels + 3) / 4, '\0');
          size_t at = 0;
          for (const char c : runs) {
            const uint8_t level = static_cast<uint8_t>(c) >> 6;
            for (size_t n = (static_cast<uint8_t>(c) & 63) + 1u; n > 0 && at < pixels; --n, ++at)
              g.pixels[at >> 2] = static_cast<char>(static_cast<uint8_t>(g.pixels[at >> 2]) | level << ((3 - (at & 3)) * 2));
          }
          if (at != pixels) g.pixels.clear();
        }
        if (!g.pixels.empty()) {
          g.width = w;
          g.height = h;
          g.left = static_cast<int8_t>(rec[2]);
          g.top = static_cast<int8_t>(rec[3]);
          g.advance = static_cast<uint8_t>(rec[4]);
        }
      }
      break;
    }
    if (at < cp) lo = mid + 1;
    else hi = mid;
  }
  if (recent.size() >= KEEP) recent.erase(recent.begin());
  recent.push_back(std::move(g));
  return recent.back().width ? &recent.back() : nullptr;
}
}  // namespace sumitsubo
