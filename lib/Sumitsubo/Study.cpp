#include "Study.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace sumitsubo {
namespace {
const std::string CARDS = std::string(DECK_DIR) + "/cards.tsv";
constexpr int REQUEUE_AFTER = 5;  // a missed card comes back after at least this many others
constexpr int MAX_REPEATS = 2;
constexpr int MIXUP_WINDOW = 30, MIXUP_MISSES = 2;

std::string hex(const uint64_t id) {
  char buf[20];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(id));
  return buf;
}
}  // namespace

bool Study::load(const int32_t day) {
  today = day;
  recs.clear();
  occs.clear();
  srcs.clear();
  contrastMembers.clear();
  err.clear();
  std::string text;
  if (!store.read(std::string(DECK_DIR) + "/deck.ini", text)) {
    err = "No deck on the card. Write one with the desktop program: sumitsubo deck";
    return false;
  }
  for (const auto& line : split(text, '\n')) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "format") cfg.format = std::atoi(v.c_str());
    else if (k == "written_day") cfg.writtenDay = std::atoi(v.c_str());
    else if (k == "cards") cfg.cards = std::atoi(v.c_str());
    else if (k == "log_lines") cfg.logLines = std::atoi(v.c_str());
    else if (k == "daily_new") cfg.dailyNew = std::atoi(v.c_str());
    else if (k == "more_count") cfg.moreCount = std::atoi(v.c_str());
    else if (k == "more_new") cfg.moreNew = v;
    else if (k == "contrast_when") cfg.contrastWhen = v;
  }
  if (cfg.format != DECK_FORMAT) {
    err = "This deck was written for a different version. Update the desktop program or this firmware.";
    return false;
  }
  store.lines(std::string(DECK_DIR) + "/sources.tsv", [&](uint32_t, const std::string& line) {
    const auto c = split(line, '\t');
    if (c.size() >= 8) {
      Source s;
      s.id = std::atoi(c[0].c_str());
      s.priority = std::atoi(c[1].c_str());
      s.title = c[2];
      s.total = static_cast<uint32_t>(std::atoi(c[3].c_str()));
      s.fixed = std::atoi(c[4].c_str()) / 1000.0f;
      s.addedPct = std::atoi(c[5].c_str()) / 10.0f;
      s.startedBefore = std::atoi(c[6].c_str());
      s.newTotal = std::atoi(c[7].c_str());
      srcs.push_back(s);
    }
    return true;
  });
  // Only the columns needed to schedule are kept; a card's text is read again when it is shown.
  store.lines(CARDS, [&](const uint32_t offset, const std::string& line) {
    size_t tabs[9], n = 0;
    for (size_t i = 0; i < line.size() && n < 9; ++i)
      if (line[i] == '\t') tabs[n++] = i;
    if (n < 9) return true;
    auto col = [&](const size_t k) { return line.substr(k ? tabs[k - 1] + 1 : 0, tabs[k] - (k ? tabs[k - 1] + 1 : 0)); };
    Rec r;
    r.id = parseId(col(0));
    r.kind = kindOf(col(1));
    r.offset = offset;
    r.item = std::atoi(col(2).c_str());
    r.rank = static_cast<uint16_t>(std::atoi(col(3).c_str()));
    {  // column 12, when the deck has it: "1" for a word saved in the reader
      size_t tabsN = 0;
      for (const char ch : line) tabsN += ch == '\t';
      r.first = tabsN >= 12 && !line.empty() && line.back() == '1' && line.size() >= 2 && line[line.size() - 2] == '\t';
    }
    r.firstSource = static_cast<uint16_t>(std::atoi(col(4).c_str()));
    const auto seed = split(col(5), ',');
    if (seed.size() == 4) {
      r.mem.stability = static_cast<float>(std::atof(seed[0].c_str()));
      r.mem.difficulty = static_cast<float>(std::atof(seed[1].c_str()));
      r.mem.lastDay = std::atoi(seed[2].c_str());
      r.mem.dueDay = std::atoi(seed[3].c_str());
      r.seeded = true;
    }
    r.occStart = static_cast<uint16_t>(occs.size());
    for (const auto& o : split(col(6), ',')) {
      const size_t colon = o.find(':');
      if (colon == std::string::npos || r.occCount == 255) continue;
      occs.push_back({static_cast<uint16_t>(std::atoi(o.c_str())), static_cast<uint16_t>(std::atoi(o.c_str() + colon + 1))});
      ++r.occCount;
    }
    if (r.kind == Kind::Contrast) {  // member items, to judge when the card may appear
      r.item = static_cast<int32_t>(contrastMembers.size());
      const auto ids = split(col(8), ',');
      contrastMembers.push_back(ids.size());
      for (const auto& m : ids) contrastMembers.push_back(static_cast<uint64_t>(std::atoll(m.c_str())));
    }
    recs.push_back(r);
    return true;
  });
  if (recs.empty()) {
    err = "The deck on the card has no cards.";
    return false;
  }
  replay();
  return true;
}

