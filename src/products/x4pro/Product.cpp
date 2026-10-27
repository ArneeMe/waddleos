#include "core/Product.h"

#include "WaddleHomeActivity.h"
#include "apps/news/NewsActivity.h"
#include "apps/news/NewsPaper.h"

// The e-reader: Hjem as home, Nyheter after the NRK paper, CrossPoint's deep sleep.
namespace product {

void beginBoard() {}
void begin() {}
void loop() {}
bool preventAutoSleep() { return false; }
bool sleep() { return false; }
void registerWebRoutes(WebServer&) {}
const char* firmwareName() { return "waddleos-x4pro.bin"; }

std::unique_ptr<Activity> makeHome(GfxRenderer& renderer, MappedInputManager& input, const bool cleanInitialRefresh) {
  return std::make_unique<WaddleHomeActivity>(renderer, input, cleanInitialRefresh);
}

std::unique_ptr<Activity> afterBook(GfxRenderer& renderer, MappedInputManager& input, const std::string& bookPath) {
  // Leaving the news paper goes back to the headline list.
  if (bookPath == waddle::news::EPUB_PATH) return std::make_unique<NewsActivity>(renderer, input, /*fetch=*/false);
  return nullptr;
}

}  // namespace product
