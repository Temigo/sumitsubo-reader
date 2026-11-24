#include "StudyActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalTiltSensor.h>

#include <algorithm>
#include <ctime>

#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

using sumitsubo::Kind;
using sumitsubo::Rating;
using Button = MappedInputManager::Button;

namespace {
constexpr int SIDE = 24;             // left and right margin
constexpr int HEADER_H = 44;
constexpr int SIDE_LINE_H = 30;      // the line above the button row that says what the side buttons do
constexpr int FOOT_H = 52;           // the button row: four cells, one per front button
constexpr unsigned long LONG_MS = 700;
constexpr int SOURCES_PER_PAGE = 5;
constexpr int BODY_FONT = NOTOSERIF_16_FONT_ID;   // sentences
constexpr int WORD_FONT = NOTOSERIF_18_FONT_ID;
constexpr int TEXT_FONT = UI_12_FONT_ID;          // meanings, messages
constexpr int NOTE_FONT = UI_10_FONT_ID;
constexpr int RUBY_FONT = SMALL_FONT_ID;
const Button BUTTONS[6] = {Button::Back, Button::Confirm, Button::Left, Button::Right, Button::Up, Button::Down};
enum { B_BACK, B_CONFIRM, B_LEFT, B_RIGHT, B_UP, B_DOWN };
const char* const MENU[] = {"Bad sentence: use another", "Suspend this card", "I know this well", "Leave Study", "Cancel"};
constexpr int MENU_N = 5;
constexpr int OPTIONS_N = 6;
constexpr int MARK_OPTION = 5;  // the one option that is not on or off

size_t utf8Len(const unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
bool closesLine(const std::string& ch) {   // never starts a line
  static const char* const marks[] = {"、", "。", "」", "』", "）", "，", "．", "？", "！", ",", ".", ")"};
  for (const auto* m : marks)
    if (ch == m) return true;
  return false;
}
int32_t localDay(const time_t t) {
  struct tm tm {};
  localtime_r(&t, &tm);
  struct tm utc = tm;
  utc.tm_isdst = 0;
  return static_cast<int32_t>(timegm(&utc) / 86400);
}
std::string dayText(const int32_t day, const char* fmt) {
  const time_t t = static_cast<time_t>(day) * 86400;
  struct tm tm {};
  gmtime_r(&t, &tm);
  char buf[48];
  strftime(buf, sizeof buf, fmt, &tm);
  return buf;
}
const char* kindName(const Kind k) {
  switch (k) {
    case Kind::Word: return "WORD";
    case Kind::Kanji: return "KANJI";
    case Kind::Colloc: return "COLLOCATION";
    case Kind::Contrast: return "CONTRAST";
    default: return "CARD";
  }
}
}  // namespace

// ---- life cycle ----------------------------------------------------------------------------------

void StudyActivity::onEnter() {
  Activity::onEnter();
  // Japanese from the card's fonts when they are installed (Noto Sans/Serif JP, as for books):
  // better shapes than the built-in subset, and the rare kanji it lacks. Borrowed the way
  // Matcha's word-lookup panel borrows them, and given back in onExit().
  sdFontSystem.setCjkFallbackNeeded(renderer, CjkScript::Japanese);
  sdFontSystem.ensureLoaded(renderer);
  sdFontSystem.ensureWordLookupFallback(renderer, BODY_FONT, 16);
  sdFontSystem.ensureWordLookupFallback(renderer, WORD_FONT, 18);
  prefs = sumitsubo::Prefs::load(store);
  big.open(store);
  hintsHidden = !prefs.hints;
  halTiltSensor.clearPendingEvents();
  sumitsubo::studyWantsTilt = prefs.tilt;
  const time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  clockTrusted = halClock.hasTime() && tm.tm_year + 1900 >= 2026;
  today = localDay(now);
  if (!study.load(today)) {
    message = study.error();
    screen = Screen::Message;
  } else if (!clockTrusted || today < study.lastReviewDay()) {
    // Reviews are scheduled by day, so a clock that lost its time must not be believed.
    today = std::max(std::max(today, study.lastReviewDay()), study.settings().writtenDay);
    screen = Screen::DateCheck;
  } else {
    screen = Screen::Today;
  }
  requestUpdate();
}

void StudyActivity::onExit() {
  sumitsubo::studyWantsTilt = false;
  study.checkpoint();  // the next start reads only what the log gains from here on
  sdFontSystem.releaseWordLookupFallback(renderer);
  sdFontSystem.setCjkFallbackNeeded(renderer, CjkScript::None);
  Activity::onExit();
}

void StudyActivity::open(const int32_t day) {
  today = day;
  if (!study.load(today)) {
    message = study.error();
    screen = Screen::Message;
  } else {
    screen = Screen::Today;
  }
}

void StudyActivity::nextCard() {
  if (study.current(card)) {
    screen = Screen::CardFront;
  } else {
    screen = Screen::Done;
    study.checkpoint();
  }
}

// ---- buttons -------------------------------------------------------------------------------------

void StudyActivity::loop() {
  // One event per button: a short press on release, or a long press once it has been held.
  bool shortPress[6] = {}, longPress[6] = {};
  for (int i = 0; i < 6; ++i) {
    if (mappedInput.wasPressed(BUTTONS[i])) {
      pressedAt[i] = millis();
      longFired[i] = false;
    }
    if (mappedInput.isPressed(BUTTONS[i]) && pressedAt[i] && !longFired[i] && millis() - pressedAt[i] >= LONG_MS) {
      longFired[i] = true;
      longPress[i] = true;
    }
    if (mappedInput.wasReleased(BUTTONS[i])) {
      shortPress[i] = pressedAt[i] && !longFired[i];
      pressedAt[i] = 0;
    }
  }
  // Tilting is a second way to press the button in that direction, on a card only: a flick the way
  // a page is turned forward is Right (Good), the other way Left (Again), up and down the side buttons.
  if (prefs.tilt && (screen == Screen::CardFront || screen == Screen::CardBack)) {
    if (halTiltSensor.wasTiltedForward()) shortPress[B_RIGHT] = true;
    if (halTiltSensor.wasTiltedBack()) shortPress[B_LEFT] = true;
    if (halTiltSensor.wasTiltedUp()) shortPress[B_UP] = true;
    if (halTiltSensor.wasTiltedDown()) shortPress[B_DOWN] = true;
  } else {
    halTiltSensor.clearPendingEvents();
  }
  auto setHints = [&](const bool hidden) {
    hintsHidden = hidden;
    prefs.hints = !hidden;
    prefs.save(store);
  };
  const Screen before = screen;
  bool redraw = false;
  auto leave = [&] {
    screen = Screen::Today;
    redraw = true;
  };

  switch (screen) {
    case Screen::Message:
      if (shortPress[B_BACK] || shortPress[B_CONFIRM]) {
        onGoHome(HomeMenuItem::STUDY);
        return;
      }
      break;
    case Screen::DateCheck:
      if (shortPress[B_UP]) ++today, redraw = true;
      if (shortPress[B_DOWN]) --today, redraw = true;
      if (shortPress[B_CONFIRM] || shortPress[B_RIGHT]) open(today), redraw = true;
      if (shortPress[B_BACK]) {
        onGoHome(HomeMenuItem::STUDY);
        return;
      }
      break;
    case Screen::Today:
      if (shortPress[B_BACK]) {
        onGoHome(HomeMenuItem::STUDY);
        return;
      }
      if (longPress[B_CONFIRM]) {
        optionCursor = 0;
        screen = Screen::Options;
      } else if (shortPress[B_CONFIRM] || shortPress[B_RIGHT]) {
        study.start();
        offeredMore = false;
        nextCard();
      } else if (shortPress[B_LEFT]) {
        sourceRows = study.sources();
        sourceCursor = 0;
        screen = Screen::Sources;
      }
      break;
    case Screen::Options:
      if (shortPress[B_BACK]) screen = Screen::Today;
      if (shortPress[B_DOWN]) optionCursor = (optionCursor + 1) % OPTIONS_N, redraw = true;
      if (shortPress[B_UP]) optionCursor = (optionCursor + OPTIONS_N - 1) % OPTIONS_N, redraw = true;
      if (shortPress[B_CONFIRM] || shortPress[B_RIGHT] || shortPress[B_LEFT]) {
        bool* const which[MARK_OPTION] = {&prefs.hints, &prefs.markNew, &prefs.furiganaNeeded, &prefs.tilt, &prefs.smooth};
        if (optionCursor == MARK_OPTION) prefs.mark = prefs.mark % 3 + 1;  // grey, box, heavy underline
        else *which[optionCursor] = !*which[optionCursor];
        hintsHidden = !prefs.hints;
        sumitsubo::studyWantsTilt = prefs.tilt;
        prefs.save(store);
        redraw = true;
      }
      break;
    case Screen::Sources: {
      const int n = static_cast<int>(sourceRows.size());
      if (shortPress[B_BACK]) screen = Screen::Today;
      if (shortPress[B_DOWN] && sourceCursor + 1 < n) ++sourceCursor, redraw = true;
      if (shortPress[B_UP] && sourceCursor > 0) --sourceCursor, redraw = true;
      if (shortPress[B_CONFIRM] && n) {
        study.start(sourceRows[static_cast<size_t>(sourceCursor)].source.id);
        offeredMore = false;
        nextCard();
      }
      break;
    }
    case Screen::CardFront:
      if (longPress[B_BACK]) badSentence(), redraw = true;
      else if (longPress[B_CONFIRM]) menuCursor = 0, menuFrom = Screen::CardFront, screen = Screen::Menu;
      else if (longPress[B_DOWN]) setHints(!hintsHidden), redraw = true;
      else if (shortPress[B_CONFIRM] || shortPress[B_LEFT] || shortPress[B_RIGHT]) screen = Screen::CardBack;
      else if (shortPress[B_UP] || shortPress[B_DOWN]) {
        study.stepSentence(shortPress[B_UP] ? -1 : 1);
        study.current(card);
        redraw = true;
      } else if (shortPress[B_BACK] && study.canUndo()) {
        study.undo();
        nextCard();
        redraw = true;
      } else if (shortPress[B_BACK]) {
        leave();  // nothing to take back: Back is the way out
      }
      break;
    case Screen::CardBack: {
      Rating verdict = Rating::Good;
      bool given = true;
      if (longPress[B_BACK]) {
        badSentence();
        redraw = true;
        break;
      }
      if (longPress[B_DOWN]) {
        setHints(!hintsHidden);
        redraw = true;
        break;
      }
      if (longPress[B_CONFIRM]) {
        menuCursor = 0;
        menuFrom = Screen::CardBack;
        screen = Screen::Menu;
        break;
      }
      if (shortPress[B_RIGHT]) verdict = Rating::Good;  // onward, as a page is turned
      else if (shortPress[B_LEFT]) verdict = Rating::Again;
      else if (shortPress[B_UP]) verdict = Rating::Easy;
      else if (shortPress[B_DOWN]) verdict = Rating::Hard;
      else given = false;
      if (given) {
        study.answer(verdict);
        nextCard();
        redraw = true;
      } else if (shortPress[B_CONFIRM] && definitionCut) {
        screen = Screen::Entry;
        entryFrom = 0;
      } else if (shortPress[B_BACK] && study.canUndo()) {
        study.undo();
        nextCard();
        redraw = true;
      } else if (shortPress[B_BACK]) {
        leave();
      }
      break;
    }
    case Screen::Entry:
      if (shortPress[B_BACK] || shortPress[B_CONFIRM]) screen = Screen::CardBack;
      // a definition longer than the screen: Down or Right for the next senses, Up or Left back
      if ((shortPress[B_DOWN] || shortPress[B_RIGHT]) && entryCut && entryShown > 0) entryFrom += entryShown, redraw = true;
      if ((shortPress[B_UP] || shortPress[B_LEFT]) && entryFrom > 0) entryFrom = 0, redraw = true;
      break;
    case Screen::Menu:
      if (shortPress[B_BACK]) screen = menuFrom;
      if (shortPress[B_DOWN]) menuCursor = (menuCursor + 1) % MENU_N, redraw = true;
      if (shortPress[B_UP]) menuCursor = (menuCursor + MENU_N - 1) % MENU_N, redraw = true;
      if (shortPress[B_CONFIRM]) {
        if (menuCursor == 4) {
          screen = menuFrom;
        } else if (menuCursor == 3) {
          leave();
        } else {
          study.mark(menuCursor == 0   ? sumitsubo::Study::Mark::BadSentence
                     : menuCursor == 1 ? sumitsubo::Study::Mark::Suspend
                                       : sumitsubo::Study::Mark::Known);
          nextCard();
          redraw = true;
        }
      }
      break;
    case Screen::Done: {
      if (shortPress[B_BACK] || shortPress[B_CONFIRM]) {
        onGoHome(HomeMenuItem::STUDY);
        return;
      }
      if (shortPress[B_LEFT]) {
        sourceRows = study.sources();
        sourceCursor = 0;
        screen = Screen::Sources;
      } else if (shortPress[B_RIGHT]) {
        const auto s = study.summary();
        if (study.sessionSource() != sumitsubo::ALL_SOURCES && s.leftElsewhere + s.newElsewhere > 0) {
          study.start();  // go on into the other sources
          nextCard();
        } else if (study.settings().moreNew == "free" || (study.settings().moreNew == "once" && !offeredMore)) {
          offeredMore = true;
          if (study.addNew(study.settings().moreCount)) nextCard();
        }
        redraw = true;
      }
      break;
    }
  }
  if (redraw || screen != before) requestUpdate();
}

// ---- drawing: pieces -----------------------------------------------------------------------------

std::string StudyActivity::clock(const bool withTime) const {
  if (!clockTrusted || !withTime) return dayText(today, "%a %d %b");
  const time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  char buf[48];
  strftime(buf, sizeof buf, "%a %d %b  %H:%M", &tm);
  return buf;
}

std::string StudyActivity::sourceTitle(const int id) const {
  for (const auto& s : study.sources())
    if (s.source.id == id) return s.source.title;
  return "";
}

void StudyActivity::drawHeader(const char* left, const std::string& middle) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  renderer.drawText(NOTE_FONT, SIDE, 12, left, true, EpdFontFamily::BOLD);
  const int mw = renderer.getTextWidth(NOTE_FONT, middle.c_str());
  renderer.drawText(NOTE_FONT, (w - mw) / 2, 12, middle.c_str());
  // The battery, with its percentage, where Matcha's own screens have it.
  const int bw = renderer.getTextWidth(SMALL_FONT_ID, "100%") + metrics.batteryWidth + 14;
  GUI.drawBatteryLeft(renderer, Rect{w - SIDE - bw, 14, metrics.batteryWidth, metrics.batteryHeight}, true);
  renderer.fillRect(0, HEADER_H - 2, w, 2);
}

