#pragma once
// What the reader knows of Study, so a book and the cards are not two separate worlds:
//   * how much of this book you can read (from the desktop's last sync),
//   * whether a word looked up is in your cards already (then it is not saved again),
//   * which words on the page are new to you (optionally marked),
//   * which furigana you no longer need (optionally left out).
// The desktop writes small word filters (Bloom filters: a few KB each, answers "certainly not" or
// "almost certainly yes") beside the deck. Nothing here depends on the firmware; the reader
// loads the files and asks. The files are described in CLAUDE.md, "Sumitsubo".
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Store.h"

namespace sumitsubo {

constexpr const char* PREFS_FILE = "/sumitsubo/prefs.ini";
constexpr const char* BOOKS_FILE = "/sumitsubo/deck/books.tsv";
constexpr const char* KNOWN_FILTER = "/sumitsubo/deck/known.bloom";
constexpr const char* WORDS_FILTER = "/sumitsubo/deck/words.bloom";
constexpr size_t MAX_FILTER_BYTES = 16 * 1024;  // a filter is held in memory while a book is open

// "Certainly not in the set" or "almost certainly in it". File: "SBL1", bits (uint32 LE), hashes
// (uint8), then the bit array. Written by sumitsubo/device.py with the same hash.
class WordFilter {
 public:
  bool parse(const std::string& file);
  bool has(std::string_view key) const;
  bool loaded() const { return !bits.empty(); }
  void clear() { std::vector<uint8_t>().swap(bits); }
  static uint64_t hash(std::string_view key);

 private:
  std::vector<uint8_t> bits;
  uint32_t m = 0;
  uint8_t k = 0;
};

// What you chose on Study's Options screen. Kept in a small file on the card.
struct Prefs {
  bool hints = true;          // the button hints at the foot of a card
  bool markNew = false;       // in a book: a line beside the words that are new to you
  bool furiganaNeeded = false;  // in a book: furigana only on words you do not know well yet
  bool tilt = true;           // tilting answers a card, when tilt is switched on in Settings
  bool smooth = true;         // cards: grey at the edges of letters, at the cost of a second pass of the screen
  int mark = 1;               // how a card shows the tested word: 1 grey ground, 2 box, 3 heavy underline (0: the thin underline of earlier drafts)
  static Prefs load(Store& store);
  bool save(Store& store) const;
};

extern bool studyWantsTilt;  // Study is in front and wants the tilt sensor read (see main.cpp)

std::string hiragana(std::string_view kana);  // katakana to hiragana; everything else unchanged

namespace link {
// The book being read. open() finds it in the desktop's list by its file name and loads what the
// options need; close() frees it. All answers are "no" when nothing is loaded.
void open(Store& store, const std::string& bookPath);
void close();
bool known();              // the desktop sent this book
int readablePercent();     // at the last sync; -1 when not known
int newWords();            // words of this book still new at the last sync
bool marking();            // new words are to be marked on the page
bool isNewWord(std::string_view surface);
bool hideFurigana(std::string_view base, std::string_view reading);
// Whether a dictionary headword has a card in Study. Loads its filter for the question only.
bool inStudy(Store& store, std::string_view word);
size_t maxWordBytes();     // the longest marked word, to bound the search on a page
}  // namespace link
}  // namespace sumitsubo
