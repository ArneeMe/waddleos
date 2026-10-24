#pragma once

#include <string>
#include <vector>

#include "NewsPaper.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"

// WaddleOS home screen ("Hjem"): clock, date and today's weather, the book
// being read, the top unread NRK headlines and five buttons. Shows the saved
// weather and paper at once; when they are older than 30 minutes it joins
// Wi-Fi, fetches new ones from arnee.me, redraws once and turns Wi-Fi off.
class WaddleHomeActivity final : public Activity {
 public:
  WaddleHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool cleanInitialRefresh = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
  bool preventAutoSleep() override { return fetching; }

  struct Weather {
    bool valid = false;
    std::string place;
    std::string icon;  // sol, lettsky, sky, regn, sno, torden, taake
    std::string text;  // "Regn fra kl 15"
    int temp = 0, min = 0, max = 0;
  };

 private:
  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  // Things that can be selected, in Up/Down order.
  enum Target {
    T_BOOK,
    T_STORY_1,
    T_STORY_2,
    T_STORY_3,
    T_NEWS_LIST,
    T_LIBRARY,
    T_FILES,
    T_TRANSFER,
    T_BLUESOUND,
    T_SETTINGS,
    T_COUNT
  };
  static constexpr int TOP_STORIES = 3;

  bool cleanInitialRefresh;
  bool firstRender = true;
  int renders = 0;
  int shownMinute = -1;

  bool hasBook = false;
  RecentBook book;
  int bookProgress = -1;
  bool coverTried = false;

  Weather weather;
  long fetchedAt = -1;  // local minutes since 1970 of the last fetch, -1 = never
  bool fetching = false;
  unsigned long enteredAt = 0;
  unsigned long connectStartedAt = 0;

  waddle::news::Paper paper;
  std::vector<std::string> readIds;
  std::vector<int> topStories;  // indices into paper.stories
  int unread = 0;

  int focus = T_BOOK;
  Box boxes[T_COUNT];  // filled by render() for touch input

  void loadData();
  bool isStale() const;
  void startFetch();
  void finishFetch();
  void ensureCover();
  bool available(int target) const;
  void moveFocus(int step);
  void activate(int target);
};