// ---- where the log had brought the cards ----------------------------------------------------------
// state.tsv: a first line naming the deck and how much of the log is included, then one line per
// card the log has changed. It is only a shortcut: the log stays the truth, and a state file that
// does not fit this deck or this log is ignored and written again.
uint64_t Study::digest() const {
  uint64_t h = 0xcbf29ce484222325ULL;
  auto mix = [&h](const void* p, const size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(p)[i]) * 0x100000001b3ULL;
  };
  for (const Rec& r : recs) {
    const int32_t v[6] = {r.mem.lastDay, r.mem.dueDay, r.lastAgainDay, r.startedDay, r.againRecent | (r.bad << 8), r.suspended ? 1 : 0};
    mix(&r.id, sizeof r.id);
    mix(&r.mem.stability, sizeof r.mem.stability);
    mix(&r.mem.difficulty, sizeof r.mem.difficulty);
    mix(v, sizeof v);
  }
  const int32_t tail[2] = {lastLogDay, startedToday};
  mix(tail, sizeof tail);
  return h;
}

std::string Study::signature() const {
  return std::to_string(cfg.writtenDay) + ":" + std::to_string(cfg.cards) + ":" + std::to_string(cfg.logLines);
}

bool Study::restore() {
  const uint32_t have = store.size(LOG_FILE);
  bool head = true, ok = false;
  std::unordered_map<uint64_t, uint32_t> at;
  store.lines(STATE_FILE, [&](uint32_t, const std::string& line) {
    const auto c = split(line, '\t');
    if (head) {
      head = false;
      if (c.size() < 5 || c[0] != "1" || c[1] != signature()) return false;
      logBytes = static_cast<uint32_t>(std::strtoul(c[2].c_str(), nullptr, 10));
      logLine = static_cast<uint32_t>(std::strtoul(c[3].c_str(), nullptr, 10));
      lastLogDay = std::atoi(c[4].c_str());
      if (logBytes > have) return false;  // the log is shorter than the state claims: not this log
      ok = true;
      at.reserve(recs.size());
      for (size_t i = 0; i < recs.size(); ++i) at[recs[i].id] = static_cast<uint32_t>(i);
      return true;
    }
    if (c.size() < 10) return true;
    const auto it = at.find(parseId(c[0]));
    if (it == at.end()) return true;
    Rec& r = recs[it->second];
    r.mem = Memory{static_cast<float>(std::atof(c[1].c_str())), static_cast<float>(std::atof(c[2].c_str())), std::atoi(c[3].c_str()),
                   std::atoi(c[4].c_str())};
    r.lastAgainDay = std::atoi(c[5].c_str());
    r.startedDay = std::atoi(c[6].c_str());
    r.againRecent = static_cast<uint8_t>(std::atoi(c[7].c_str()));
    r.bad = static_cast<uint8_t>(std::atoi(c[8].c_str()));
    r.suspended = c[9] == "1";
    r.touched = true;
    return true;
  });
  if (!ok) logBytes = logLine = 0, lastLogDay = 0;
  return ok;
}

void Study::checkpoint() {
  if (!dirty && fromState) return;
  char buf[160];
  std::string out = "1\t" + signature() + "\t" + std::to_string(store.size(LOG_FILE)) + "\t" + std::to_string(logLine) + "\t" +
                    std::to_string(lastLogDay) + "\n";
  for (const Rec& r : recs) {
    if (!r.touched) continue;
    std::snprintf(buf, sizeof buf, "%016llx\t%.9g\t%.9g\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", static_cast<unsigned long long>(r.id),
                  static_cast<double>(r.mem.stability), static_cast<double>(r.mem.difficulty), static_cast<int>(r.mem.lastDay),
                  static_cast<int>(r.mem.dueDay), static_cast<int>(r.lastAgainDay), static_cast<int>(r.startedDay),
                  static_cast<int>(r.againRecent), static_cast<int>(r.bad), r.suspended ? 1 : 0);
    out += buf;
  }
  if (store.write(STATE_FILE, out)) dirty = false, fromState = true;
  writeLive();
}

