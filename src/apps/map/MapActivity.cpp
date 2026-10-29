#include "MapActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "apps/messages/MeshUi.h"
#include "core/CrossPointSettings.h"
#include "core/MappedInputManager.h"
#include "core/fontIds.h"
#include "services/mesh/MeshGps.h"
#include "services/mesh/MeshNode.h"
#include "services/mesh/MeshSettings.h"

namespace {

constexpr int SIDE = 20;
// Ring scales to pick from, in metres.
constexpr double SCALES[] = {200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000};

mesh::Node& node() { return mesh::Node::get(); }

}  // namespace

MapActivity::MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Map", renderer, mappedInput) {}

void MapActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void MapActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.goHome();
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasReleased(Button::Confirm) || (mappedInput.wasScreenTapped(x, y) && y >= toggleY)) {
    MESH_SETTINGS.onTrip = MESH_SETTINGS.onTrip ? 0 : 1;
    SETTINGS.saveToFile();
    node().startCheck();
    requestUpdate();
    return;
  }
  // New positions, or once a minute for the ages.
  if (node().changes() != shownChanges || millis() - drawnAt > 60000) requestUpdate();
}

void MapActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  shownChanges = node().changes();
  drawnAt = millis();
  renderer.clearScreen();
  const int top = meshui::drawTitle(renderer, "Kart");
  const auto& group = node().network();

  // My position: the GPS now, or what I last shared.
  mesh::Position mine;
  bool haveMine = false;
  const auto& fix = mesh::gps::lastFix();
  if (fix.valid) {
    mine.latE7 = fix.latE7;
    mine.lonE7 = fix.lonE7;
    mine.altitude = fix.altitude;
    mine.time = fix.time;
    haveMine = true;
  } else if (node().member(group.me).hasPosition) {
    mine = node().member(group.me).position;
    haveMine = true;
  }

  const int cx = width / 2;
  const int cy = top + 250;
  const int r = 210;
  double farthest = 0;
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    const auto& st = node().member(n);
    if (n != group.me && st.hasPosition && haveMine) farthest = std::max(farthest, meshui::distanceM(mine, st.position));
  }
  double scale = SCALES[0];
  for (const double s : SCALES) {
    scale = s;
    if (farthest <= s) break;
  }

  for (int k = 1; k <= 3; k++) {
    const int rr = r * k / 3;
    renderer.drawArc(rr, cx, cy, 1, 1, 1, true);
    renderer.drawArc(rr, cx, cy, -1, 1, 1, true);
    renderer.drawArc(rr, cx, cy, 1, -1, 1, true);
    renderer.drawArc(rr, cx, cy, -1, -1, 1, true);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, cy - r - 34, "N", true, EpdFontFamily::BOLD);
  const int xs[] = {cx, cx - 11, cx + 11};
  const int ys[] = {cy - 14, cy + 10, cy + 10};
  renderer.fillPolygon(xs, ys, 3);
  const std::string scaleText = meshui::distanceText(scale);
  renderer.drawText(SMALL_FONT_ID, cx + r - renderer.getTextWidth(SMALL_FONT_ID, scaleText.c_str()), cy + r - 4,
                    scaleText.c_str());

  // Everyone on the compass; in the list the nearest first, as many as fit.
  std::vector<uint8_t> people;
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    if (n != group.me && group.member(n)->present) people.push_back(n);
  }
  auto distance = [&](const uint8_t n) {
    const auto& st = node().member(n);
    return st.hasPosition && haveMine ? meshui::distanceM(mine, st.position) : 1e12;
  };
  std::stable_sort(people.begin(), people.end(), [&](const uint8_t a, const uint8_t b) {
    return node().member(a).sos != node().member(b).sos ? node().member(a).sos : distance(a) < distance(b);
  });
  int listY = cy + r + 30;
  const int listEnd = height - 90 - 40;
  size_t shown = 0;
  for (const uint8_t n : people) {
    const auto& st = node().member(n);
    std::string line;
    if (!st.hasPosition) {
      line = "Ingen posisjon ennå";
    } else if (!haveMine) {
      line = "Venter på egen posisjon · " + meshui::agoText(st.position.time);
    } else {
      const double d = meshui::distanceM(mine, st.position);
      const double b = meshui::bearingDeg(mine, st.position);
      line = meshui::distanceText(d) + " " + meshui::directionText(b) + " · " + meshui::agoText(st.position.time);
      const double frac = std::min(1.0, d / scale);
      const int px = cx + int(r * frac * std::sin(b * M_PI / 180.0));
      const int py = cy - int(r * frac * std::cos(b * M_PI / 180.0));
      renderer.fillRoundedRect(px - 9, py - 9, 18, 18, 9, Color::Black);
      renderer.drawText(UI_12_FONT_ID, px + 14, py - 14, group.name(n), true, EpdFontFamily::BOLD);
    }
    if (st.sos) line = "SOS! " + line;
    if (listY + 72 + 30 > listEnd) continue;  // still on the compass, not in the list
    renderer.drawText(UI_12_FONT_ID, SIDE, listY, group.name(n), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, SIDE, listY + 32, line.c_str());
    listY += 72;
    shown++;
  }
  if (shown < people.size()) {
    const std::string more = "og " + std::to_string(people.size() - shown) + " til";
    renderer.drawText(UI_10_FONT_ID, SIDE, listY - 4, more.c_str());
    listY += 26;
  }

  std::string self = haveMine ? "Du: " + meshui::coordinatesText(mine) : std::string("Du: ingen posisjon");
  if (haveMine && mine.altitude) self += " · " + std::to_string(mine.altitude) + " moh";
  renderer.drawText(UI_10_FONT_ID, SIDE, listY + 4, self.c_str());

  toggleY = height - 90;
  const bool on = MESH_SETTINGS.onTrip;
  char label[48];
  snprintf(label, sizeof(label), on ? "På tur: deler posisjon hvert %u. min" : "Hjemme: deler ikke posisjon",
           static_cast<unsigned>(MESH_SETTINGS.positionMinutes));
  if (on) {
    renderer.fillRoundedRect(SIDE, toggleY + 10, width - 2 * SIDE, 64, 14, Color::Black);
  } else {
    renderer.drawRoundedRect(SIDE, toggleY + 10, width - 2 * SIDE, 64, 2, 14, true);
  }
  const int lw = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, (width - lw) / 2, toggleY + 28, label, !on, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}
