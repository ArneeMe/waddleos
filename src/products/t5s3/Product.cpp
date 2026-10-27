#include "core/Product.h"

#include <BoardT5S3.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <esp_sleep.h>

#include <algorithm>

#include "T5HomeActivity.h"
#include "apps/news/NewsActivity.h"
#include "apps/news/NewsPaper.h"
#include "apps/sos/SosAlertActivity.h"
#include "core/activities/ActivityManager.h"
#include "core/activities/RenderLock.h"
#include "core/fontIds.h"
#include "services/mesh/MeshNode.h"
#include "services/mesh/MeshRadio.h"
#include "services/mesh/MeshWebApi.h"

// The mesh unit: LilyGO T5 E-Paper S3 Pro, 868 MHz.
namespace product {

void beginBoard() { BoardT5S3::begin(); }

void begin() { mesh::Node::get().begin(); }

void registerWebRoutes(WebServer& server) { mesh::web::registerRoutes(server); }
const char* firmwareName() { return "waddleos-t5.bin"; }

namespace {
// Holding the user button (beside the screen) this long starts an SOS from anywhere.
constexpr unsigned long SOS_HOLD_MS = 3000;
unsigned long userButtonSince = 0;
bool sosFromButton = false;
}  // namespace

void loop() {
  auto& node = mesh::Node::get();
  // A new group from `waddle mesh setup`: load it without a restart.
  if (mesh::web::takeGroupChanged()) node.begin();
  node.loop();

  if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
    if (!userButtonSince) userButtonSince = millis();
    if (!sosFromButton && !node.sosActive() && node.configured() && millis() - userButtonSince >= SOS_HOLD_MS) {
      sosFromButton = true;
      activityManager.replaceActivity(
          std::make_unique<SosActivity>(activityManager.getRenderer(), activityManager.getMappedInput()));
    }
  } else {
    userButtonSince = 0;
    sosFromButton = false;
  }

  // An SOS from a member takes over the screen, whatever is open.
  if (const uint8_t from = node.takeNewSos()) {
    activityManager.pushActivity(std::make_unique<SosAlertActivity>(activityManager.getRenderer(),
                                                                    activityManager.getMappedInput(), from));
  }
}

bool preventAutoSleep() { return mesh::Node::get().busy(); }

namespace {

// A line at the bottom of the sleep screen: "2 nye meldinger · sist fra Kari 14:32".
void drawUnreadOnSleepScreen(const uint32_t unread) {
  auto& node = mesh::Node::get();
  GfxRenderer& renderer = activityManager.getRenderer();
  const int width = renderer.getScreenWidth();
  const int y = renderer.getScreenHeight() - 96;
  uint8_t from = 0;
  uint32_t newest = 0;
  for (uint8_t c = 0; c < mesh::CONVERSATIONS; c++) {
    const auto& sum = node.store().summary(c);
    if (sum.unread && sum.lastTime >= newest) {
      newest = sum.lastTime;
      from = sum.lastSender;
    }
  }
  char line[64];
  snprintf(line, sizeof(line), "%lu %s · sist fra %s", static_cast<unsigned long>(unread),
           unread == 1 ? "ny melding" : "nye meldinger", node.group().name(from));
  RenderLock lock;
  renderer.fillRect(0, y, width, 96, false);
  renderer.fillRoundedRect(20, y + 16, width - 40, 64, 14, Color::Black);
  const int w = renderer.getTextWidth(UI_12_FONT_ID, line, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, (width - w) / 2, y + 34, line, false, EpdFontFamily::BOLD);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

}  // namespace

// Light sleep. With "Lytt hele tiden" the radio listens: messages arrive, are
// stored, acknowledged and relayed as they come. Otherwise the radio sleeps too
// and wakes for each check (MeshSettings), for positions on a trip and for
// retries of unanswered messages. The power button or the user button wakes the
// screen; so does an SOS from a member.
bool sleep() {
  auto& node = mesh::Node::get();
  if (!node.configured()) return false;  // no group: sleep deeply, like the reader

  const gpio_num_t radioIrq = gpio_num_t(mesh::radio::irqPin());
  const gpio_num_t powerButton = gpio_num_t(T5S3_BOOT_BTN);
  const gpio_num_t expanderIrq = gpio_num_t(T5S3_PCA9535_INT);
  gpio_wakeup_enable(powerButton, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(expanderIrq, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  LOG_INF("SLP", "Light sleep, %s", node.listensAlways() ? "radio listening" : "radio asleep between checks");
  uint32_t shownUnread = node.store().totalUnread();

  while (true) {
    uint32_t ms = node.msUntilWork();
    if (node.listensAlways()) {
      gpio_wakeup_enable(radioIrq, GPIO_INTR_HIGH_LEVEL);
    } else {
      // A sleeping radio may leave its interrupt pin high: do not wake on it.
      gpio_wakeup_disable(radioIrq);
      mesh::radio::pause();
      ms = std::min(ms, node.msUntilCheck());
    }
    esp_sleep_enable_timer_wakeup(uint64_t(std::max<uint32_t>(ms, 1000)) * 1000ULL);
    esp_light_sleep_start();
    if (digitalRead(T5S3_BOOT_BTN) == LOW || BoardT5S3::readButton()) break;
    // node.loop() starts a check that is due; sending wakes the radio by itself.
    mesh::radio::rearm();
    // Handle what woke us, and finish sending before sleeping again.
    do {
      node.loop();
      delay(5);
    } while (node.busy());
    if (node.hasNewSos()) break;
    // New messages show on the sleep screen without waking the unit.
    const uint32_t unread = node.store().totalUnread();
    if (unread > shownUnread) drawUnreadOnSleepScreen(unread);
    shownUnread = unread;
  }

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  gpio_wakeup_disable(radioIrq);
  gpio_wakeup_disable(powerButton);
  gpio_wakeup_disable(expanderIrq);
  mesh::radio::resume();
  mesh::radio::rearm();
  // Woken by hand: fetch what came in while asleep.
  node.startCheck();
  LOG_INF("SLP", "Awake");
  return true;
}

std::unique_ptr<Activity> makeHome(GfxRenderer& renderer, MappedInputManager& input, const bool cleanInitialRefresh) {
  return std::make_unique<T5HomeActivity>(renderer, input, cleanInitialRefresh);
}

std::unique_ptr<Activity> afterBook(GfxRenderer& renderer, MappedInputManager& input, const std::string& bookPath) {
  if (bookPath == waddle::news::EPUB_PATH) return std::make_unique<NewsActivity>(renderer, input, /*fetch=*/false);
  return nullptr;
}

}  // namespace product