void StudyActivity::drawFooter(const char* back, const char* confirm, const char* left, const char* right, const char* side) const {
  const int w = renderer.getScreenWidth(), h = renderer.getScreenHeight();
  const int top = h - FOOT_H;
  if (side && *side) {  // what the two side buttons do: "name|action" pairs separated by tabs
    const int y = top - SIDE_LINE_H;
    renderer.drawLine(0, y, w, y);
    int x = SIDE;
    std::string all = side;
    size_t start = 0;
    for (;;) {
      const size_t tab = all.find('\t', start);
      const std::string part = all.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
      const size_t bar = part.find('|');
      const std::string name = part.substr(0, bar), action = bar == std::string::npos ? "" : part.substr(bar + 1);
      renderer.drawText(RUBY_FONT, x, y + 6, name.c_str());
      x += renderer.getTextWidth(RUBY_FONT, name.c_str()) + 8;
      renderer.drawText(NOTE_FONT, x, y + 3, action.c_str(), true, EpdFontFamily::BOLD);
      x += renderer.getTextWidth(NOTE_FONT, action.c_str(), EpdFontFamily::BOLD) + 28;
      if (tab == std::string::npos) break;
      start = tab + 1;
    }
  }
  renderer.fillRect(0, top, w, 2);
  const char* const names[4] = {"Back", "Confirm", "Left", "Right"};
  const char* const actions[4] = {back, confirm, left, right};
  const int cell = w / 4;
  for (int i = 0; i < 4; ++i) {
    const int x = i * cell;
    if (i) renderer.drawLine(x, top, x, h);
    const bool live = actions[i] && *actions[i];
    const int nw = renderer.getTextWidth(RUBY_FONT, names[i]);
    renderer.drawText(RUBY_FONT, x + (cell - nw) / 2, top + 6, names[i]);
    if (!live) continue;  // the cell stays, so the others never shift; it just does nothing now
    const int aw = renderer.getTextWidth(NOTE_FONT, actions[i], EpdFontFamily::BOLD);
    renderer.drawText(NOTE_FONT, x + (cell - aw) / 2, top + 22, actions[i], true, EpdFontFamily::BOLD);
  }
}

