#include "UpdateActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "WaddleDraw.h"
#include "WaddleVersion.h"
#include "WaddleWifi.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

// arnee.me passes on the waddle-latest release, so the reader only talks to arnee.me.
constexpr const char* RELEASE = "https://arnee.me/api/fastvare/";
constexpr const char* FIRMWARE_PATH = "/waddleos-x4pro.bin";
constexpr unsigned long WIFI_TIMEOUT_MS = 15000;

bool lowHeap() {
  return ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
         ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC;
}

void drawButton(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const char* label) {
  renderer.fillRoundedRect(x, y, w, h, 12, Color::Black);
  const int textW = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = y + (h - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, x + (w - textW) / 2, textY, label, false, EpdFontFamily::BOLD);
}

}  // namespace

UpdateActivity::UpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Update", renderer, mappedInput) {}

void UpdateActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void UpdateActivity::onExit() {
  waddle::wifi::off();
  Activity::onExit();
}

void UpdateActivity::startWifi(const Action action) {
  pending = action;
  connectStartedAt = millis();
  if (WiFi.status() != WL_CONNECTED && !waddle::wifi::joinSaved(*this)) fail("Fant ikke Wi-Fi");
}

void UpdateActivity::fail(const char* message) {
  waddle::wifi::off();
  pending = Action::NONE;
  {
    RenderLock lock(*this);
    error = message;
    state = State::FAILED;
  }
  requestUpdate();
}

// version.json: {"versjon": "WaddleOS 1.42", "build": "abc1234", "dato": "26. okt kl 22:22", "melding": "..."}
void UpdateActivity::check() {
  if (lowHeap()) return fail("For lite minne");
  std::string body;
  const bool ok = HttpDownloader::fetchUrl(std::string(RELEASE) + "version.json",
                                           [&body](const uint8_t* data, const size_t len) {
                                             if (body.size() + len > 2048) return false;
                                             body.append(reinterpret_cast<const char*>(data), len);
                                             return true;
                                           });
  waddle::wifi::off();
  JsonDocument doc;
  if (!ok || deserializeJson(doc, body) || doc["build"].isNull()) return fail("Fant ingen oppdatering");
  {
    RenderLock lock(*this);
    latestBuild = doc["build"] | "";
    latestVersion = doc["versjon"] | "";
    latestDate = doc["dato"] | "";
    latestNote = doc["melding"] | "";
    state = latestBuild == waddle::build() ? State::LATEST : State::AVAILABLE;
  }
  requestUpdate();
}

void UpdateActivity::download() {
  if (lowHeap()) return fail("For lite minne");
  int shown = -1;
  const auto result = HttpDownloader::downloadToFile(
      std::string(RELEASE) + "waddleos-x4pro.bin", FIRMWARE_PATH, [this, &shown](const size_t done, const size_t total) {
        if (total == 0) return;
        const int now = static_cast<int>(done * 100 / total);
        if (now / 5 == shown / 5) return;  // redraw every 5%
        shown = now;
        percent = now;
        requestUpdate();
      });
  waddle::wifi::off();
  if (result != HttpDownloader::OK) return fail("Nedlastingen feilet");
  // The normal update screen checks the file (chip, board, checksum) and asks first.
  startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput, false, FIRMWARE_PATH),
                         [this](const ActivityResult&) {
                           state = State::IDLE;
                           requestUpdate();
                         });
}

void UpdateActivity::press() {
  switch (state) {
    case State::IDLE:
    case State::LATEST:
    case State::FAILED:
      state = State::CHECKING;
      requestUpdate();
      startWifi(Action::CHECK);
      return;
    case State::AVAILABLE:
      percent = 0;
      state = State::DOWNLOADING;
      requestUpdate();
      startWifi(Action::DOWNLOAD);
      return;
    default:
      return;
  }
}

void UpdateActivity::loop() {
  using Button = MappedInputManager::Button;

  if (pending != Action::NONE) {
    if (WiFi.status() == WL_CONNECTED) {
      const Action action = pending;
      pending = Action::NONE;
      action == Action::CHECK ? check() : download();
    } else if (millis() - connectStartedAt > WIFI_TIMEOUT_MS) {
      fail("Fant ikke Wi-Fi");
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    finish();
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasReleased(Button::Confirm) ||
      (mappedInput.wasScreenTapped(x, y) && x >= buttonX && x < buttonX + buttonW && y >= buttonY &&
       y < buttonY + buttonH)) {
    press();
  }
}

void UpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Oppdater fastvare");
  waddle::drawPenguin(renderer, (pageWidth - 90) / 2, metrics.topPadding + metrics.headerHeight + 30, 3);

  int y = metrics.topPadding + metrics.headerHeight + 150;
  char line[96];
  snprintf(line, sizeof(line), "Installert: %s", waddle::version());
  renderer.drawCenteredText(SMALL_FONT_ID, y, line);
  y += smallH + 24;

  const char* title = "";
  const char* buttonLabel = nullptr;
  switch (state) {
    case State::IDLE:
      buttonLabel = "Se etter oppdatering";
      break;
    case State::CHECKING:
      title = "Ser etter oppdatering...";
      break;
    case State::LATEST:
      title = "Du har nyeste versjon";
      buttonLabel = "Se etter på nytt";
      break;
    case State::AVAILABLE:
      title = "Ny versjon klar";
      buttonLabel = "Oppdater";
      break;
    case State::DOWNLOADING:
      title = "Laster ned...";
      break;
    case State::FAILED:
      title = error.c_str();
      buttonLabel = "Prøv igjen";
      break;
  }
  if (title[0]) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, title, true, EpdFontFamily::BOLD);
    y += lineH + 8;
  }

  if (state == State::AVAILABLE || state == State::LATEST) {
    snprintf(line, sizeof(line), "Nyeste: %s, %s", latestVersion.c_str(), latestDate.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, y, line);
    y += smallH + 4;
    const auto lines = renderer.wrappedText(SMALL_FONT_ID, latestNote.c_str(), pageWidth - 60, 2);
    for (const auto& l : lines) {
      renderer.drawCenteredText(SMALL_FONT_ID, y, l.c_str());
      y += smallH;
    }
  }

  if (state == State::DOWNLOADING) {
    const int barW = pageWidth - 80;
    renderer.drawRoundedRect(40, y + 8, barW, 20, 2, 10, true);
    const int filled = (barW - 8) * percent / 100;
    if (filled > 0) renderer.fillRect(44, y + 12, filled, 12);
    snprintf(line, sizeof(line), "%d %%", percent);
    renderer.drawCenteredText(SMALL_FONT_ID, y + 36, line);
  }

  buttonW = buttonH = 0;
  if (buttonLabel) {
    buttonW = 300;
    buttonH = 64;
    buttonX = (pageWidth - buttonW) / 2;
    buttonY = pageHeight - buttonH - 60;
    drawButton(renderer, buttonX, buttonY, buttonW, buttonH, buttonLabel);
  }

  renderer.displayBuffer(state == State::DOWNLOADING ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
}
