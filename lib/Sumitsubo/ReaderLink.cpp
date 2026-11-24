#include "ReaderLink.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Deck.h"

namespace sumitsubo {

bool studyWantsTilt = false;

uint64_t WordFilter::hash(const std::string_view key) {  // FNV-1a, 64 bits
  uint64_t h = 0xcbf29ce484222325ULL;
  for (const unsigned char c : key) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  return h;
}

bool WordFilter::parse(const std::string& file) {
  clear();
  m = 0;
  k = 0;
  if (file.size() < 10 || file.compare(0, 4, "SBL1") != 0) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(file.data());
  const uint32_t bitsN = p[4] | (p[5] << 8) | (p[6] << 16) | (static_cast<uint32_t>(p[7]) << 24);
  const size_t bytes = (static_cast<size_t>(bitsN) + 7) / 8;
  if (bitsN == 0 || p[8] == 0 || bytes > MAX_FILTER_BYTES || file.size() < 9 + bytes) return false;
  bits.assign(p + 9, p + 9 + bytes);
  m = bitsN;
  k = p[8];
  return true;
}

bool WordFilter::has(const std::string_view key) const {
  if (bits.empty()) return false;
  uint64_t h = hash(key);  // then mixed, as in device.py: FNV alone leaves like words too alike
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  const uint64_t a = h & 0xffffffffULL, b = (h >> 32) | 1ULL;
  for (uint8_t i = 0; i < k; ++i) {
    const uint64_t bit = (a + i * b) % m;
    if (!(bits[bit >> 3] & (1u << (bit & 7)))) return false;
  }
  return true;
}

Prefs Prefs::load(Store& store) {
  Prefs p;
  std::string text;
  if (!store.read(PREFS_FILE, text)) return p;
  for (const auto& line : split(text, '\n')) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const bool on = line.size() > eq + 1 && line[eq + 1] == '1';
    if (key == "hints") p.hints = on;
    if (key == "smooth") p.smooth = on;
    if (key == "mark") p.mark = std::atoi(line.c_str() + eq + 1);
    else if (key == "mark_new") p.markNew = on;
    else if (key == "furigana_needed") p.furiganaNeeded = on;
    else if (key == "tilt") p.tilt = on;
  }
  return p;
}

bool Prefs::save(Store& store) const {
  return store.write(PREFS_FILE, std::string("hints=") + (hints ? "1" : "0") + "\nmark_new=" + (markNew ? "1" : "0") +
                                     "\nfurigana_needed=" + (furiganaNeeded ? "1" : "0") + "\ntilt=" + (tilt ? "1" : "0") + "\nmark=" + std::to_string(mark) + "\nsmooth=" + (smooth ? "1" : "0") + "\n");
}

