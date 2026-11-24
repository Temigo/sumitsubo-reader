#pragma once
// A study session: what is due, in what order, what a verdict does, and the log it leaves.
// Nothing here knows about the screen or the hardware; see Store.h for the one thing it needs.
//
// State is not stored. It is rebuilt at load from the deck's starting states and the review log,
// which is appended to after every verdict: the log is the truth, and a power cut loses at most
// the card on screen. (A checkpoint to shorten the replay is future work; see docs.)
#include <cstdint>
#include <string>
#include <vector>

#include "Deck.h"
#include "Fsrs.h"
#include "Store.h"

namespace sumitsubo {

constexpr int ALL_SOURCES = 0;
constexpr int MATURE_DAYS = 21;

struct Counts {
  int reviews = 0;  // due today
  int fresh = 0;    // new cards that would be introduced today
  int byKind[5] = {0, 0, 0, 0, 0};  // reviews, by Kind
};
struct Summary {
  int cards = 0, again = 0, hard = 0, good = 0, easy = 0, started = 0;
  int dueTomorrow = 0;
  int leftElsewhere = 0, newElsewhere = 0;  // after a one-source session
};
struct SourceStats {
  Source source;
  int readablePct = 0;
  int started = 0, newTotal = 0;
  int reviewsToday = 0, newToday = 0;
  int lastNewDay = 0;  // day by which its last new word is introduced; 0 = all started
};
struct Totals {
  int mature = 0, young = 0, fresh = 0;  // word cards
};

class Study {
 public:
  explicit Study(Store& store) : store(store) {}

  // Reads the deck and replays the log. False with `error` set when there is no usable deck.
  bool load(int32_t today);
  const std::string& error() const { return err; }
  const Settings& settings() const { return cfg; }
  int32_t day() const { return today; }
  int32_t lastReviewDay() const { return lastLogDay; }
  size_t size() const { return recs.size(); }

  Counts counts(int source = ALL_SOURCES) const;
  std::vector<SourceStats> sources() const;
  Totals totals() const;

  // A session over everything, or over the cards whose word occurs in one source.
  void start(int source = ALL_SOURCES);
  bool finished() const { return pos >= queue.size(); }
  int position() const { return static_cast<int>(pos) + 1; }
  int length() const { return static_cast<int>(queue.size()); }
  int sessionSource() const { return filter; }
  // The card to show now; its text is read from the deck. sentenceStep moves through its sentences.
  bool current(Card& card);
  int sentenceIndex() const { return sentence; }
  void stepSentence(int by);
  Memory currentMemory() const;
  bool currentIsYoung() const;  // new or not yet mature: the collocation hint is shown

  void answer(Rating rating);
  bool canUndo() const { return hasUndo; }
  void undo();
  enum class Mark { Suspend, Known, BadSentence };
  void mark(Mark m);
  bool addNew(int n);  // "more new cards" after the session; false when there are none
  // Writes where every card stands, so the next start reads only what the log gained since.
  // Called when a session ends and when Study is left.
  void checkpoint();
  bool resumed() const { return fromState; }  // the last load() started from such a file
  uint64_t digest() const;  // one number for the state of every card: equal after any two routes to the same state

  Summary summary() const;

 private:
  struct Rec {
    uint64_t id = 0;
    uint32_t offset = 0;
    int32_t item = 0;
    Memory mem;
    int32_t lastAgainDay = 0;
    int32_t startedDay = 0;  // first review on this device; 0 = not started here
    uint16_t rank = 0, firstSource = 0, occStart = 0;
    uint8_t occCount = 0, againRecent = 0, bad = 0;  // bad: bit per unusable sentence
    Kind kind = Kind::Unknown;
    bool seeded = false, suspended = false;
    bool first = false;    // saved in the reader: introduced before all other new cards
    bool touched = false;  // the log changed it
  };
  struct Occ {
    uint16_t source, n;
  };
  struct Step {  // what answer() changed, for undo
    size_t index = 0, queueSize = 0;
    Rec before;
    Summary sum;
  };

  Store& store;
  Settings cfg;
  std::vector<Source> srcs;
  std::vector<Rec> recs;
  std::vector<Occ> occs;
  std::vector<uint64_t> contrastMembers;  // per contrast rec: packed in memberStart order (see .cpp)
  std::vector<uint32_t> queue;
  std::vector<uint8_t> repeats;  // per queue slot: times this card was re-queued
  size_t pos = 0;
  int32_t today = 0, lastLogDay = 0;
  int filter = ALL_SOURCES, sentence = 0, startedToday = 0, extraNew = 0;
  Summary sum;
  Step last;
  bool hasUndo = false, fromState = false, dirty = false;
  uint32_t logBytes = 0, logLine = 0;  // how far into the log the cards are brought
  std::string err;
  std::string signature() const;
  bool restore();
  void writeLive();

  int find(uint64_t id) const;
  bool inSource(const Rec& r, int source) const;
  int priorityOf(int source) const;
  bool supported(const Rec& r) const { return r.kind != Kind::Listen && r.kind != Kind::Unknown; }
  bool contrastReady(const Rec& r) const;
  void apply(Rec& r, Rating rating, int32_t day);
  void replay();
  std::vector<uint32_t> build(int source, int newLimit) const;
  void log(const std::string& file, const std::string& line);
};

}  // namespace sumitsubo