// The reader's figures follow the reviews, not the last sync: how readable each source is now,
// which written forms are no longer new (their card was started here), and which readings no
// longer need furigana (their card became mature here, or was marked known).
void Study::writeLive() {
  std::string out = "1\t" + signature() + "\n";
  for (const SourceStats& s : sources()) out += "B\t" + std::to_string(s.source.id) + "\t" + std::to_string(s.readablePct) + "\n";
  std::unordered_set<uint64_t> started, mature;
  for (const Rec& r : recs) {
    if (!r.touched || r.suspended || (r.kind != Kind::Word && r.kind != Kind::Colloc)) continue;
    if (r.startedDay > 0 && !r.mem.isNew()) started.insert(r.id);
    if (!r.mem.isNew() && r.mem.dueDay - r.mem.lastDay >= MATURE_DAYS) mature.insert(r.id);
  }
  if (!started.empty() || !mature.empty()) {
    store.lines(FORMS_FILE, [&](uint32_t, const std::string& line) {
      const auto c = split(line, '\t');
      if (c.size() < 3) return true;
      const uint64_t id = parseId(c[0]);
      if (started.count(id))
        for (const auto& form : split(c[1], '\x1f'))
          if (!form.empty()) out += "N\t" + form + "\n";
      if (mature.count(id))
        for (auto key : split(c[2], '\x1f')) {
          const size_t sep = key.find('\x1e');
          if (sep == std::string::npos) continue;
          key[sep] = '\t';
          out += "K\t" + key + "\n";
        }
      return out.size() < MAX_LIVE_BYTES;
    });
  }
  store.write(LIVE_FILE, out);
}

int Study::find(const uint64_t id) const {
  for (size_t i = 0; i < recs.size(); ++i)
    if (recs[i].id == id) return static_cast<int>(i);
  return -1;
}

void Study::apply(Rec& r, const Rating rating, const int32_t day) {
  if (r.startedDay == 0 && !r.seeded && r.mem.isNew()) r.startedDay = day;
  if (rating == Rating::Again) {
    if (day - r.lastAgainDay > MIXUP_WINDOW) r.againRecent = 0;
    if (r.againRecent < 255) ++r.againRecent;
    r.lastAgainDay = day;
  }
  r.mem = fsrs::review(r.mem, rating, day);
}

// events.csv, in the order things happened: day,id,rating  or  day,id,mark[,...]
void Study::replay() {
  std::unordered_map<uint64_t, uint32_t> at;
  at.reserve(recs.size());
  for (size_t i = 0; i < recs.size(); ++i) at[recs[i].id] = static_cast<uint32_t>(i);
  lastLogDay = 0;
  logBytes = logLine = 0;
  fromState = restore();
  dirty = false;
  store.lines(LOG_FILE, [&](uint32_t, const std::string& line) {
    const uint32_t number = logLine++;
    const auto c = split(line, ',');
    if (c.size() < 3) return true;
    const int32_t day = std::atoi(c[0].c_str());
    const auto it = at.find(parseId(c[1]));
    if (it == at.end() || day <= 0) return true;
    Rec& r = recs[it->second];
    r.touched = true;
    dirty = true;
    const int rating = std::atoi(c[2].c_str());
    if (rating >= 1 && rating <= 4) {
      apply(r, static_cast<Rating>(rating), day);
      lastLogDay = std::max(lastLogDay, day);
    } else if (c[2] == "suspend") {
      r.suspended = true;
    } else if (c[2] == "bad" && c.size() >= 4) {
      // A mark names a sentence by its place on the card. Once the desktop has read the mark, the
      // deck no longer has that sentence, so the mark must not fall on the one now in its place.
      if (number >= static_cast<uint32_t>(cfg.logLines)) r.bad |= static_cast<uint8_t>(1u << (std::atoi(c[3].c_str()) & 7));
    } else if (c[2] == "known") {
      r.mem = Memory{365.0f, 5.0f, day, day + 240 + static_cast<int32_t>(r.id % 241)};
    } else if (c[2] == "undo" && c.size() >= 9) {  // the state to restore travels with the line
      const bool wasStarted = !r.mem.isNew();
      r.mem = Memory{static_cast<float>(std::atof(c[3].c_str())), static_cast<float>(std::atof(c[4].c_str())),
                     std::atoi(c[5].c_str()), std::atoi(c[6].c_str())};
      r.againRecent = static_cast<uint8_t>(std::atoi(c[7].c_str()));
      r.lastAgainDay = std::atoi(c[8].c_str());
      if (wasStarted && r.mem.isNew()) r.startedDay = 0;
    }
    return true;
  }, logBytes);
  startedToday = 0;  // new cards begun today, which count against today's number
  for (const Rec& r : recs)
    if (r.startedDay == today && !r.seeded) ++startedToday;
}

