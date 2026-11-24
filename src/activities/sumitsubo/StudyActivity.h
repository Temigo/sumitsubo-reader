#pragma once
// Sumitsubo's Study mode: today's reviews and new cards from the deck the desktop program wrote.
// One activity, several screens. The scheduling and the deck live in lib/Sumitsubo, which knows
// nothing of this firmware; this file only draws and reads buttons. Design: docs/sumitsubo-design.md
#include <BigGlyphs.h>
#include <ReaderLink.h>
#include <Study.h>

#include <memory>
#include <string>
#include <vector>

#include "SdStore.h"
#include "activities/Activity.h"

class StudyActivity final : public Activity {
 public:
  explicit StudyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Study", renderer, mappedInput), study(store) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen { Message, DateCheck, Today, CardFront, CardBack, Entry, Menu, Done, Sources, Options };
  sumitsubo::Prefs prefs;
  int optionCursor = 0;
  void badSentence();
  void smoothCard(bool back);
  sumitsubo::BigGlyphs big;
  void drawMark(int x, int y, int w, int h, int scale) const;
  void drawOptions() const;
  SdStore store;
  sumitsubo::Study study;
  sumitsubo::Card card;
  Screen screen = Screen::Message;
  Screen menuFrom = Screen::CardBack;  // where the card menu was opened, to return there
  std::string message;
  int32_t today = 0;
  bool clockTrusted = false, hintsHidden = false, offeredMore = false;
  int sourceCursor = 0, menuCursor = 0;
  unsigned long pressedAt[6] = {0, 0, 0, 0, 0, 0};
  bool longFired[6] = {false, false, false, false, false, false};
  std::vector<sumitsubo::SourceStats> sourceRows;

  void open(int32_t day);
  void nextCard();
  void drawHeader(const char* left, const std::string& middle) const;
  void drawFooter(const char* back, const char* confirm, const char* left, const char* right, const char* side) const;
  int drawSentence(const sumitsubo::Sentence& s, int x, int y, int width, int fontId, bool blankTarget, bool draw = true) const;
  int fitFont(const sumitsubo::Sentence& s, int width, int room, bool blankTarget) const;
  int drawWrapped(int fontId, const std::string& text, int x, int y, int width, int maxLines, bool bold = false) const;
  // `from` is the first sense to draw; `shown`, when given, receives how many were drawn.
  int drawSenses(const std::string& gloss, int x, int y, int width, int bottom, bool& cut, int from = 0, int* shown = nullptr) const;
  int entryFrom = 0, entryShown = 0;  // the "in full" screen: its first sense, and how many it holds
  bool entryCut = false;
  void drawToday() const;
  void drawCard(bool back) const;
  void drawWordLike(bool back, int top, int bottom) const;
  void drawKanji(bool back, int top, int bottom) const;
  bool drawKanjiAnswer(int y, int bottom, int maxLines) const;
  void drawContrast(bool back, int top, int bottom) const;
  void drawDone() const;
  void drawSources() const;
  void drawDateCheck() const;
  std::string sourceTitle(int id) const;
  std::string clock(bool withTime) const;
  bool definitionCut = false;
};
