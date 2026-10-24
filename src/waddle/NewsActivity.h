#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

namespace waddle::news {
// The latest NRK paper. Leaving the reader from this book returns to the list.
constexpr const char* EPUB_PATH = "/Nyheter/NRK.epub";
}  // namespace waddle::news

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

  struct Item {
    std::string id;
    std::string title;
    std::string time;
  };

  State state = State::CONNECTING;
  bool fetchOnEnter;
  bool stale = false;  // the fetch failed, so the list is an older paper
  unsigned long connectStartedAt = 0;

  std::string edition;  // "NRK lør. 24. okt. kl 08"
  std::string built;    // build time, to skip downloading the same paper twice
  std::vector<Item> items;
  std::vector<std::string> readIds;
  int selected = 0;
  int rowsPerPage = 1;  // set by render()
  int listTop = 0;
  int rowHeight = 1;
  int footerTop = 0;

  void startFetch();
  bool download();
  void finishFetch(bool ok);
  bool loadList();
  void loadRead();
  bool isRead(const std::string& id) const;
  void open(int index);
  void moveTo(int index);
};