std::string hiragana(const std::string_view kana) {
  std::string out;
  out.reserve(kana.size());
  for (size_t i = 0; i < kana.size();) {
    const unsigned char c = static_cast<unsigned char>(kana[i]);
    if (c == 0xE3 && i + 2 < kana.size()) {  // U+3000..U+3FFF: katakana U+30A1..U+30F6 sits 0x60 above hiragana
      const uint32_t cp = 0x3000u | ((static_cast<unsigned char>(kana[i + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(kana[i + 2]) & 0x3F);
      if (cp >= 0x30A1 && cp <= 0x30F6) {
        const uint32_t h = cp - 0x60;
        out += static_cast<char>(0xE3);
        out += static_cast<char>(0x80 | ((h >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (h & 0x3F));
        i += 3;
        continue;
      }
    }
    out += kana[i++];
  }
  return out;
}

namespace link {
namespace {
struct Book {
  bool known = false;
  int readable = -1, fresh = 0;
  size_t longest = 0;
  WordFilter newWords, knownReadings;
  bool mark = false, furigana = false;
  // Since the last sync (Study's live file): forms no longer new, readings no longer needing furigana. Sorted.
  std::vector<std::string> learned, alsoKnown;
};
Book book;

bool in(const std::vector<std::string>& sorted, const std::string_view key) {
  const auto it = std::lower_bound(sorted.begin(), sorted.end(), key, [](const std::string& a, const std::string_view b) { return a < b; });
  return it != sorted.end() && *it == key;
}

// What Study wrote after its last review, when it was written for the deck that is on the card now.
void loadLive(Store& store, const std::string& source) {
  std::string ini, sig[3];
  if (!store.read(std::string(DECK_DIR) + "/deck.ini", ini)) return;
  for (const auto& line : split(ini, '\n')) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const int which = key == "written_day" ? 0 : key == "cards" ? 1 : key == "log_lines" ? 2 : -1;
    if (which >= 0) sig[which] = std::to_string(std::atoi(line.c_str() + eq + 1));
  }
  if (sig[2].empty()) sig[2] = "0";
  bool head = true;
  store.lines(LIVE_FILE, [&](uint32_t, const std::string& line) {
    if (head) {
      head = false;
      return line == "1\t" + sig[0] + ":" + sig[1] + ":" + sig[2];
    }
    if (line.size() < 3 || line[1] != '\t') return true;
    if (line[0] == 'B') {
      const size_t tab = line.find('\t', 2);
      if (tab != std::string::npos && book.known && line.compare(2, tab - 2, source) == 0) book.readable = std::atoi(line.c_str() + tab + 1);
    } else if (line[0] == 'N' && book.mark) {
      book.learned.push_back(line.substr(2));
    } else if (line[0] == 'K' && book.furigana) {
      book.alsoKnown.push_back(line.substr(2));
    }
    return true;
  });
  std::sort(book.learned.begin(), book.learned.end());
  std::sort(book.alsoKnown.begin(), book.alsoKnown.end());
}

std::string fileName(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
}  // namespace

void close() {
  book = Book{};  // frees the filters and the lists
}

void open(Store& store, const std::string& bookPath) {
  close();
  const Prefs prefs = Prefs::load(store);
  const std::string name = fileName(bookPath);
  std::string source;
  // books.tsv: file name, source id, readable (per mille), new words, longest marked word (bytes)
  store.lines(BOOKS_FILE, [&](uint32_t, const std::string& line) {
    const auto c = split(line, '\t');
    if (c.size() < 5 || c[0] != name) return true;
    book.known = true;
    source = c[1];
    book.readable = (std::atoi(c[2].c_str()) + 5) / 10;
    book.fresh = std::atoi(c[3].c_str());
    book.longest = static_cast<size_t>(std::atoi(c[4].c_str()));
    return false;
  });
  std::string file;
  if (prefs.markNew && book.known && store.read(std::string(DECK_DIR) + "/new-" + source + ".bloom", file))
    book.mark = book.newWords.parse(file);
  file.clear();
  if (prefs.furiganaNeeded && store.read(KNOWN_FILTER, file)) book.furigana = book.knownReadings.parse(file);
  std::string().swap(file);
  loadLive(store, source);
}

bool known() { return book.known; }
int readablePercent() { return book.known ? book.readable : -1; }
int newWords() { return book.fresh; }
bool marking() { return book.mark; }
size_t maxWordBytes() { return book.longest; }
bool isNewWord(const std::string_view surface) { return book.mark && book.newWords.has(surface) && !in(book.learned, surface); }

bool hideFurigana(const std::string_view base, const std::string_view reading) {
  if (!book.furigana || base.empty() || reading.empty()) return false;
  std::string key(base);
  key += '\t';
  key += hiragana(reading);
  return book.knownReadings.has(key) || in(book.alsoKnown, key);
}

bool inStudy(Store& store, const std::string_view word) {
  std::string file;
  WordFilter words;
  return !word.empty() && store.read(WORDS_FILTER, file) && words.parse(file) && words.has(word);
}
}  // namespace link
}  // namespace sumitsubo