// A sentence, wrapped at any character as Japanese is: the tested part underlined (or left as a
// blank), furigana above the words that carry it. Returns the y below the last line.
int StudyActivity::drawSentence(const sumitsubo::Sentence& s, const int x0, const int y0, const int width, const int fontId,
                                const bool blankTarget, const bool draw) const {
  bool hasRuby = false;
  for (const auto& r : s.runs) hasRuby |= !r.ruby.empty();
  const int rubyH = hasRuby ? renderer.getLineHeight(RUBY_FONT) - 2 : 0;
  const int charH = renderer.getLineHeight(fontId);
  const int lineH = charH + rubyH + 4;
  const int em = renderer.getTextWidth(fontId, "国");
  int x = x0, y = y0 + rubyH;
  bool blankDrawn = false;
  // The tested word is marked as one piece per line: from where it starts on a line to where it ends.
  int markX = 0, markY = 0, markW = 0;
  const auto flush = [&] {
    if (markW > 0 && draw) drawMark(markX, markY, markW, charH, 1);
    markW = 0;
  };
  const auto mark = [&](const int px, const int py, const int pw) {
    if (markW > 0 && (py != markY || px != markX + markW)) flush();
    if (markW == 0) markX = px, markY = py;
    markW += pw;
  };
  for (const auto& run : s.runs) {
    if (!run.target) flush();
    if (run.target && blankTarget) {
      if (blankDrawn) continue;
      blankDrawn = true;
      const int bw = em * 4;
      if (x + bw > x0 + width) x = x0, y += lineH;
      if (draw) renderer.fillRect(x + 4, y + charH - 3, bw - 8, 3);
      x += bw;
      continue;
    }
    if (!run.target) blankDrawn = false;
    if (!run.ruby.empty()) {  // a word with furigana stays on one line
      const int bw = renderer.getTextWidth(fontId, run.text.c_str());
      const int rw = renderer.getTextWidth(RUBY_FONT, run.ruby.c_str());
      const int cell = std::max(bw, rw);
      if (x + cell > x0 + width && x > x0) x = x0, y += lineH;
      if (draw) {
        renderer.drawText(fontId, x + (cell - bw) / 2, y, run.text.c_str());
        renderer.drawText(RUBY_FONT, x + (cell - rw) / 2, y - rubyH, run.ruby.c_str());
      }
      if (run.target) mark(x, y, cell);
      x += cell;
      continue;
    }
    for (size_t i = 0; i < run.text.size();) {
      const size_t n = utf8Len(static_cast<unsigned char>(run.text[i]));
      const std::string ch = run.text.substr(i, n);
      i += n;
      const int cw = renderer.getTextWidth(fontId, ch.c_str());
      if (x + cw > x0 + width && x > x0 && !closesLine(ch)) x = x0, y += lineH;
      if (draw) {
        renderer.drawText(fontId, x, y, ch.c_str());
      }
      if (run.target) mark(x, y, cw);
      x += cw;
    }
  }
  flush();
  return y + lineH;
}

