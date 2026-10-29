#include "SosAlertActivity.h"

#include <GfxRenderer.h>

#include <vector>

#include "apps/messages/MeshUi.h"
#include "core/MappedInputManager.h"
#include "core/fontIds.h"
#include "core/waddle/WaddleDraw.h"
#include "services/mesh/MeshGps.h"
#include "services/mesh/MeshNode.h"

namespace {

mesh::Node& node() { return mesh::Node::get(); }

void whiteButton(const GfxRenderer& renderer, const int y, const char* label) {
  const int width = renderer.getScreenWidth();
  renderer.fillRoundedRect(80, y, width - 160, 90, 18, Color::White);
  const int w = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, (width - w) / 2, y + 32, label, true, EpdFontFamily::BOLD);
}

}  // namespace

// --- My SOS ---------------------------------------------------------------------

SosActivity::SosActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Sos", renderer, mappedInput) {}

void SosActivity::onEnter() {
  Activity::onEnter();
  node().startSos();
  requestUpdate();
}

void SosActivity::loop() {
  using Button = MappedInputManager::Button;
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y) && y >= cancelY && y < cancelY + 90) {
    node().cancelSos();
    activityManager.goHome();
    return;
  }
  // Back leaves the screen; the SOS keeps going (Hjem shows it).
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.goHome();
    return;
  }
  // Answers coming in, or once a minute for the position line.
  if (node().changes() != shownChanges || millis() - drawnAt > 60000) requestUpdate();
}

void SosActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  drawnAt = millis();
  shownChanges = node().changes();
  renderer.clearScreen();
  renderer.fillRect(0, 0, width, height, true);
  waddle::drawPixelText(renderer, (width - waddle::pixelTextWidth("SOS", 20)) / 2, 160, "SOS", 20, false);
  // Who has it: "Mottatt av Kari og serveren", then "Sett av Kari". With many,
  // three names and how many more.
  auto names = [](const mesh::Members bits, const bool server) {
    std::vector<std::string> who;
    for (uint8_t m = 1; m <= mesh::MAX_MEMBERS; m++) {
      if (bits & mesh::memberBit(m)) who.push_back(node().group().name(m));
    }
    const size_t total = who.size() + (server ? 1 : 0);
    if (who.size() > 3) {
      const size_t more = who.size() - 3;
      who.resize(3);
      who.push_back(std::to_string(more) + " til");
    }
    if (server) who.push_back("serveren");
    std::string out;
    for (size_t i = 0; i < who.size(); i++) out += (i == 0 ? "" : i + 1 == who.size() ? " og " : ", ") + who[i];
    return total ? out : std::string();
  };
  const mesh::Members received = node().sosReceivedBy();
  const mesh::Members seen = node().sosSeenBy();
  const bool server = node().sosOnServer();
  const std::string got =
      received || server ? "Mottatt av " + names(received, server) : std::string("Ingen svar ennå");
  renderer.drawCenteredText(UI_12_FONT_ID, 330, got.c_str(), false, EpdFontFamily::BOLD);
  if (seen) renderer.drawCenteredText(UI_12_FONT_ID, 370, ("Sett av " + names(seen, false)).c_str(), false);
  renderer.drawCenteredText(UI_12_FONT_ID, 410, "Sendes på nytt hvert minutt", false);
  const auto& fix = mesh::gps::lastFix();
  if (fix.valid) {
    mesh::Position p;
    p.latE7 = fix.latE7;
    p.lonE7 = fix.lonE7;
    renderer.drawCenteredText(UI_12_FONT_ID, 460, meshui::coordinatesText(p).c_str(), false, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, 460, "Leter etter GPS …", false);
  }
  cancelY = height - 220;
  whiteButton(renderer, cancelY, "Avbryt SOS");
  renderer.displayBuffer();
}

// --- Someone else's SOS -----------------------------------------------------------

SosAlertActivity::SosAlertActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const uint8_t from)
    : Activity("SosAlert", renderer, mappedInput), from(from) {}

void SosAlertActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void SosAlertActivity::loop() {
  using Button = MappedInputManager::Button;
  int x = 0, y = 0;
  if (mappedInput.wasReleased(Button::Confirm) || mappedInput.wasReleased(Button::Back) ||
      mappedInput.wasScreenTapped(x, y)) {
    // "Sett" tells the sender someone has seen it.
    node().acknowledgeSos(from);
    finish();
    return;
  }
  if (node().changes() != shownChanges) requestUpdate();  // a newer position, or cancelled
}

void SosAlertActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  shownChanges = node().changes();
  const auto& st = node().member(from);
  const char* name = node().group().name(from);
  renderer.clearScreen();
  renderer.fillRect(0, 0, width, height, true);
  waddle::drawPixelText(renderer, (width - waddle::pixelTextWidth("SOS", 16)) / 2, 120, "SOS", 16, false);
  char title[48];
  snprintf(title, sizeof(title), st.sos ? "fra %s" : "%s har avbrutt SOS", name);
  renderer.drawCenteredText(UI_12_FONT_ID, 270, title, false, EpdFontFamily::BOLD);

  const mesh::Position& p = st.sosPosition;
  int y = 340;
  if (p.noFix) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, "Ingen GPS-posisjon i SOS-en", false);
    y += 40;
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, y, meshui::coordinatesText(p).c_str(), false, EpdFontFamily::BOLD);
    y += 40;
    // Distance and direction from me, when I know where I am.
    const auto& fix = mesh::gps::lastFix();
    if (fix.valid) {
      mesh::Position me;
      me.latE7 = fix.latE7;
      me.lonE7 = fix.lonE7;
      const std::string where = meshui::distanceText(meshui::distanceM(me, p)) + " " +
                                meshui::directionText(meshui::bearingDeg(me, p)) + " for deg";
      renderer.drawCenteredText(UI_12_FONT_ID, y, where.c_str(), false);
      y += 40;
    }
  }
  char battery[32];
  snprintf(battery, sizeof(battery), "Batteri %u %%", p.battery);
  renderer.drawCenteredText(UI_12_FONT_ID, y, battery, false);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 40, meshui::agoText(p.time).c_str(), false);
  whiteButton(renderer, height - 220, "Sett");
  renderer.displayBuffer();
}
