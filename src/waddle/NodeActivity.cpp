#include "NodeActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "WaddleDraw.h"
#include "WaddlePrivacy.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

#ifndef WADDLE_NODE_IP
#define WADDLE_NODE_IP "192.168.50.239"
#endif

namespace {

constexpr unsigned long WIFI_TIMEOUT_MS = 15000;
constexpr unsigned long POLL_MS = 5000;
constexpr int VOLUME_STEP_DB = 2;
// Notes move one step this often while playing: slow enough for e-ink.
constexpr unsigned long NOTE_MS = 6000;
constexpr size_t MAX_PRESETS = 4;
// Optional override: a file with just the Node's IP address.
constexpr const char* NODE_IP_FILE = "/.waddle/node-ip.txt";

// Short button names for the Node's inputs, matched on the preset URL so a
// renamed preset in the BluOS app still gets the short name.
const char* shortName(const std::string& url) {
  if (url.rfind("Capture:hw:imxspdif", 0) == 0) return "TV";
  if (url.rfind("Capture:plughw:soundadc", 0) == 0) return "LP";
  return nullptr;
}

std::string nodeHost() {
  if (Storage.exists(NODE_IP_FILE)) {
    String ip = Storage.readFile(NODE_IP_FILE);
    ip.trim();
    if (ip.length() > 0) return ip.c_str();
  }
  return WADDLE_NODE_IP;
}

}  // namespace

NodeActivity::NodeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Node", renderer, mappedInput), node(nodeHost()) {}

void NodeActivity::onEnter() {
  Activity::onEnter();
  state = State::CONNECTING;
  requestUpdate();
  startWifi();
}

void NodeActivity::onExit() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Activity::onExit();
}

void NodeActivity::startWifi() {
  connectStartedAt = millis();
  if (WiFi.status() == WL_CONNECTED) return;
  // The store is only loaded by the Wi-Fi screens, so load it here too.
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }
  auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred && WIFI_STORE.getCredentialCount() > 0) cred = WIFI_STORE.getCredentialAt(0);
  if (!cred) {
    LOG_ERR("NODE", "no saved Wi-Fi network");
    state = State::NO_WIFI;
    requestUpdate();
    return;
  }
  // Same sequence as WifiSelectionActivity::attemptConnection().
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
#if WADDLE_PRIVATE
  waddle::privacy::prepareStation(cred->ssid.c_str());
#endif
  LOG_INF("NODE", "joining %s", cred->ssid.c_str());
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
}

void NodeActivity::loadNode() {
  if (!node.presets(presets) || !node.status(status)) {
    state = State::NO_NODE;
  } else {
    if (presets.size() > MAX_PRESETS) presets.resize(MAX_PRESETS);
    for (auto& preset : presets) {
      if (const char* name = shortName(preset.url)) preset.name = name;
    }
    state = State::READY;
  }
  lastPollAt = millis();
  requestUpdate();
}

void NodeActivity::refreshStatus(const bool force) {
  if (!force && millis() - lastPollAt < POLL_MS) return;
  lastPollAt = millis();
  waddle::bluos::Status fresh;
  if (!node.status(fresh)) return;
  const bool changed = fresh.volume != status.volume || fresh.playing != status.playing ||
                       fresh.title != status.title || fresh.streamUrl != status.streamUrl;
  status = std::move(fresh);
  if (changed || force) requestUpdate();
}

int NodeActivity::activePreset() const {
  for (size_t i = 0; i < presets.size(); i++) {
    const auto& url = presets[i].url;
    if (url == status.streamUrl) return static_cast<int>(i);
    if (status.service == "Spotify" && url.rfind("Spotify", 0) == 0) return static_cast<int>(i);
  }
  return -1;
}

void NodeActivity::selectPreset(const int index) {
  if (index < 0 || index >= static_cast<int>(presets.size())) return;
  node.playPreset(presets[index].id);
  refreshStatus(true);
}

void NodeActivity::changeVolume(const int db) {
  node.volumeStep(db);
  refreshStatus(true);
}

void NodeActivity::togglePlay() {
  status.playing ? node.pause() : node.play();
  refreshStatus(true);
}

void NodeActivity::skip(const bool forward) {
  forward ? node.next() : node.previous();
  refreshStatus(true);
}

void NodeActivity::goHome() { activityManager.goHome(); }

void NodeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
    goHome();
    return;
  }

  if (state == State::CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      loadNode();
    } else if (millis() - connectStartedAt > WIFI_TIMEOUT_MS) {
      state = State::NO_WIFI;
      requestUpdate();
    }
    return;
  }

  if (state != State::READY) {
    // Confirm or a tap retries.
    int x = 0, y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      state = State::CONNECTING;
      requestUpdate();
      startWifi();
    }
    return;
  }

  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::PageBack)) {
    changeVolume(-VOLUME_STEP_DB);
  } else if (mappedInput.wasReleased(Button::Right) || mappedInput.wasReleased(Button::PageForward)) {
    changeVolume(VOLUME_STEP_DB);
  } else if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::Down)) {
    const int count = static_cast<int>(presets.size());
    if (count > 0) {
      const int step = mappedInput.wasReleased(Button::Down) ? 1 : count - 1;
      selectPreset((std::max(activePreset(), 0) + step) % count);
    }
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    togglePlay();
  } else {
    int x = 0, y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      for (size_t i = 0; i < presetBoxes.size(); i++) {
        if (presetBoxes[i].contains(x, y)) {
          selectPreset(static_cast<int>(i));
          return;
        }
      }
      if (volDownBox.contains(x, y)) changeVolume(-VOLUME_STEP_DB);
      else if (volUpBox.contains(x, y)) changeVolume(VOLUME_STEP_DB);
      else if (prevBox.contains(x, y)) skip(false);
      else if (playBox.contains(x, y)) togglePlay();
      else if (nextBox.contains(x, y)) skip(true);
    }
  }

  refreshStatus(false);

  if (status.playing && millis() - lastNoteAt > NOTE_MS) {
    lastNoteAt = millis();
    noteFrame++;
    requestUpdate();
  }
}