// The largest of three type sizes at which the sentence fits in `room` pixels of height.
int StudyActivity::fitFont(const sumitsubo::Sentence& s, const int width, const int room, const bool blankTarget) const {
  static const int sizes[] = {NOTOSERIF_16_FONT_ID, NOTOSERIF_14_FONT_ID, NOTOSERIF_12_FONT_ID};
  for (const int f : sizes)
    if (drawSentence(s, 0, 0, width, f, blankTarget, false) <= room) return f;
  return sizes[2];
}

int StudyActivity::drawWrapped(const int fontId, const std::string& text, const int x, int y, const int width, const int maxLines,
                               const bool bold) const {
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  for (const auto& line : renderer.wrappedText(fontId, text.c_str(), width, maxLines, style)) {
    renderer.drawText(fontId, x, y, line.c_str(), true, style);
    y += renderer.getLineHeight(fontId);
  }
  return y;
}

// The whole definition, sense by sense, for as long as there is room; `cut` says it did not fit.
int StudyActivity::drawSenses(const std::string& gloss, const int x, int y, const int width, const int bottom, bool& cut, const int from,
                              int* const shown) const {
  cut = false;
  if (shown) *shown = 0;
  const int lineH = renderer.getLineHeight(TEXT_FONT);
  std::vector<std::string> senses;
  size_t start = 0;
  for (;;) {
    const size_t bar = gloss.find(" | ", start);
    senses.push_back(gloss.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
    if (bar == std::string::npos) break;
    start = bar + 3;
  }
  for (size_t i = static_cast<size_t>(std::max(0, from)); i < senses.size(); ++i) {
    const std::string text = senses.size() > 1 ? std::to_string(i + 1) + ". " + senses[i] : senses[i];
    const auto lines = renderer.wrappedText(TEXT_FONT, text.c_str(), width, 12);
    if (y + static_cast<int>(lines.size()) * lineH > bottom) {
      cut = true;
      break;
    }
    for (const auto& line : lines) {
      renderer.drawText(TEXT_FONT, x, y, line.c_str());
      y += lineH;
    }
    if (shown) ++*shown;
    y += 4;
  }
  return y;
}

// ---- drawing: screens ----------------------------------------------------------------------------

void StudyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int w = renderer.getScreenWidth() - 2 * SIDE;
  switch (screen) {
    case Screen::Message:
      drawHeader("STUDY", "");
      drawWrapped(TEXT_FONT, message, SIDE, HEADER_H + 40, w, 10);
      drawFooter("Home", "Home", "", "", nullptr);
      break;
    case Screen::DateCheck: drawDateCheck(); break;
    case Screen::Today: drawToday(); break;
    case Screen::CardFront:
    case Screen::CardBack:
      drawCard(screen == Screen::CardBack);
      if (prefs.smooth) {
        renderer.displayBuffer();
        smoothCard(screen == Screen::CardBack);
        return;
      }
      break;
    case Screen::Entry: {
      drawHeader(kindName(card.kind), "in full");
      if (card.kind == Kind::Kanji) {
        drawKanjiAnswer(HEADER_H + 20, renderer.getScreenHeight() - FOOT_H - 10, 12);
        drawFooter("Back", "Back", "", "", nullptr);
        break;
      }
      int y = HEADER_H + 20;
      renderer.drawText(WORD_FONT, SIDE, y, (card.head + "  " + card.reading).c_str());
      y += renderer.getLineHeight(WORD_FONT) + 12;
      drawSenses(card.gloss, SIDE, y, w, renderer.getScreenHeight() - FOOT_H - 10, entryCut, entryFrom, &entryShown);
      drawFooter("Back", "Back", entryFrom > 0 ? "First" : "", entryCut ? "More" : "", nullptr);
      break;
    }
    case Screen::Menu: {
      drawHeader(kindName(card.kind), "this card");
      int y = HEADER_H + 40;
      const int rowH = renderer.getLineHeight(TEXT_FONT) + 22;
      for (int i = 0; i < MENU_N; ++i) {
        if (i == menuCursor) renderer.fillRect(SIDE - 8, y - 8, w + 16, rowH - 6);
        renderer.drawText(TEXT_FONT, SIDE, y, MENU[i], i != menuCursor);
        y += rowH;
      }
      drawFooter("Cancel", "Choose", "", "", "Side Up / Down|move");
      break;
    }
    case Screen::Done: drawDone(); break;
    case Screen::Sources: drawSources(); break;
    case Screen::Options: drawOptions(); break;
  }
  renderer.displayBuffer();
}