void Study::log(const std::string& file, const std::string& line) {
  store.append(file, line + "\n");
  ++logLine;
  dirty = true;
}

bool Study::inSource(const Rec& r, const int source) const {
  if (source == ALL_SOURCES) return true;
  if (r.firstSource == source) return true;
  for (int i = 0; i < r.occCount; ++i)
    if (occs[r.occStart + i].source == source) return true;
  return false;
}

int Study::priorityOf(const int source) const {
  for (const auto& s : srcs)
    if (s.id == source) return s.priority;
  return 2;
}

bool Study::contrastReady(const Rec& r) const {
  if (cfg.contrastWhen == "never") return false;
  const size_t at = static_cast<size_t>(r.item);
  const size_t n = static_cast<size_t>(contrastMembers[at]);
  int met = 0, mixed = 0;
  for (size_t k = 1; k <= n; ++k) {
    const auto item = static_cast<int32_t>(contrastMembers[at + k]);
    for (const auto& w : recs) {
      if (w.item != item || (w.kind != Kind::Word && w.kind != Kind::Kanji)) continue;
      if (!w.mem.isNew()) ++met;
      if (w.againRecent >= MIXUP_MISSES && today - w.lastAgainDay <= MIXUP_WINDOW) ++mixed;
      break;
    }
  }
  if (cfg.contrastWhen == "all") return met == static_cast<int>(n);
  if (cfg.contrastWhen == "most") return met * 3 >= static_cast<int>(n) * 2;
  if (cfg.contrastWhen == "one") return met >= 1;
  return mixed >= 2;  // mixup
}

std::vector<uint32_t> Study::build(const int source, const int newLimit) const {
  std::vector<uint32_t> due, fresh;
  std::unordered_set<int32_t> taken;  // one card per item per day
  for (const auto& r : recs)
    if (!r.mem.isNew() && r.mem.lastDay == today && r.kind != Kind::Contrast) taken.insert(r.item);
  for (size_t i = 0; i < recs.size(); ++i) {
    const Rec& r = recs[i];
    if (r.suspended || !supported(r) || r.mem.isNew() || r.mem.dueDay > today || !inSource(r, source)) continue;
    due.push_back(static_cast<uint32_t>(i));
  }
  std::sort(due.begin(), due.end(), [&](uint32_t a, uint32_t b) { return recs[a].mem.dueDay < recs[b].mem.dueDay; });
  std::vector<uint32_t> out;
  for (const auto i : due) {
    const Rec& r = recs[i];
    if (r.kind != Kind::Contrast && !taken.insert(r.item).second) continue;
    out.push_back(i);
  }
  for (size_t i = 0; i < recs.size(); ++i) {
    const Rec& r = recs[i];
    if (r.suspended || !supported(r) || !r.mem.isNew() || !inSource(r, source)) continue;
    if (r.kind == Kind::Contrast && !contrastReady(r)) continue;
    fresh.push_back(static_cast<uint32_t>(i));
  }
  std::sort(fresh.begin(), fresh.end(), [&](uint32_t a, uint32_t b) {
    const Rec &x = recs[a], &y = recs[b];
    if (x.first != y.first) return x.first;  // saved in the reader: before everything else
    const int px = x.kind == Kind::Contrast ? 9 : priorityOf(x.firstSource), py = y.kind == Kind::Contrast ? 9 : priorityOf(y.firstSource);
    if (px != py) return px < py;
    if (x.firstSource != y.firstSource) return x.firstSource < y.firstSource;
    return x.rank < y.rank;
  });
  int added = 0;
  for (const auto i : fresh) {
    if (added >= newLimit) break;
    const Rec& r = recs[i];
    if (r.kind != Kind::Contrast && !taken.insert(r.item).second) continue;
    out.push_back(i);
    ++added;
  }
  return out;
}

