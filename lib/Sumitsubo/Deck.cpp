#include "Deck.h"

#include <cstdlib>

namespace sumitsubo {

std::vector<std::string> split(const std::string& s, const char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t end = s.find(sep, start);
    out.emplace_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

uint64_t parseId(const std::string& hex) { return std::strtoull(hex.c_str(), nullptr, 16); }

Kind kindOf(const std::string& c) {
  if (c == "W") return Kind::Word;
  if (c == "K") return Kind::Kanji;
  if (c == "C") return Kind::Colloc;
  if (c == "X") return Kind::Contrast;
  if (c == "L") return Kind::Listen;
  return Kind::Unknown;
}

// \x01 tested \x02, \x03 base \x04 reading \x05 (see device.py on the desktop).
static void parseRuns(const std::string& text, std::vector<Run>& runs) {
  Run cur;
  bool target = false;
  auto flush = [&] {
    if (!cur.text.empty()) runs.push_back(cur);
    cur = Run{};
    cur.target = target;
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\x01' || c == '\x02') {
      flush();
      target = c == '\x01';
      cur.target = target;
    } else if (c == '\x03') {
      flush();
      const size_t mid = text.find('\x04', i), end = text.find('\x05', i);
      if (mid == std::string::npos || end == std::string::npos || mid > end) continue;
      Run r;
      r.text = text.substr(i + 1, mid - i - 1);
      r.ruby = text.substr(mid + 1, end - mid - 1);
      r.target = target;
      runs.push_back(r);
      i = end;
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      cur.text += c;
    }
  }
  flush();
}

Sentence parseSentence(const std::string& packed, const bool withSource) {
  Sentence s;
  if (packed.empty()) return s;
  std::string text = packed;
  if (withSource) {
    const size_t a = packed.find('~'), b = a == std::string::npos ? a : packed.find('~', a + 1);
    if (b == std::string::npos) return s;
    s.source = std::atoi(packed.substr(0, a).c_str());
    s.place = packed.substr(a + 1, b - a - 1);
    text = packed.substr(b + 1);
  }
  parseRuns(text, s.runs);
  return s;
}

bool parseCard(const std::string& line, Card& out) {
  const auto col = split(line, '\t');
  if (col.size() < 12) return false;
  out = Card{};
  out.id = col[0];
  out.kind = kindOf(col[1]);
  out.head = col[7];
  out.reading = col[8];
  out.gloss = col[10];
  out.extra = col[11];
  const auto items = col[9].empty() ? std::vector<std::string>{} : split(col[9], '\x1f');
  switch (out.kind) {
    case Kind::Word:
    case Kind::Colloc:
    case Kind::Listen:
      for (const auto& it : items) {
        Sentence s = parseSentence(it, true);
        if (!s.empty()) out.sentences.push_back(std::move(s));
      }
      break;
    case Kind::Kanji:
      for (const auto& it : items) {
        const auto f = split(it, '~');
        if (f.size() >= 3) out.words.push_back({f[0], f[1], f[2]});
      }
      out.sentence = parseSentence(col[11], true);
      out.extra.clear();
      break;
    case Kind::Contrast:
      for (const auto& it : items) {
        const auto f = split(it, '~');
        if (f.size() < 5) continue;
        Member m{f[0], f[1], f[2], f[3], f[4], {}};
        if (f.size() >= 6) m.sentence = parseSentence(f[5], false);
        out.members.push_back(std::move(m));
      }
      break;
    default:
      return false;
  }
  return true;
}

}  // namespace sumitsubo