void StudyActivity::drawDateCheck() const {
  const int w = renderer.getScreenWidth() - 2 * SIDE;
  drawHeader("STUDY", "");
  int y = HEADER_H + 50;
  y = drawWrapped(TEXT_FONT, "The clock may be wrong. Reviews are scheduled by day, so the date has to be right.", SIDE, y, w, 4);
  y += 30;
  renderer.drawText(TEXT_FONT, SIDE, y, "Is today");
  y += renderer.getLineHeight(TEXT_FONT) + 14;
  renderer.drawText(WORD_FONT, SIDE, y, dayText(today, "%A").c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(WORD_FONT) + 4;
  renderer.drawText(WORD_FONT, SIDE, y, (dayText(today, "%d %B %Y") + " ?").c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(WORD_FONT) + 30;
  if (study.lastReviewDay())
    drawWrapped(NOTE_FONT, "Your last review was recorded on " + dayText(study.lastReviewDay(), "%A %d %B") + ".", SIDE, y, w, 2);
  drawFooter("Home", "Yes", "", "Yes", "Side Up / Down|a day later / earlier");
}

void StudyActivity::drawToday() const {
  const int sw = renderer.getScreenWidth(), w = sw - 2 * SIDE;
  drawHeader("STUDY", clock(true));
  const auto c = study.counts();
  int y = HEADER_H + 34;
  const int boxW = (w - 14) / 2, boxH = 118;
  const std::string nums[2] = {std::to_string(c.reviews), std::to_string(c.fresh)};
  const char* const names[2] = {"reviews due", "new today"};
  for (int i = 0; i < 2; ++i) {
    const int x = SIDE + i * (boxW + 14);
    renderer.drawRect(x, y, boxW, boxH, 2, true);
    renderer.drawTextScaled(WORD_FONT, x + 16, y + 10, nums[i].c_str(), 448, true, EpdFontFamily::BOLD);
    renderer.drawText(NOTE_FONT, x + 16, y + boxH - 34, names[i]);
  }
  y += boxH + 26;
  const int minutes = std::max(1, (c.reviews + c.fresh) * 19 / 60);
  renderer.drawText(TEXT_FONT, SIDE, y, ("About " + std::to_string(minutes) + (minutes == 1 ? " minute." : " minutes.")).c_str());
  y += renderer.getLineHeight(TEXT_FONT) + 26;
  renderer.drawText(RUBY_FONT, SIDE, y, "REVIEWS", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(RUBY_FONT) + 6;
  const std::string kinds = std::to_string(c.byKind[0]) + " word  -  " + std::to_string(c.byKind[1]) + " kanji  -  " +
                            std::to_string(c.byKind[2]) + " collocation  -  " + std::to_string(c.byKind[3]) + " contrast";
  renderer.drawText(NOTE_FONT, SIDE, y, kinds.c_str());
  y += renderer.getLineHeight(NOTE_FONT) + 30;
  drawWrapped(NOTE_FONT, "To study one source only, or to see how far along each one is, open Sources. Hold Confirm for the options.", SIDE, y, w, 3);
  drawFooter("Home", "Start", "Sources", "Start", nullptr);
}

// A second pass over a card that is already on the screen in black and white: the letters are
// drawn again into two planes that tell the panel which of their edge pixels are light or dark
// grey (the reader's own text smoothing, in its plain whole-screen form). Anything drawn in
// black leaves the planes untouched, so the whole card can simply be drawn again.
void StudyActivity::smoothCard(const bool back) {
  renderer.waitRefreshComplete();
  if (!renderer.storeBwBuffer()) return;  // no memory to set the page aside: it stays black and white
  for (int plane = 0; plane < 2; ++plane) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(plane == 0 ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
    drawCard(back);
    if (plane == 0) renderer.copyGrayscaleLsbBuffers();
    else renderer.copyGrayscaleMsbBuffers();
  }
  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}

// The tested word in its sentence, or the tested kanji in its word, marked the way Options says.
// (x, y, w, h) is the text's own box; scale is 2 for the large word of a kanji card.
void StudyActivity::drawMark(const int x, const int y, const int w, const int h, const int scale) const {
  switch (prefs.mark) {
    case 1: {  // a grey ground: one pixel in eight, in a staggered grid, which leaves the strokes clean
      const int x0 = x - 2 * scale, x1 = x + w + 2 * scale, y0 = y - scale, y1 = y + h + 2 * scale;
      for (int py = y0; py < y1; ++py) {
        if (py & 1) continue;
        for (int px = x0 + ((py & 2) ? 2 : 0) - (x0 & 3) + 4; px < x1; px += 4) renderer.drawPixel(px, py);
      }
      break;
    }
    case 2:
      renderer.drawRoundedRect(x - 3 * scale, y - 2 * scale, w + 6 * scale, h + 5 * scale, 2 * scale, 6 * scale, true);
      break;
    case 3:
      renderer.fillRect(x, y + h - 2, w, 6 * scale);
      break;
    default:
      renderer.fillRect(x, y + h - 3, w, 3 * scale);
  }
}

// Hold Back on a card: this sentence is no good. The card comes back with its next sentence, or
// steps aside for today when it has no other.
void StudyActivity::badSentence() {
  if (card.sentences.empty()) return;
  study.mark(sumitsubo::Study::Mark::BadSentence);
  nextCard();
}

void StudyActivity::drawOptions() const {
  const int w = renderer.getScreenWidth() - 2 * SIDE;
  drawHeader("STUDY", "options");
  const char* const names[OPTIONS_N] = {"Button hints on cards", "Books: mark new words", "Books: furigana only where needed",
                                        "Tilt to answer a card", "Cards: smooth letters", "Cards: the tested word"};
  const char* const notes[OPTIONS_N] = {
      "Also switched by holding Side Down on a card.",
      "A line by each word still new to you, in books the desktop sent.",
      "Furigana is left off words you know well.",
      "Flick as to turn a page: forward Good, back Again, up Easy, down Hard. Needs tilt page turn in Settings.",
      "Grey at the edges of letters; a card settles a moment later.",
      "",
  };
  const bool values[OPTIONS_N] = {prefs.hints, prefs.markNew, prefs.furiganaNeeded, prefs.tilt, prefs.smooth, false};
  int y = HEADER_H + 28;
  for (int i = 0; i < OPTIONS_N; ++i) {
    const int top = y;
    const int rowH = renderer.getLineHeight(TEXT_FONT) + 14;
    if (i == optionCursor) renderer.fillRect(SIDE - 8, top - 6, w + 16, rowH);
    renderer.drawText(TEXT_FONT, SIDE, y, names[i], i != optionCursor, EpdFontFamily::BOLD);
    const char* const v = i == MARK_OPTION ? (prefs.mark == 2 ? "Box" : prefs.mark == 3 ? "Underline" : prefs.mark == 1 ? "Grey" : "Thin line")
                                           : values[i] ? "On" : "Off";
    renderer.drawText(TEXT_FONT, SIDE + w - renderer.getTextWidth(TEXT_FONT, v, EpdFontFamily::BOLD), y, v, i != optionCursor, EpdFontFamily::BOLD);
    y += rowH + 4;
    y = drawWrapped(NOTE_FONT, notes[i], SIDE, y, w, 3) + 22;
  }
  drawFooter("Back", "Change", "", "", "Side Up / Down|move");
}

void StudyActivity::drawCard(const bool back) const {
  drawHeader(kindName(card.kind), std::to_string(study.position()) + " / " + std::to_string(study.length()));
  int bottom = renderer.getScreenHeight() - 10;
  if (!hintsHidden) bottom = renderer.getScreenHeight() - FOOT_H - SIDE_LINE_H - 8;
  const_cast<StudyActivity*>(this)->definitionCut = false;
  if (card.kind == Kind::Kanji) drawKanji(back, HEADER_H + 20, bottom);
  else if (card.kind == Kind::Contrast) drawContrast(back, HEADER_H + 20, bottom);
  else drawWordLike(back, HEADER_H + 20, bottom);
  if (hintsHidden) return;
  if (!back) {
    const bool many = card.sentences.size() > 1;
    const bool has = !card.sentences.empty();
    drawFooter(study.canUndo() ? "Undo" : "Leave", "Show", "Show", "Show",
               many ? "Side Up / Down|next sentence\tHold Back|bad one" : has ? "Hold Back|bad sentence" : "");
  } else {
    drawFooter(study.canUndo() ? "Undo" : "Leave", definitionCut ? "Show all" : "Menu (hold)", "Again", "Good",
               card.sentences.empty() ? "Side Up|Easy\tSide Down|Hard" : "Side Up|Easy\tDown|Hard\tHold Back|bad sentence");
  }
}

void StudyActivity::drawWordLike(const bool back, const int top, const int bottom) const {
  const int w = renderer.getScreenWidth() - 2 * SIDE;
  const bool colloc = card.kind == Kind::Colloc;
  const int room = bottom - top - 230;  // what the question may take, leaving the answer its share
  int y = top;
  if (!card.sentences.empty()) {
    const auto& s = card.sentences[0];
    y = drawSentence(s, SIDE, y, w, fitFont(s, w, room - 30, colloc), colloc && !back);
    if (colloc && study.currentIsYoung() && !card.extra.empty()) {  // the hint fades once the card is mature
      const std::string hint = "Hint: " + card.extra;
      const int hw = renderer.getTextWidth(NOTE_FONT, hint.c_str());
      renderer.drawRect(SIDE, y + 4, hw + 20, renderer.getLineHeight(NOTE_FONT) + 10);
      renderer.drawText(NOTE_FONT, SIDE + 10, y + 9, hint.c_str());
      y += renderer.getLineHeight(NOTE_FONT) + 20;
    }
    std::string where = sourceTitle(s.source);
    if (!s.place.empty()) where += "  -  " + s.place;
    if (card.sentences.size() > 1)
      where += "  -  sentence " + std::to_string(study.sentenceIndex() % static_cast<int>(card.sentences.size()) + 1) + " of " +
               std::to_string(card.sentences.size());
    renderer.drawText(RUBY_FONT, SIDE, y + 4, renderer.truncatedText(RUBY_FONT, where.c_str(), w).c_str());
    y += renderer.getLineHeight(RUBY_FONT) + 4;
  } else {
    renderer.drawText(WORD_FONT, SIDE, y, card.head.c_str());
    y += renderer.getLineHeight(WORD_FONT);
  }
  // The answer area starts under the question, at the same place before and after it is shown.
  const int answerTop = std::max(top + 200, y + 12);
  for (int x = SIDE; x < SIDE + w; x += 8) renderer.drawLine(x, answerTop, x + 4, answerTop);
  if (!back) return;
  y = answerTop + 16;
  renderer.drawText(WORD_FONT, SIDE, y, card.reading.c_str(), true, EpdFontFamily::BOLD);
  const int rw = renderer.getTextWidth(WORD_FONT, card.reading.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(BODY_FONT, SIDE + rw + 18, y + 3, card.head.c_str());
  y += renderer.getLineHeight(WORD_FONT) + 10;
  bool cut = false;
  drawSenses(card.gloss, SIDE, y, w, bottom - 24, cut);
  if (cut) {
    const char* more = "... more senses: Confirm shows all";
    renderer.drawText(NOTE_FONT, SIDE + w - renderer.getTextWidth(NOTE_FONT, more, EpdFontFamily::BOLD), bottom - 22, more, true, EpdFontFamily::BOLD);
  }
  const_cast<StudyActivity*>(this)->definitionCut = cut;
}

void StudyActivity::drawKanji(const bool back, const int top, const int bottom) const {
  const int sw = renderer.getScreenWidth(), w = sw - 2 * SIDE;
  const std::string word = card.words.empty() ? card.head : card.words[0].word;
  // The word, large and centred, the tested kanji underlined.
  // Each character comes from the deck's own large drawings when it has them (true size, four
  // greys); one it lacks is the reader font enlarged, which is rougher.
  constexpr uint16_t SCALE = 640;  // 2.5 x
  auto& bigGlyphs = const_cast<StudyActivity*>(this)->big;
  std::vector<std::pair<std::string, const sumitsubo::BigGlyph*>> parts;
  int total = 0;
  for (size_t i = 0; i < word.size();) {
    const size_t n = utf8Len(static_cast<unsigned char>(word[i]));
    parts.emplace_back(word.substr(i, n), nullptr);
    i += n;
  }
  // looked up in one go: a later lookup may move the few glyphs that are kept
  bool allBig = bigGlyphs.ready() && parts.size() <= 8;
  for (auto& part : parts) allBig = allBig && bigGlyphs.find(sumitsubo::firstCodepoint(part.first)) != nullptr;
  for (auto& part : parts) {
    if (allBig) part.second = bigGlyphs.find(sumitsubo::firstCodepoint(part.first));
    total += part.second ? part.second->advance : renderer.getTextWidthScaled(WORD_FONT, part.first.c_str(), SCALE);
  }
  int x = (sw - total) / 2;
  const int bigH = allBig ? bigGlyphs.lineHeight() : renderer.getLineHeightScaled(WORD_FONT, SCALE);
  const auto mode = renderer.getRenderMode();
  for (const auto& part : parts) {
    const std::string& ch = part.first;
    int cw;
    if (const sumitsubo::BigGlyph* g = part.second) {
      cw = g->advance;
      for (int gy = 0; gy < g->height; ++gy) {
        for (int gx = 0; gx < g->width; ++gx) {
          const uint8_t level = g->level(gx, gy);
          if (!level) continue;
          const int px = x + g->left + gx, py = top + 6 + g->top + gy;
          // As the reader draws its own letters: every inked pixel black first, then the two grey
          // planes say which of them are to be lightened. Without smoothing, the faint edge is left off.
          if (mode == GfxRenderer::BW) {
            if (prefs.smooth || level >= 2) renderer.drawPixel(px, py, true);
          } else if (mode == GfxRenderer::GRAYSCALE_MSB) {
            if (level == 1 || level == 2) renderer.drawPixel(px, py, false);
          } else if (level == 2) {
            renderer.drawPixel(px, py, false);
          }
        }
      }
    } else {
      cw = renderer.getTextWidthScaled(WORD_FONT, ch.c_str(), SCALE);
      renderer.drawTextScaled(WORD_FONT, x, top + 6, ch.c_str(), SCALE);
    }
    if (ch == card.head && word != card.head && mode == GfxRenderer::BW) drawMark(x, top + 6, cw, bigH, 2);
    x += cw;
  }
  int y = top + bigH + 30;
  if (!card.sentence.empty()) {
    const int room = bottom - y - 230;
    y = drawSentence(card.sentence, SIDE, y, w, fitFont(card.sentence, w, room - 30, false), false);
    renderer.drawText(RUBY_FONT, SIDE, y + 4,
                      renderer.truncatedText(RUBY_FONT, (sourceTitle(card.sentence.source) + "  -  " + card.sentence.place).c_str(), w).c_str());
    y += renderer.getLineHeight(RUBY_FONT) + 4;
  }
  const int answerTop = std::max(top + 200, y + 12);
  for (int dx = SIDE; dx < SIDE + w; dx += 8) renderer.drawLine(dx, answerTop, dx + 4, answerTop);
  if (!back) return;
  y = answerTop + 14;
  const bool cut = drawKanjiAnswer(y, bottom, 3);
  const_cast<StudyActivity*>(this)->definitionCut = cut;
}

// The kanji with its reading and meaning, then every word of yours that uses the reading, the one
// from the front first. Each word has its reading beside it and its meaning underneath, in full
// for up to `maxLines` lines. Returns true when something did not fit (Confirm then shows all).
bool StudyActivity::drawKanjiAnswer(int y, const int bottom, const int maxLines) const {
  const int w = renderer.getScreenWidth() - 2 * SIDE;
  const int noteH = renderer.getLineHeight(NOTE_FONT);
  bool cut = false;
  auto meaning = [&](std::string gloss, const int x, int yy, const int width) {
    for (size_t at; (at = gloss.find(" | ")) != std::string::npos;) gloss.replace(at, 3, "  /  ");
    const auto all = renderer.wrappedText(NOTE_FONT, gloss.c_str(), width, 12);
    const auto lines = renderer.wrappedText(NOTE_FONT, gloss.c_str(), width, maxLines);
    cut |= all.size() > lines.size();
    for (const auto& line : lines) {
      renderer.drawText(NOTE_FONT, x, yy, line.c_str());
      yy += noteH;
    }
    return yy;
  };
  renderer.drawText(WORD_FONT, SIDE, y, card.head.c_str(), true, EpdFontFamily::BOLD);
  const int hw = renderer.getTextWidth(WORD_FONT, card.head.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(WORD_FONT, SIDE + hw + 14, y, card.reading.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(WORD_FONT) + 2;
  y = meaning(card.gloss, SIDE, y, w) + 8;
  bool first = true;
  const int headH = renderer.getLineHeight(BODY_FONT);
  for (const auto& wd : card.words) {
    if (wd.word == card.head && wd.reading == card.reading) continue;  // the kanji as a word of its own: already above
    if (y + headH + noteH + 12 > bottom) {
      cut = true;
      break;
    }
    renderer.drawLine(SIDE, y, SIDE + w, y);
    const auto style = first ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(BODY_FONT, SIDE, y + 6, wd.word.c_str(), true, style);
    const int ww = renderer.getTextWidth(BODY_FONT, wd.word.c_str(), style);
    renderer.drawText(TEXT_FONT, SIDE + ww + 18, y + 9, wd.reading.c_str(), true, style);
    y = meaning(wd.gloss, SIDE, y + 6 + headH, w) + 6;
    first = false;
  }
  return cut;
}

void StudyActivity::drawContrast(const bool back, const int top, const int bottom) const {
  const int w = renderer.getScreenWidth() - 2 * SIDE;
  const size_t n = card.members.size();
  if (n == 0) return;
  const bool kanji = card.head == "look" || card.head == "read";
  if (kanji) {  // two by two, the kanji large, each with a word that uses it
    const int cellW = (w - 14) / 2, cellH = std::min(250, (bottom - top - 14) / 2);
    constexpr uint16_t SCALE = 640;
    for (size_t i = 0; i < n && i < 4; ++i) {
      const auto& m = card.members[i];
      const int x = SIDE + static_cast<int>(i % 2) * (cellW + 14), y = top + static_cast<int>(i / 2) * (cellH + 14);
      renderer.drawRect(x, y, cellW, cellH, 2, true);
      const int kw = renderer.getTextWidthScaled(WORD_FONT, m.head.c_str(), SCALE);
      renderer.drawTextScaled(WORD_FONT, x + (cellW - kw) / 2, y + 6, m.head.c_str(), SCALE, true, EpdFontFamily::BOLD);
      int ty = y + 12 + renderer.getLineHeightScaled(WORD_FONT, SCALE);
      const int ww = renderer.getTextWidth(BODY_FONT, m.word.c_str());
      renderer.drawText(BODY_FONT, x + (cellW - ww) / 2, ty, m.word.c_str());
      if (!back) continue;
      ty += renderer.getLineHeight(BODY_FONT) + 2;
      const int rw = renderer.getTextWidth(TEXT_FONT, m.reading.c_str(), EpdFontFamily::BOLD);
      renderer.drawText(TEXT_FONT, x + (cellW - rw) / 2, ty, m.reading.c_str(), true, EpdFontFamily::BOLD);
      ty += renderer.getLineHeight(TEXT_FONT);
      const std::string g = renderer.truncatedText(RUBY_FONT, m.gloss.c_str(), cellW - 12);
      renderer.drawText(RUBY_FONT, x + (cellW - renderer.getTextWidth(RUBY_FONT, g.c_str())) / 2, ty, g.c_str());
    }
    return;
  }
  // Words: stacked, each with its sentence; the answer adds reading, role and meaning.
  const int rowH = std::min(200, (bottom - top - 14 * static_cast<int>(std::min<size_t>(n, 4) - 1)) / static_cast<int>(std::min<size_t>(n, 4)));
  for (size_t i = 0; i < n && i < 4; ++i) {
    const auto& m = card.members[i];
    const int y = top + static_cast<int>(i) * (rowH + 14);
    renderer.drawRect(SIDE, y, w, rowH, 2, true);
    int ty = y + 10;
    renderer.drawText(WORD_FONT, SIDE + 14, ty, m.word.c_str(), true, EpdFontFamily::BOLD);
    if (back) {
      const int ww = renderer.getTextWidth(WORD_FONT, m.word.c_str(), EpdFontFamily::BOLD);
      const std::string r = m.role.empty() ? m.reading : m.reading + "  (" + m.role + ")";
      renderer.drawText(TEXT_FONT, SIDE + 14 + ww + 16, ty + 6, r.c_str(), true, EpdFontFamily::BOLD);
    }
    ty += renderer.getLineHeight(WORD_FONT) + 4;
    const int lineH = renderer.getLineHeight(NOTE_FONT);
    // The sentence sits at the foot of the row, in the largest type that fits two lines; one that
    // would not fit even small is left out.
    int sentFont = 0, sentH = 0;
    if (!m.sentence.empty()) {
      sentFont = fitFont(m.sentence, w - 28, rowH / 2, false);
      sentH = drawSentence(m.sentence, 0, 0, w - 28, sentFont, false, false);
      if (sentH > rowH / 2) sentFont = 0, sentH = 0;
    }
    const int room = y + rowH - 8 - ty - sentH;
    if (back) {
      for (const auto& line : renderer.wrappedText(NOTE_FONT, m.gloss.c_str(), w - 28, std::max(1, room / lineH))) {
        renderer.drawText(NOTE_FONT, SIDE + 14, ty, line.c_str());
        ty += lineH;
      }
    }
    if (sentFont) drawSentence(m.sentence, SIDE + 14, y + rowH - sentH - 4, w - 28, sentFont, false);
  }
}

void StudyActivity::drawDone() const {
  const int w = renderer.getScreenWidth() - 2 * SIDE;
  drawHeader("STUDY", clock(true));
  const auto s = study.summary();
  const bool one = study.sessionSource() != sumitsubo::ALL_SOURCES;
  int y = HEADER_H + 36;
  y = drawWrapped(WORD_FONT, one ? "Done with " + sourceTitle(study.sessionSource()) + "." : "Done for today.", SIDE, y, w, 3, true);
  y += 22;
  const int lineH = renderer.getLineHeight(TEXT_FONT) + 6;
  renderer.drawText(TEXT_FONT, SIDE, y, (std::to_string(s.cards) + " cards").c_str());
  y += lineH;
  renderer.drawText(TEXT_FONT, SIDE, y,
                    (std::to_string(s.good) + " Good  -  " + std::to_string(s.easy) + " Easy  -  " + std::to_string(s.hard) + " Hard  -  " +
                     std::to_string(s.again) + " Again").c_str());
  y += lineH;
  renderer.drawText(TEXT_FONT, SIDE, y, (std::to_string(s.started) + " new cards started").c_str());
  y += lineH + 16;
  renderer.drawLine(SIDE, y, SIDE + w, y);
  y += 18;
  renderer.drawText(TEXT_FONT, SIDE, y, ("Tomorrow: " + std::to_string(s.dueTomorrow) + " reviews due.").c_str());
  y += lineH + 24;
  const char* left = "";
  if (one && s.leftElsewhere + s.newElsewhere > 0) {
    renderer.drawRect(SIDE, y, w, 2 * lineH + 20, 2, true);
    renderer.drawText(TEXT_FONT, SIDE + 14, y + 10,
                      (std::to_string(s.leftElsewhere) + " reviews from other sources are still due today.").c_str(), true, EpdFontFamily::BOLD);
    renderer.drawText(NOTE_FONT, SIDE + 14, y + 12 + lineH, ("and " + std::to_string(s.newElsewhere) + " new cards").c_str());
    left = "Continue";
  } else if (study.settings().moreNew == "free" || (study.settings().moreNew == "once" && !offeredMore)) {
    left = "More new";
  }
  drawFooter("Home", "Home", "Sources", left, nullptr);
}

void StudyActivity::drawSources() const {
  const int w = renderer.getScreenWidth() - 2 * SIDE;
  const int n = static_cast<int>(sourceRows.size());
  const int page = sourceCursor / SOURCES_PER_PAGE, first = page * SOURCES_PER_PAGE;
  const int lastShown = std::min(n, first + SOURCES_PER_PAGE);
  drawHeader("SOURCES", n ? std::to_string(first + 1) + " - " + std::to_string(lastShown) + " of " + std::to_string(n) : "");
  const auto t = study.totals();
  int y = HEADER_H + 10;
  renderer.drawText(RUBY_FONT, SIDE, y,
                    ("Your words: " + std::to_string(t.mature) + " mature  -  " + std::to_string(t.young) + " young  -  " + std::to_string(t.fresh) + " new").c_str());
  y += renderer.getLineHeight(RUBY_FONT) + 8;
  renderer.fillRect(SIDE, y, w, 2);
  const int rowH = 126;
  for (int i = first; i < lastShown; ++i) {
    const auto& s = sourceRows[static_cast<size_t>(i)];
    const bool on = i == sourceCursor;
    if (on) renderer.fillRect(SIDE - 8, y + 2, w + 16, rowH - 2);
    const bool ink = !on;
    const std::string pct = std::to_string(s.readablePct) + "%";
    const int pw = renderer.getTextWidth(WORD_FONT, pct.c_str(), EpdFontFamily::BOLD);
    const char* tag = s.source.priority == 1 ? "  [First]" : s.source.priority == 3 ? "  [Later]" : "";
    renderer.drawText(TEXT_FONT, SIDE, y + 16, renderer.truncatedText(TEXT_FONT, (s.source.title + tag).c_str(), w - pw - 90, EpdFontFamily::BOLD).c_str(), ink,
                      EpdFontFamily::BOLD);
    renderer.drawText(WORD_FONT, SIDE + w - pw - 66, y + 8, pct.c_str(), ink, EpdFontFamily::BOLD);
    renderer.drawText(RUBY_FONT, SIDE + w - 60, y + 24, "readable", ink);
    const int barY = y + 58;
    renderer.drawRect(SIDE, barY, w, 9, ink);
    renderer.fillRect(SIDE, barY, w * std::min(100, s.readablePct) / 100, 9, ink);
    std::string line = "was " + std::to_string(static_cast<int>(s.source.addedPct + 0.5f)) + "% when added  -  ";
    if (s.newTotal > 0 && s.started < s.newTotal) {
      line += std::to_string(s.started) + " of " + std::to_string(s.newTotal) + " new words started (" + std::to_string(100 * s.started / s.newTotal) + "%)";
      if (s.lastNewDay) line += "  -  the last by " + dayText(s.lastNewDay, "%d %b");
    } else {
      line += "all " + std::to_string(s.newTotal) + " new words started";
    }
    renderer.drawText(RUBY_FONT, SIDE, barY + 16, renderer.truncatedText(RUBY_FONT, line.c_str(), w).c_str(), ink);
    renderer.drawText(RUBY_FONT, SIDE, barY + 16 + renderer.getLineHeight(RUBY_FONT),
                      ("today: " + std::to_string(s.reviewsToday) + " reviews  -  " + std::to_string(s.newToday) + " new").c_str(), ink);
    y += rowH;
    renderer.drawLine(SIDE, y, SIDE + w, y);
  }
  drawFooter("Back", "Study this", "", "", "Side Up / Down|move, and turn the page");
}