Counts Study::counts(const int source) const {
  Counts c;
  for (const auto i : build(source, std::max(0, cfg.dailyNew - startedToday))) {
    const Rec& r = recs[i];
    if (r.mem.isNew()) ++c.fresh;
    else {
      ++c.reviews;
      ++c.byKind[static_cast<int>(r.kind)];
    }
  }
  return c;
}

void Study::start(const int source) {
  filter = source;
  queue = build(source, std::max(0, cfg.dailyNew - startedToday));
  repeats.assign(queue.size(), 0);
  pos = 0;
  sentence = 0;
  sum = Summary{};
  hasUndo = false;
  extraNew = 0;
}

bool Study::addNew(const int n) {
  const auto more = build(filter, n);
  size_t added = 0;
  for (const auto i : more)
    if (recs[i].mem.isNew()) {
      queue.push_back(i);
      repeats.push_back(0);
      ++added;
    }
  hasUndo = false;
  return added > 0;
}

bool Study::current(Card& card) {
  while (!finished()) {
    Rec& r = recs[queue[pos]];
    std::string line;
    if (store.lineAt(CARDS, r.offset, line) && parseCard(line, card)) {
      if (!card.sentences.empty()) {
        // Show the next usable sentence each time the card comes round.
        const int n = static_cast<int>(card.sentences.size());
        int reviews = 0;
        if (!r.mem.isNew()) reviews = (r.mem.lastDay + r.mem.dueDay) % n;
        for (int k = 0; k < n; ++k) {
          const int idx = ((reviews + sentence + k) % n + n) % n;
          if (!(r.bad & (1u << (idx & 7)))) {
            sentence = ((idx - reviews) % n + n) % n;
            std::swap(card.sentences[0], card.sentences[static_cast<size_t>(idx)]);
            break;
          }
        }
      }
      return true;
    }
    ++pos;  // unreadable: skip it
  }
  return false;
}

void Study::stepSentence(const int by) { sentence += by; }

Memory Study::currentMemory() const { return finished() ? Memory{} : recs[queue[pos]].mem; }

bool Study::currentIsYoung() const {
  if (finished()) return false;
  const Memory& m = recs[queue[pos]].mem;
  return m.isNew() || m.dueDay - m.lastDay < MATURE_DAYS;
}

void Study::answer(const Rating rating) {
  if (finished()) return;
  const uint32_t index = queue[pos];
  Rec& r = recs[index];
  r.touched = true;
  last = Step{index, queue.size(), r, sum};
  hasUndo = true;
  const bool wasNew = r.mem.isNew();
  apply(r, rating, today);
  log(LOG_FILE, std::to_string(today) + "," + hex(r.id) + "," + std::to_string(static_cast<int>(rating)));
  lastLogDay = today;
  if (repeats[pos] == 0) {  // a card counts once, however often it came back
    ++sum.cards;
    if (wasNew) {
      ++sum.started;
      ++startedToday;
    }
    switch (rating) {
      case Rating::Again: ++sum.again; break;
      case Rating::Hard: ++sum.hard; break;
      case Rating::Good: ++sum.good; break;
      case Rating::Easy: ++sum.easy; break;
    }
  }
  if (rating == Rating::Again && repeats[pos] < MAX_REPEATS) {
    const size_t at = std::min(queue.size(), pos + 1 + REQUEUE_AFTER);
    queue.insert(queue.begin() + static_cast<long>(at), index);
    repeats.insert(repeats.begin() + static_cast<long>(at), static_cast<uint8_t>(repeats[pos] + 1));
  }
  ++pos;
  sentence = 0;
}

void Study::undo() {
  if (!hasUndo || pos == 0) return;
  --pos;
  if (queue.size() > last.queueSize) {  // take the re-queued copy back out
    const size_t at = std::min(queue.size() - 1, pos + 1 + REQUEUE_AFTER);
    queue.erase(queue.begin() + static_cast<long>(at));
    repeats.erase(repeats.begin() + static_cast<long>(at));
  }
  Rec& r = recs[last.index];
  r.touched = true;
  if (r.startedDay == today && last.before.mem.isNew() && startedToday > 0) --startedToday;
  r = last.before;
  sum = last.sum;
  char buf[128];
  std::snprintf(buf, sizeof buf, "%d,%s,undo,%.9g,%.9g,%d,%d,%d,%d", static_cast<int>(today), hex(r.id).c_str(),
                static_cast<double>(r.mem.stability), static_cast<double>(r.mem.difficulty), static_cast<int>(r.mem.lastDay),
                static_cast<int>(r.mem.dueDay), static_cast<int>(r.againRecent), static_cast<int>(r.lastAgainDay));
  log(LOG_FILE, buf);
  hasUndo = false;
  sentence = 0;
}