namespace {

void drawButton(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const char* label,
                const bool selected) {
  if (selected) {
    renderer.fillRoundedRect(x, y, w, h, 10, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, h, 2, 10, true);
  }
  // Shorten with "..." until the label fits inside the button.
  std::string text = label;
  const int maxW = w - 16;
  while (text.size() > 1 && renderer.getTextWidth(UI_12_FONT_ID, text.c_str(), EpdFontFamily::BOLD) > maxW) {
    text.erase(text.size() - (text.size() > 3 && text.compare(text.size() - 3, 3, "...") == 0 ? 4 : 1));
    // Do not leave half of a UTF-8 character (æ, ø, å).
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
    if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xC0) text.pop_back();
    if (text.size() < 3 || text.compare(text.size() - 3, 3, "...") != 0) text += "...";
  }
  const int textW = renderer.getTextWidth(UI_12_FONT_ID, text.c_str(), EpdFontFamily::BOLD);
  const int textY = y + (h - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, x + (w - textW) / 2, textY, text.c_str(), !selected, EpdFontFamily::BOLD);
}

}  // namespace

void NodeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int side = 24;
  const int contentW = pageWidth - 2 * side;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Node");
  int y = metrics.topPadding + metrics.headerHeight + 24;

  if (state != State::READY) {
    const char* line1 = state == State::CONNECTING ? "Kobler til..." : state == State::NO_WIFI ? "Fant ikke Wi-Fi"
                                                                                                : "Fant ikke Node";
    const char* line2 = state == State::CONNECTING ? "" : "Trykk for å prøve igjen";
    waddle::drawPenguin(renderer, (pageWidth - 90) / 2, pageHeight / 2 - 140, 3);
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, line1, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 36, line2);
    renderer.displayBuffer();
    return;
  }

  // Now playing
  renderer.drawText(UI_12_FONT_ID, side, y, status.title.empty() ? "-" : status.title.c_str(), true,
                    EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 4;
  if (!status.artist.empty()) renderer.drawText(SMALL_FONT_ID, side, y, status.artist.c_str());
  y += 40;

  // Sources
  presetBoxes.clear();
  const int active = activePreset();
  const int count = static_cast<int>(presets.size());
  const int gap = 12;
  const int buttonH = 72;
  if (count > 0) {
    const int buttonW = (contentW - gap * (count - 1)) / count;
    for (int i = 0; i < count; i++) {
      const int x = side + i * (buttonW + gap);
      drawButton(renderer, x, y, buttonW, buttonH, presets[i].name.c_str(), i == active);
      presetBoxes.push_back({x, y, buttonW, buttonH});
    }
  }
  y += buttonH + 48;

  // Volume
  char volumeText[16];
  snprintf(volumeText, sizeof(volumeText), "%d", status.volume < 0 ? 0 : status.volume);
  renderer.drawCenteredText(UI_12_FONT_ID, y, status.mute ? "Mute" : volumeText, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 16;
  const int roundW = 96;
  volDownBox = {side, y, roundW, roundW};
  volUpBox = {pageWidth - side - roundW, y, roundW, roundW};
  drawButton(renderer, volDownBox.x, volDownBox.y, roundW, roundW, "-", false);
  drawButton(renderer, volUpBox.x, volUpBox.y, roundW, roundW, "+", false);
  // Volume bar between the buttons
  const int barX = side + roundW + 20;
  const int barW = contentW - 2 * (roundW + 20);
  const int barY = y + roundW / 2 - 10;
  renderer.drawRoundedRect(barX, barY, barW, 20, 2, 10, true);
  const int filled = status.volume <= 0 ? 0 : (barW - 8) * status.volume / 100;
  if (filled > 0) renderer.fillRect(barX + 4, barY + 4, filled, 12);
  y += roundW + 48;

  // Playback
  const int playW = (contentW - 2 * gap) / 3;
  prevBox = {side, y, playW, buttonH};
  playBox = {side + playW + gap, y, playW, buttonH};
  nextBox = {side + 2 * (playW + gap), y, playW, buttonH};
  drawButton(renderer, prevBox.x, prevBox.y, playW, buttonH, "<<", false);
  drawButton(renderer, playBox.x, playBox.y, playW, buttonH, status.playing ? "||" : ">", false);
  drawButton(renderer, nextBox.x, nextBox.y, playW, buttonH, ">>", false);

  // The penguin sings while music plays: two notes float up from its beak.
  constexpr int penguinScale = 3;
  const int penguinX = pageWidth - side - 30 * penguinScale;
  const int penguinY = pageHeight - 100;
  waddle::drawPenguin(renderer, penguinX, penguinY, penguinScale, true);
  if (status.playing) {
    constexpr int path[][2] = {{-62, -30}, {-84, -58}, {-66, -92}, {-96, -122}};
    const int beakX = penguinX + 15 * penguinScale;
    const int beakY = penguinY + 14 * penguinScale;
    for (int k = 0; k < 2; k++) {
      const auto& step = path[(noteFrame + 2 * k) % 4];
      waddle::drawNote(renderer, beakX + step[0], beakY + step[1], 2, (noteFrame + k) % 2);
    }
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
