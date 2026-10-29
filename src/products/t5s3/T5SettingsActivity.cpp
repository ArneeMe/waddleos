#include "T5SettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <string>

#include "apps/messages/MeshUi.h"
#include "core/CrossPointSettings.h"
#include "core/MappedInputManager.h"
#include "core/fontIds.h"
#include "core/waddle/WaddleVersion.h"
#include "services/mesh/MeshNode.h"
#include "services/mesh/MeshSettings.h"

namespace {

constexpr int SIDE = 20;
constexpr int ROW_H = 68;
constexpr uint8_t POSITION_MINUTES[] = {5, 10, 15, 30, 60};
constexpr uint8_t SLEEP_MINUTES[] = {1, 2, 5, 10, 15, 30, CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES};

const char* checkText(const uint8_t check) {
  static const char* names[] = {"lytter hele tiden", "hvert kvarter", "hver time",
                                "hver 6. time",      "en gang i døgnet", "bare når jeg vekker den"};
  return check < 6 ? names[check] : "?";
}

template <size_t N>
uint8_t nextOf(const uint8_t (&values)[N], const uint8_t now) {
  for (size_t i = 0; i < N; i++) {
    if (values[i] > now) return values[i];
  }
  return values[0];
}

}  // namespace

T5SettingsActivity::T5SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("T5Settings", renderer, mappedInput) {}

void T5SettingsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void T5SettingsActivity::activate(const int row) {
  auto& m = MESH_SETTINGS;
  switch (row) {
    case MODE:
      m.onTrip = m.onTrip ? 0 : 1;
      mesh::Node::get().startCheck();
      break;
    case HOME_CHECK:
      m.homeCheck = uint8_t((m.homeCheck + 1) % 6);
      break;
    case HOME_WIFI:
      m.homeWifi = m.homeWifi ? 0 : 1;
      break;
    case TRIP_CHECK:
      m.tripCheck = uint8_t((m.tripCheck + 1) % 4);  // "only when woken" makes no sense on a trip
      break;
    case POSITION:
      m.positionMinutes = nextOf(POSITION_MINUTES, m.positionMinutes);
      break;
    case SLEEP:
      SETTINGS.sleepTimeoutMinutes = nextOf(SLEEP_MINUTES, SETTINGS.sleepTimeoutMinutes);
      break;
    default:
      openT5Setting(static_cast<Row>(row));
      return;
  }
  SETTINGS.saveToFile();
  requestUpdate();
}

void T5SettingsActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasReleased(Button::Down)) {
    focus = (focus + 1) % ROWS;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Up)) {
    focus = (focus + ROWS - 1) % ROWS;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    activate(focus);
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    for (int r = 0; r < ROWS; r++) {
      if (y >= boxes[r].y && y < boxes[r].y + boxes[r].h) {
        focus = r;
        activate(r);
        return;
      }
    }
  }
}

void T5SettingsActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  const auto& m = MESH_SETTINGS;
  renderer.clearScreen();
  int y = meshui::drawTitle(renderer, "Innstillinger");

  char value[48];
  auto section = [&](const char* title) {
    renderer.drawText(SMALL_FONT_ID, SIDE, y + 8, title, true, EpdFontFamily::BOLD);
    y += 34;
  };
  // A row: what it is, and below it the value (or ">" when it opens a screen).
  auto row = [&](const Row r, const char* label, const char* now, const bool opens) {
    boxes[r] = {y, ROW_H};
    if (focus == r) renderer.fillRect(0, y, 6, ROW_H - 4, true);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 8, label, true, EpdFontFamily::BOLD);
    if (now && now[0]) renderer.drawText(UI_10_FONT_ID, SIDE, y + 36, now);
    if (opens) renderer.drawText(UI_12_FONT_ID, width - SIDE - 20, y + 18, ">", true, EpdFontFamily::BOLD);
    y += ROW_H;
    renderer.drawLine(SIDE, y - 4, width - SIDE, y - 4);
  };

  section("GJELDER ALT");
  if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
    snprintf(value, sizeof(value), "aldri");
  } else {
    snprintf(value, sizeof(value), "etter %u min uten trykk", static_cast<unsigned>(SETTINGS.sleepTimeoutMinutes));
  }
  row(SLEEP, "Skjermen sover", value, false);
  row(WIFI, "Wi-Fi", "nettverk hjemme", true);
  row(CLOCK, "Klokke og tidssone", "", true);
  row(UPDATE, "Oppdater fastvare", waddle::version(), true);

  section("BØKER");
  row(READING, "Lesing og bibliotek", "skrift, sidevending, bibliotek", true);

  section("SPESIELT FOR T5");
  row(MODE, "Modus", m.onTrip ? "På tur: GPS på, deler posisjon" : "Hjemme: sjelden sjekk, sparer batteri", false);
  row(HOME_CHECK, "Sjekker hjemme", checkText(m.homeCheck), false);
  row(HOME_WIFI, "Sjekk også over Wi-Fi hjemme", m.homeWifi ? "På" : "Av", false);
  row(TRIP_CHECK, "Sjekker på tur", checkText(m.tripCheck), false);
  snprintf(value, sizeof(value), "hvert %u. minutt", static_cast<unsigned>(m.positionMinutes));
  row(POSITION, "Deler posisjon på tur", value, false);
  renderer.displayBuffer();
}