void Study::mark(const Mark m) {
  if (finished()) return;
  Rec& r = recs[queue[pos]];
  r.touched = true;
  const std::string head = std::to_string(today) + "," + hex(r.id) + ",";
  if (m == Mark::BadSentence) {
    Card card;
    std::string line;
    int n = 1, reviews = 0;
    if (store.lineAt(CARDS, r.offset, line) && parseCard(line, card)) n = std::max<int>(1, static_cast<int>(card.sentences.size()));
    if (!r.mem.isNew()) reviews = (r.mem.lastDay + r.mem.dueDay) % n;
    const int idx = ((reviews + sentence) % n + n) % n;
    r.bad |= static_cast<uint8_t>(1u << (idx & 7));
    log(LOG_FILE, head + "bad," + std::to_string(idx));
    sentence = 0;
    bool left = false;
    for (int k = 0; k < n && k < 8; ++k) left |= !(r.bad & (1u << k));
    if (!left) ++pos;  // it was the last one: the card waits, unanswered, for the sync to give it new sentences
    return;  // otherwise the card stays, with its next sentence
  }
  if (m == Mark::Suspend) {
    r.suspended = true;
    log(LOG_FILE, head + "suspend");
  } else {
    r.mem = Memory{365.0f, 5.0f, today, today + 240 + static_cast<int32_t>(r.id % 241)};
    log(LOG_FILE, head + "known");
  }
  hasUndo = false;
  ++pos;
  sentence = 0;
}

Summary Study::summary() const {
  Summary s = sum;
  std::unordered_set<int32_t> items;
  for (const auto& r : recs)
    if (!r.suspended && supported(r) && !r.mem.isNew() && r.mem.dueDay == today + 1) ++s.dueTomorrow;
  if (filter != ALL_SOURCES) {
    for (const auto i : build(ALL_SOURCES, std::max(0, cfg.dailyNew - startedToday))) {
      if (recs[i].mem.isNew()) ++s.newElsewhere;
      else ++s.leftElsewhere;
    }
  }
  return s;
}

Totals Study::totals() const {
  Totals t;
  for (const auto& r : recs) {
    if (r.kind != Kind::Word || r.suspended) continue;
    if (r.mem.isNew()) ++t.fresh;
    else if (r.mem.dueDay - r.mem.lastDay >= MATURE_DAYS) ++t.mature;
    else ++t.young;
  }
  return t;
}

std::vector<SourceStats> Study::sources() const {
  std::vector<SourceStats> out;
  const auto todays = build(ALL_SOURCES, std::max(0, cfg.dailyNew - startedToday));
  int ahead = 0;  // new word cards in sources that come first
  std::vector<Source> order = srcs;
  std::stable_sort(order.begin(), order.end(), [](const Source& a, const Source& b) { return a.priority < b.priority; });
  for (const auto& s : order) {
    SourceStats st;
    st.source = s;
    double expected = s.fixed;
    int remaining = 0;
    for (const auto& r : recs) {
      if (r.suspended) continue;
      if (r.kind == Kind::Word || r.kind == Kind::Colloc) {
        if (r.firstSource == s.id) {
          if (r.startedDay) ++st.started;
          if (r.mem.isNew()) ++remaining;
        }
      }
      if (r.kind != Kind::Word || r.mem.isNew()) continue;
      for (int i = 0; i < r.occCount; ++i)
        if (occs[r.occStart + i].source == s.id) expected += occs[r.occStart + i].n * fsrs::recall(r.mem, today);
    }
    st.started += s.startedBefore;
    st.newTotal = s.newTotal;
    st.readablePct = s.total ? static_cast<int>(100.0 * expected / s.total + 0.5) : 0;
    for (const auto i : todays) {
      if (!inSource(recs[i], s.id)) continue;
      if (recs[i].mem.isNew()) ++st.newToday;
      else ++st.reviewsToday;
    }
    ahead += remaining;
    st.lastNewDay = remaining && cfg.dailyNew > 0 ? today + (ahead + cfg.dailyNew - 1) / cfg.dailyNew - 1 : 0;
    out.push_back(st);
  }
  return out;
}

}  // namespace sumitsubo
