// Host tests for lib/Sumitsubo. Build and run:  test/sumitsubo/run.sh
// Arguments: the FSRS vectors file, and a folder holding a card's contents (with sumitsubo/deck).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "Study.h"
#include "ReaderLink.h"
using namespace sumitsubo;
namespace fs = std::filesystem;

struct DirStore : Store {
  std::string root;
  explicit DirStore(std::string r) : root(std::move(r)) {}
  bool read(const std::string& path, std::string& out) override {
    std::ifstream f(root + path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
  }
  bool append(const std::string& path, const std::string& text) override {
    fs::create_directories(fs::path(root + path).parent_path());
    std::ofstream f(root + path, std::ios::binary | std::ios::app);
    f << text;
    return static_cast<bool>(f);
  }
  bool write(const std::string& path, const std::string& text) override {
    fs::create_directories(fs::path(root + path).parent_path());
    std::ofstream f(root + path, std::ios::binary | std::ios::trunc);
    f << text;
    return static_cast<bool>(f);
  }
  uint32_t size(const std::string& path) override {
    std::error_code ec;
    const auto n = fs::file_size(root + path, ec);
    return ec ? 0 : static_cast<uint32_t>(n);
  }
  bool lines(const std::string& path, const std::function<bool(uint32_t, const std::string&)>& each, uint32_t from = 0) override {
    std::ifstream f(root + path, std::ios::binary);
    if (!f) return false;
    f.seekg(from);
    std::string line;
    for (;;) {
      const auto at = static_cast<uint32_t>(f.tellg());
      if (!std::getline(f, line)) break;
      if (!each(at, line)) break;
    }
    return true;
  }
  bool lineAt(const std::string& path, uint32_t offset, std::string& out) override {
    std::ifstream f(root + path, std::ios::binary);
    if (!f) return false;
    f.seekg(offset);
    return static_cast<bool>(std::getline(f, out));
  }
  bool readAt(const std::string& path, uint32_t offset, size_t n, std::string& out) override {
    std::ifstream f(root + path, std::ios::binary);
    if (!f) return false;
    f.seekg(offset);
    out.resize(n);
    f.read(&out[0], static_cast<std::streamsize>(n));
    return static_cast<size_t>(f.gcount()) == n;
  }
};

#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static int failures = 0;

static void testFsrs(const char* file) {
  std::ifstream f(file);
  std::string line;
  int steps = 0, bad = 0;
  while (std::getline(f, line)) {
    std::stringstream ss(line);
    std::string part;
    std::getline(ss, part, ';');
    Memory m;
    while (std::getline(ss, part, ';')) {
      int day, rating, due;
      double s, d;
      std::sscanf(part.c_str(), "%d %d %lf %lf %d", &day, &rating, &s, &d, &due);
      m = fsrs::review(m, static_cast<Rating>(rating), day);
      ++steps;
      if (std::fabs(m.stability - s) / s > 1e-3 || std::fabs(m.difficulty - d) > 1e-3 || m.dueDay != due) ++bad;
    }
  }
  std::printf("fsrs: %d reviews compared with py-fsrs, %d disagree\n", steps, bad);
  CHECK(steps > 5000 && bad == 0);
}

int main(int argc, char** argv) {
  assert(argc >= 3);
  testFsrs(argv[1]);
  const std::string card = argv[2];
  fs::remove_all(card + "/sumitsubo/log");
  DirStore store(card);
  int day = 0;
  {
    std::string deck;
    store.read("/sumitsubo/deck/deck.ini", deck);
    day = std::atoi(deck.substr(deck.find("written_day=") + 12).c_str());
  }
  Study st(store);
  CHECK(st.load(day));
  const Counts c = st.counts();
  std::printf("deck: %zu cards; today %d reviews, %d new\n", st.size(), c.reviews, c.fresh);
  CHECK(st.size() > 100 && c.fresh == st.settings().dailyNew);

  // every supported kind parses, and the tested part is marked
  int kinds[5] = {0, 0, 0, 0, 0}, marked = 0, ruby = 0;
  store.lines("/sumitsubo/deck/cards.tsv", [&](uint32_t, const std::string& line) {
    Card k;
    if (parseCard(line, k)) {
      ++kinds[static_cast<int>(k.kind)];
      for (const auto& s : k.sentences)
        for (const auto& r : s.runs) {
          marked += r.target;
          ruby += !r.ruby.empty();
        }
      if (k.kind == Kind::Kanji) CHECK(!k.words.empty());
      if (k.kind == Kind::Contrast) CHECK(k.members.size() >= 2);
    }
    return true;
  });
  std::printf("kinds: %d word, %d kanji, %d collocation, %d contrast; %d marked spans, %d furigana\n", kinds[0], kinds[1], kinds[2], kinds[3], marked, ruby);
  CHECK(kinds[0] > 50 && kinds[1] > 50 && kinds[3] > 5 && marked > 50 && ruby > 0);

  // a session: Good for most, Again for every fifth, one undo
  st.start();
  const int planned = st.length();
  Card k;
  int shown = 0, missed = 0;
  CHECK(st.current(k) && !k.id.empty());
  const std::string first = k.id;
  st.answer(Rating::Again);
  CHECK(st.canUndo() && st.length() == planned + 1);
  st.undo();
  CHECK(st.length() == planned && st.current(k) && k.id == first && st.currentMemory().isNew() == st.currentMemory().isNew());
  while (st.current(k)) {
    ++shown;
    if (shown % 5 == 0) {
      ++missed;
      st.answer(Rating::Again);
    } else {
      st.answer(shown % 7 == 0 ? Rating::Easy : Rating::Good);
    }
    CHECK(shown < 10000);
  }
  const Summary s = st.summary();
  std::printf("session: %d planned, %d shown, %d cards counted, %d again, %d started, %d due tomorrow\n", planned, shown, s.cards, s.again, s.started, s.dueTomorrow);
  CHECK(s.cards == planned && shown > planned && s.started == st.settings().dailyNew && s.good + s.easy + s.again + s.hard == s.cards);
  CHECK(st.counts().fresh == 0 && st.counts().reviews == 0);   // nothing more today

  // the log rebuilds the same state
  Study again(store);
  CHECK(again.load(day));
  CHECK(again.counts().reviews == 0 && again.counts().fresh == 0 && again.lastReviewDay() == day);
  Study next(store);
  CHECK(next.load(day + 1));
  std::printf("next day: %d reviews, %d new\n", next.counts().reviews, next.counts().fresh);
  CHECK(next.counts().reviews >= s.dueTomorrow && next.counts().fresh == next.settings().dailyNew);

  // one source only: a subset, and the rest is still there
  const auto src = next.sources();
  CHECK(!src.empty());
  int best = 0;
  for (const auto& x : src) {
    std::printf("  source %d %-14.14s readable %d%% (was %.0f%%) started %d/%d today %d+%d\n", x.source.id, x.source.title.c_str(), x.readablePct,
                x.source.addedPct, x.started, x.newTotal, x.reviewsToday, x.newToday);
    CHECK(x.readablePct >= 0 && x.readablePct <= 100);
    if (x.reviewsToday + x.newToday > 0 && !best) best = x.source.id;
  }
  CHECK(best != 0);
  next.start(best);
  const int part = next.length();
  CHECK(part > 0 && part <= next.counts().reviews + next.counts().fresh);
  while (next.current(k)) next.answer(Rating::Good);
  CHECK(next.summary().leftElsewhere + next.summary().newElsewhere >= 0);
  next.start();
  // marks: a suspended card does not come back
  if (next.current(k)) {
    const std::string gone = k.id;
    next.mark(Study::Mark::Suspend);
    Study later(store);
    CHECK(later.load(day + 1));
    later.start();
    bool seen = false;
    while (later.current(k)) {
      seen |= k.id == gone;
      later.answer(Rating::Good);
    }
    CHECK(!seen);
  }
  // the saved state is a shortcut and nothing more: the same cards as replaying the whole log
  {
    fs::remove(card + "/sumitsubo/log/state.tsv");
    Study full(store);
    CHECK(full.load(day + 1) && !full.resumed());
    full.checkpoint();
    CHECK(fs::exists(card + "/sumitsubo/log/state.tsv"));
    Study quick(store);
    CHECK(quick.load(day + 1) && quick.resumed() && quick.digest() == full.digest());
    CHECK(quick.counts().reviews == full.counts().reviews && quick.counts().fresh == full.counts().fresh);
    quick.addNew(3);                       // more reviews after the state was written, with an undo and a mark among them
    int n = 0;
    while (quick.current(k) && n < 3) {
      quick.answer(n == 1 ? Rating::Again : Rating::Hard);
      if (n == 0) quick.undo();
      ++n;
    }
    if (quick.current(k)) quick.mark(Study::Mark::Known);
    Study tail(store);                     // state file plus what the log gained since
    CHECK(tail.load(day + 1) && tail.resumed());
    fs::remove(card + "/sumitsubo/log/state.tsv");
    Study whole(store);                    // the log alone
    CHECK(whole.load(day + 1) && !whole.resumed());
    CHECK(tail.digest() == whole.digest() && tail.digest() == quick.digest());
    Study other(store);                    // a state file from another day's view of the log still fits
    tail.checkpoint();
    CHECK(other.load(day + 3) && other.resumed());
    Study plain(store);
    fs::remove(card + "/sumitsubo/log/state.tsv");
    CHECK(plain.load(day + 3) && plain.digest() == other.digest());
    store.write("/sumitsubo/log/state.tsv", "1\tnot-this-deck\t0\t0\t0\n");   // a state for another deck is ignored
    Study strict(store);
    CHECK(strict.load(day + 3) && !strict.resumed() && strict.digest() == plain.digest());
  }

  // what the reader is told: the filters the desktop wrote, read with the same hash
  {
    CHECK(WordFilter::hash("sumitsubo") == 0x772513cfea6e9aeeULL && WordFilter::hash("私\tわたくし") == 0xb2433f15a26ff9c4ULL);
    CHECK(hiragana("カンガ") == "かんが" && hiragana("わたくし") == "わたくし" && hiragana("ヴァー") == "ゔぁー");
    std::string wordsFile, aWord;
    CHECK(store.read("/sumitsubo/deck/words.bloom", wordsFile));
    WordFilter words;
    CHECK(words.parse(wordsFile) && !words.parse("nonsense"));
    CHECK(words.parse(wordsFile));
    int in = 0, total = 0, stray = 0;
    store.lines("/sumitsubo/deck/cards.tsv", [&](uint32_t, const std::string& line) {
      Card c;
      if (parseCard(line, c) && c.kind == Kind::Word) {
        ++total;
        in += words.has(c.head);
        stray += words.has(c.head + "\x01not a word");
        if (aWord.empty()) aWord = c.head;
      }
      return true;
    });
    std::printf("filters: %d of %d card words found, %d strays\n", in, total, stray);
    CHECK(total > 50 && in == total && stray * 100 <= total);
    CHECK(link::inStudy(store, aWord) && !link::inStudy(store, "") && !link::inStudy(store, "zzzz-not-a-word"));
    std::string books, name;
    if (store.read("/sumitsubo/deck/books.tsv", books) && !books.empty()) {
      name = books.substr(0, books.find('\t'));
      store.write("/sumitsubo/prefs.ini", "hints=0\nmark_new=1\nfurigana_needed=1\n");
      const Prefs p = Prefs::load(store);
      CHECK(!p.hints && p.markNew && p.furiganaNeeded && p.tilt);
      link::open(store, "/Books/" + name);
      CHECK(link::known() && link::readablePercent() >= 0 && link::readablePercent() <= 100 && link::marking());
      std::printf("book %s: readable %d%%, %d new words, longest marked %zu bytes\n", name.c_str(), link::readablePercent(), link::newWords(), link::maxWordBytes());
      CHECK(!link::isNewWord("zzzz") && !link::hideFurigana("", "x"));
      link::open(store, "/Books/not-from-the-desktop.epub");
      CHECK(!link::known() && link::readablePercent() == -1 && !link::marking());
      Prefs q = p;
      q.markNew = false;
      CHECK(q.save(store) && !Prefs::load(store).markNew && Prefs::load(store).furiganaNeeded);
      link::open(store, "/Books/" + name);
      CHECK(link::known() && !link::marking());
      link::close();
      CHECK(!link::known());

      // The reader follows the reviews made since the sync: a word started here is no longer
      // marked as new, one marked known loses its furigana, and the book's figure is today's.
      std::string formsFile;
      if (store.read("/sumitsubo/deck/forms.tsv", formsFile) && !formsFile.empty()) {
        std::unordered_map<uint64_t, std::vector<std::string>> forms, keys;
        for (const auto& line : split(formsFile, '\n')) {
          const auto c = split(line, '\t');
          if (c.size() < 3) continue;
          for (const auto& f : split(c[1], '\x1f')) if (!f.empty()) forms[parseId(c[0])].push_back(f);
          for (auto key : split(c[2], '\x1f')) if (key.find('\x1e') != std::string::npos) { key[key.find('\x1e')] = '\t'; keys[parseId(c[0])].push_back(key); }
        }
        store.write("/sumitsubo/prefs.ini", "mark_new=1\nfurigana_needed=1\n");
        Study live(store);
        CHECK(live.load(day + 4));
        live.addNew(400);
        std::string learned, known, untouched;
        int n = 0;
        Card c;
        while (live.current(c) && n < 600) {
          const uint64_t id = parseId(c.id);
          if (known.empty() && keys.count(id)) {
            known = keys[id][0];
            live.mark(Study::Mark::Known);
          } else {
            if (learned.empty() && forms.count(id)) learned = forms[id][0];
            live.answer(Rating::Good);
          }
          ++n;
        }
        live.checkpoint();
        std::string liveFile;
        CHECK(store.read("/sumitsubo/log/live.tsv", liveFile) && liveFile.compare(0, 2, "1\t") == 0 && liveFile.find("\nB\t") != std::string::npos);
        std::printf("live: %zu bytes; learned '%s', known '%s'\n", liveFile.size(), learned.c_str(), known.c_str());
        link::open(store, "/Books/" + name);
        int pct = -1;
        for (const auto& x : live.sources()) if (liveFile.find("B\t" + std::to_string(x.source.id) + "\t" + std::to_string(x.readablePct) + "\n") == std::string::npos) pct = -2;
        CHECK(pct == -1 && link::readablePercent() >= 0);
        if (!learned.empty()) {
          CHECK(liveFile.find("N\t" + learned + "\n") != std::string::npos && !link::isNewWord(learned));
        }
        if (!known.empty()) {
          const size_t tab = known.find('\t');
          CHECK(liveFile.find("K\t" + known + "\n") != std::string::npos && link::hideFurigana(known.substr(0, tab), known.substr(tab + 1)));
        }
        CHECK(!learned.empty() || !known.empty());
        // written for another deck: not believed
        store.write("/sumitsubo/log/live.tsv", "1\tnot-this-deck\n" + liveFile.substr(liveFile.find('\n') + 1));
        link::open(store, "/Books/" + name);
        if (!learned.empty()) CHECK(link::isNewWord(learned));
        link::close();
      } else {
        std::printf("no forms.tsv in this deck: the live checks were skipped\n");
      }
    } else {
      std::printf("no book in this deck: the book checks were skipped\n");
    }
  }
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures != 0;
}
