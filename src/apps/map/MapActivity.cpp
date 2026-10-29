#include "MapActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <cmath>
#include <vector>

#include "apps/messages/MeshUi.h"
#include "apps/messages/MessagesActivity.h"
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

// My position: the GPS now, or what I last shared.
bool myPosition(mesh::Position& mine) {
  const auto& fix = mesh::gps::lastFix();
  if (fix.valid) {
    mine.latE7 = fix.latE7;
    mine.lonE7 = fix.lonE7;
    mine.altitude = fix.altitude;
    mine.time = fix.time;
    return true;
  }
  const auto& self = node().member(node().network().me);
  if (!self.hasPosition) return false;
  mine = self.position;
  return true;
}

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
  const bool tapped = mappedInput.wasScreenTapped(x, y);
  if (tapped && y < toggleY) {
    for (const auto& [n, box] : taps) {
      if (box.contains(x, y)) {
        activityManager.replaceActivity(std::make_unique<PersonActivity>(renderer, mappedInput, n));
        return;
      }
    }
  }
  if (mappedInput.wasReleased(Button::Confirm) || (tapped && y >= toggleY)) {
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

  mesh::Position mine;
  const bool haveMine = myPosition(mine);
  taps.clear();

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
    // Each ring's distance, on its right side (the outer one below, as before).
    if (k < 3) {
      const std::string ring = meshui::distanceText(scale * k / 3);
      renderer.drawText(SMALL_FONT_ID, cx + rr + 4, cy - 10, ring.c_str());
    }
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
      taps.push_back({n, Box{px - 30, py - 30, 60 + renderer.getTextWidth(UI_12_FONT_ID, group.name(n)), 60}});
    }
    if (st.sos) line = "SOS! " + line;
    if (listY + 72 + 30 > listEnd) continue;  // still on the compass, not in the list
    renderer.drawText(UI_12_FONT_ID, SIDE, listY, group.name(n), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, SIDE, listY + 32, line.c_str());
    renderer.drawText(UI_12_FONT_ID, width - SIDE - 20, listY + 14, ">", true, EpdFontFamily::BOLD);
    taps.push_back({n, Box{0, listY - 8, width, 72}});
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

// --- One person ----------------------------------------------------------------

PersonActivity::PersonActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const uint8_t member)
    : Activity("Person", renderer, mappedInput), member(member) {}

void PersonActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PersonActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.replaceActivity(std::make_unique<MapActivity>(renderer, mappedInput));
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasReleased(Button::Confirm) || (mappedInput.wasScreenTapped(x, y) && y >= writeY)) {
    activityManager.replaceActivity(std::make_unique<ChatActivity>(renderer, mappedInput, member));
    return;
  }
  // A new position, or once a minute for the ages.
  if (node().changes() != shownChanges || millis() - drawnAt > 60000) requestUpdate();
}

void PersonActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  shownChanges = node().changes();
  drawnAt = millis();
  const auto& net = node().network();
  const auto& st = node().member(member);
  renderer.clearScreen();
  int y = meshui::drawTitle(renderer, net.name(member)) + 24;

  mesh::Position mine;
  const bool haveMine = myPosition(mine);
  std::vector<std::string> lines;
  if (st.sos) lines.push_back("SOS er aktiv");
  if (!st.hasPosition) {
    renderer.drawCenteredText(UI_12_FONT_ID, y + 40, "Ingen posisjon ennå", true, EpdFontFamily::BOLD);
    y += 120;
  } else if (!haveMine) {
    renderer.drawCenteredText(UI_12_FONT_ID, y + 40, "Venter på din egen posisjon", true, EpdFontFamily::BOLD);
    y += 120;
  } else {
    // How far, big, and an arrow on a small compass (north up).
    const double d = meshui::distanceM(mine, st.position);
    const double b = meshui::bearingDeg(mine, st.position);
    const std::string far = meshui::distanceText(d);
    const int cx = width / 2, cy = y + 130, r = 110;
    renderer.drawArc(r, cx, cy, 1, 1, 2, true);
    renderer.drawArc(r, cx, cy, -1, 1, 2, true);
    renderer.drawArc(r, cx, cy, 1, -1, 2, true);
    renderer.drawArc(r, cx, cy, -1, -1, 2, true);
    renderer.drawCenteredText(UI_10_FONT_ID, cy - r - 28, "N", true, EpdFontFamily::BOLD);
    const double a = b * M_PI / 180.0;
    const double ux = std::sin(a), uy = -std::cos(a);
    const int tipX = cx + int((r - 12) * ux), tipY = cy + int((r - 12) * uy);
    const int baseX = cx + int((r - 52) * ux), baseY = cy + int((r - 52) * uy);
    renderer.drawLine(cx - int(40 * ux), cy - int(40 * uy), baseX, baseY, 6, true);
    const int xs[] = {tipX, baseX + int(18 * uy), baseX - int(18 * uy)};
    const int ys[] = {tipY, baseY - int(18 * ux), baseY + int(18 * ux)};
    renderer.fillPolygon(xs, ys, 3);
    y = cy + r + 30;
    renderer.drawCenteredText(UI_12_FONT_ID, y, far.c_str(), true, EpdFontFamily::BOLD);
    char way[48];
    snprintf(way, sizeof(way), "%s (%d°)", meshui::directionText(b), int(b + 0.5) % 360);
    renderer.drawCenteredText(UI_12_FONT_ID, y + 40, way);
    y += 100;
  }
  if (st.hasPosition) {
    const std::string when = meshui::timeText(st.position.time);
    const std::string ago = meshui::agoText(st.position.time);
    lines.push_back("Posisjonen er fra " + when + (ago.empty() ? "" : " · " + ago));
    if (st.position.altitude) lines.push_back("Høyde " + std::to_string(st.position.altitude) + " moh");
    lines.push_back(meshui::coordinatesText(st.position));
  }
  if (st.battery) lines.push_back("Batteri " + std::to_string(st.battery) + " %");
  if (st.heardAtMs) {
    const uint32_t minutes = (millis() - st.heardAtMs) / 60000;
    lines.push_back("Hørt direkte " + (minutes ? std::to_string(minutes) + " min siden" : std::string("nå nettopp")) +
                    " · signal " + std::to_string(st.rssi) + " dBm");
  } else {
    lines.push_back("Ikke hørt direkte (bare gjennom andre eller serveren)");
  }
  for (const auto& line : lines) {
    renderer.drawText(UI_10_FONT_ID, SIDE, y, line.c_str());
    y += 36;
  }

  writeY = height - 110;
  renderer.fillRoundedRect(SIDE, writeY + 10, width - 2 * SIDE, 64, 14, Color::Black);
  const std::string label = std::string("Skriv til ") + net.name(member);
  const int lw = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, (width - lw) / 2, writeY + 28, label.c_str(), false, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}
