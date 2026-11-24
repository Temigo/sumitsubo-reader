#pragma once
// The deck written by the desktop program (`sumitsubo deck`): see sumitsubo/device.py there.
// The two must change together; FORMAT is checked on load.
#include <cstdint>
#include <string>
#include <vector>

namespace sumitsubo {

constexpr int DECK_FORMAT = 1;
constexpr const char* DECK_DIR = "/sumitsubo/deck";
// Every verdict and mark, appended in the order they happened. The desktop reads this file.
constexpr const char* LOG_FILE = "/sumitsubo/log/events.csv";
constexpr const char* STATE_FILE = "/sumitsubo/log/state.tsv";  // where the log had brought every card, so it is not replayed from the start
// What the reviews since the last sync changed for the reader: written by Study, read by ReaderLink.
// First line "1 <tab> deck signature"; then "B <tab> source <tab> readable %", "N <tab> a written
// form that is no longer new", "K <tab> base <tab> reading" for furigana no longer needed.
constexpr const char* LIVE_FILE = "/sumitsubo/log/live.tsv";
constexpr const char* FORMS_FILE = "/sumitsubo/deck/forms.tsv";  // from the desktop: see device.py, write_reader_link
constexpr size_t MAX_LIVE_BYTES = 48 * 1024;

enum class Kind : uint8_t { Word, Kanji, Colloc, Contrast, Listen, Unknown };

// A stretch of a sentence drawn one way: plain, the tested part, or a word with furigana.
struct Run {
  std::string text;
  std::string ruby;
  bool target = false;
};
struct Sentence {
  int source = 0;
  std::string place;
  std::vector<Run> runs;
  bool empty() const { return runs.empty(); }
};
struct Word {  // on a kanji card: a word that uses the reading
  std::string word, reading, gloss;
};
struct Member {  // on a contrast card
  std::string head, word, reading, gloss, role;
  Sentence sentence;
};
// One card's text, read from the deck when it is shown.
struct Card {
  Kind kind = Kind::Unknown;
  std::string id, head, reading, gloss, extra;
  std::vector<Sentence> sentences;  // word, collocation
  std::vector<Word> words;          // kanji
  std::vector<Member> members;      // contrast
  Sentence sentence;                // kanji: one sentence with its first word
};
struct Source {
  int id = 0;
  int priority = 2;
  std::string title;
  uint32_t total = 0;      // running words
  float fixed = 0;         // of those, expected readable from words with no card here
  float addedPct = 0;      // readable when the source was added
  int startedBefore = 0;   // new words already started when the deck was written
  int newTotal = 0;
};
struct Settings {
  int format = 0;
  int writtenDay = 0;
  int cards = 0;
  int logLines = 0;        // lines of this device's log the desktop had read when it wrote the deck
  int dailyNew = 15;
  int moreCount = 5;
  std::string moreNew = "once";  // never, once, free
  std::string contrastWhen = "mixup";
};

std::vector<std::string> split(const std::string& s, char sep);
Sentence parseSentence(const std::string& packed, bool withSource);
Kind kindOf(const std::string& code);
bool parseCard(const std::string& line, Card& out);
uint64_t parseId(const std::string& hex);

}  // namespace sumitsubo
