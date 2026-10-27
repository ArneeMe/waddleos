#include "core/Product.h"

#include <BoardT5S3.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include <algorithm>

#include "T5HomeActivity.h"
#include "apps/news/NewsActivity.h"
#include "apps/news/NewsPaper.h"
#include "apps/sos/SosAlertActivity.h"
#include "core/activities/ActivityManager.h"
#include "services/mesh/MeshNode.h"
#include "services/mesh/MeshRadio.h"

// The mesh unit: LilyGO T5 E-Paper S3 Pro, 868 MHz.
namespace product {

void beginBoard() { BoardT5S3::begin(); }

void begin() { mesh::Node::get().begin(); }

namespace {
// Holding the user button (beside the screen) this long starts an SOS from anywhere.
constexpr unsigned long SOS_HOLD_MS = 3000;
unsigned long userButtonSince = 0;
bool sosFromButton = false;
}  // namespace

void loop() {
  auto& node = mesh::Node::get();
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

// Light sleep with the radio listening: messages still arrive, are stored,
// acknowledged and relayed, and positions go out on time. The power button or
// the user button wakes the screen; so does an SOS from a member.
bool sleep() {
  auto& node = mesh::Node::get();
  if (!node.configured()) return false;  // no group: sleep deeply, like the reader

  const gpio_num_t radioIrq = gpio_num_t(mesh::radio::irqPin());
  const gpio_num_t powerButton = gpio_num_t(T5S3_BOOT_BTN);
  const gpio_num_t expanderIrq = gpio_num_t(T5S3_PCA9535_INT);
  gpio_wakeup_enable(radioIrq, GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable(powerButton, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(expanderIrq, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  LOG_INF("SLP", "Light sleep, radio listening");

  while (true) {
    const uint32_t ms = std::max<uint32_t>(node.msUntilWork(), 1000);
    esp_sleep_enable_timer_wakeup(uint64_t(ms) * 1000ULL);
    esp_light_sleep_start();
    if (digitalRead(T5S3_BOOT_BTN) == LOW || BoardT5S3::readButton()) break;
    mesh::radio::rearm();
    // Handle what woke us, and finish sending before sleeping again.
    do {
      node.loop();
      delay(5);
    } while (node.busy());
    if (node.hasNewSos()) break;
  }

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  gpio_wakeup_disable(radioIrq);
  gpio_wakeup_disable(powerButton);
  gpio_wakeup_disable(expanderIrq);
  mesh::radio::rearm();
  node.requestSync();
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
