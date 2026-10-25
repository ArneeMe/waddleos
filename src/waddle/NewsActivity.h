#pragma once

#include <string>
#include <vector>

#include "NewsPaper.h"
#include "activities/Activity.h"

// WaddleOS: "Nyheter". Fetches the latest NRK paper from arnee.me (built every
// hour, see ArneeMe/arneeme nyheter/), then turns Wi-Fi off and lists the
// headlines. Opening a headline starts the normal reader at that article.
// Without Wi-Fi it shows the last paper it fetched.
class NewsActivity final : public Activity {
 public:
  // fetch=false shows the saved paper at once (used when coming back from the reader).
  NewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fetch = true);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::CONNECTING; }

 private:
  enum class State { CONNECTING, LIST };

  State state = State::CONNECTING;
  bool fetchOnEnter;
  bool stale = false;  // the fetch failed, so the list is an older paper
  unsigned long connectStartedAt = 0;

  waddle::news::Paper paper;
  std::vector<std::string> readIds;
  // What the list shows, row by row: an index into paper.stories, or READ_HEADING.
  static constexpr int READ_HEADING = -1;
  std::vector<int> rows;
  int unread = 0;
  int selected = 0;  // index into rows
  int rowsPerPage = 1;  // set by render()
  int listTop = 0;
  int rowHeight = 1;
  int footerTop = 0;

  void startFetch();
  void finishFetch(bool ok);
  void loadList();
  void toggleRead(int row);
  bool isRead(const std::string& id) const { return waddle::news::isRead(readIds, id); }
  void buildRows();
  void open(int row);
  void moveTo(int row, int direction = 1);
};
