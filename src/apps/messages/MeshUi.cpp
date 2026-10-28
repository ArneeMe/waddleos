#include "MeshUi.h"

#include <GfxRenderer.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

#include "core/components/UITheme.h"
#include "core/fontIds.h"
#include "services/mesh/MeshGps.h"
#include "services/mesh/MeshNode.h"
#include "services/mesh/MeshSettings.h"

namespace meshui {

namespace {
constexpr const char* MONTHS[] = {"jan", "feb", "mar", "apr", "mai", "jun", "jul", "aug", "sep", "okt", "nov", "des"};
constexpr double EARTH_M = 6371000.0;
double rad(const int32_t e7) { return double(e7) / 1e7 * M_PI / 180.0; }
}  // namespace

int drawTitle(const GfxRenderer& renderer, const char* title) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
  return metrics.topPadding + metrics.headerHeight + 10;
}

uint32_t nowUnix() {
  const time_t t = time(nullptr);
  return t > 1600000000 ? uint32_t(t) : 0;
}

std::string timeText(const uint32_t unixTime) {
  if (!unixTime) return "";
  const time_t t = unixTime;
  struct tm when {};
  localtime_r(&t, &when);
  const time_t n = nowUnix() ? time_t(nowUnix()) : t;
  struct tm today {};
  localtime_r(&n, &today);
  char out[24];
  if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday) {
    snprintf(out, sizeof(out), "%02d:%02d", when.tm_hour, when.tm_min);
  } else if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday - 1) {
    snprintf(out, sizeof(out), "i går %02d:%02d", when.tm_hour, when.tm_min);
  } else {
    snprintf(out, sizeof(out), "%d. %s", when.tm_mday, MONTHS[when.tm_mon % 12]);
  }
  return out;
}

std::string agoText(const uint32_t unixTime) {
  const uint32_t now = nowUnix();
  if (!unixTime || !now) return "";
  const uint32_t s = now > unixTime ? now - unixTime : 0;
  char out[24];
  if (s < 90) return "nå nettopp";
  if (s < 3600) {
    snprintf(out, sizeof(out), "%lu min siden", static_cast<unsigned long>(s / 60));
  } else if (s < 86400) {
    snprintf(out, sizeof(out), "%lu t siden", static_cast<unsigned long>(s / 3600));
  } else {
    snprintf(out, sizeof(out), "%lu d siden", static_cast<unsigned long>(s / 86400));
  }
  return out;
}

double distanceM(const mesh::Position& a, const mesh::Position& b) {
  const double dLat = rad(b.latE7 - a.latE7);
  const double dLon = rad(b.lonE7 - a.lonE7);
  const double h =
      std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(rad(a.latE7)) * std::cos(rad(b.latE7)) * std::sin(dLon / 2) * std::sin(dLon / 2);
  return 2 * EARTH_M * std::asin(std::sqrt(h));
}

double bearingDeg(const mesh::Position& a, const mesh::Position& b) {
  const double y = std::sin(rad(b.lonE7 - a.lonE7)) * std::cos(rad(b.latE7));
  const double x = std::cos(rad(a.latE7)) * std::sin(rad(b.latE7)) -
                   std::sin(rad(a.latE7)) * std::cos(rad(b.latE7)) * std::cos(rad(b.lonE7 - a.lonE7));
  const double deg = std::atan2(y, x) * 180.0 / M_PI;
  return deg < 0 ? deg + 360.0 : deg;
}

std::string distanceText(const double metres) {
  char out[24];
  if (metres < 1000) {
    snprintf(out, sizeof(out), "%d m", int(metres / 10 + 0.5) * 10);
  } else if (metres < 10000) {
    snprintf(out, sizeof(out), "%d,%d km", int(metres / 1000), int(std::fmod(metres, 1000) / 100));
  } else {
    snprintf(out, sizeof(out), "%d km", int(metres / 1000 + 0.5));
  }
  return out;
}

const char* directionText(const double degrees) {
  static const char* names[] = {"nord", "nordøst", "øst", "sørøst", "sør", "sørvest", "vest", "nordvest"};
  return names[int((degrees + 22.5) / 45.0) % 8];
}

std::string coordinatesText(const mesh::Position& p) {
  const double lat = p.latE7 / 1e7;
  const double lon = p.lonE7 / 1e7;
  char out[48];
  snprintf(out, sizeof(out), "%.4f %s  %.4f %s", std::fabs(lat), lat >= 0 ? "N" : "S", std::fabs(lon),
           lon >= 0 ? "Ø" : "V");
  // Norwegian decimal comma.
  for (char* c = out; *c; c++) {
    if (*c == '.') *c = ',';
  }
  return out;
}

std::string conversationName(const uint8_t conversation) {
  const auto& group = mesh::Node::get().group();
  if (conversation == mesh::CONV_GROUP) return "Alle";
  if (!mesh::isSmallConversation(conversation)) return group.name(conversation);
  const uint8_t bits = conversation - mesh::CONV_SMALL;
  std::vector<std::string> names;
  if (bits & mesh::memberBit(group.me)) names.push_back("Du");
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    if (n != group.me && (bits & mesh::memberBit(n))) names.push_back(group.name(n));
  }
  std::string out;
  for (size_t i = 0; i < names.size(); i++) {
    out += (i == 0 ? "" : i + 1 == names.size() ? " og " : ", ") + names[i];
  }
  return out;
}

std::string contactText(const uint8_t member) {
  auto& node = mesh::Node::get();
  const auto& st = node.member(member);
  std::vector<std::string> parts;
  if (st.heardAtMs) {
    const uint32_t ago = (millis() - st.heardAtMs) / 1000;
    const uint32_t now = nowUnix();
    parts.push_back("hørt " + (now ? agoText(now - ago) : std::string("nylig")));
  } else if (st.hasPosition && st.position.time) {
    parts.push_back("posisjon " + agoText(st.position.time));
  } else {
    parts.push_back("ikke hørt ennå");
  }
  if (st.battery) parts.push_back(std::to_string(st.battery) + " %");
  // Distance when we both know where we are.
  const auto& self = node.member(node.group().me);
  mesh::Position mine;
  bool haveMine = false;
  if (mesh::gps::lastFix().valid) {
    mine.latE7 = mesh::gps::lastFix().latE7;
    mine.lonE7 = mesh::gps::lastFix().lonE7;
    haveMine = true;
  } else if (self.hasPosition) {
    mine = self.position;
    haveMine = true;
  }
  if (haveMine && st.hasPosition) {
    parts.push_back(distanceText(distanceM(mine, st.position)) + " " + directionText(bearingDeg(mine, st.position)));
  }
  std::string out;
  for (size_t i = 0; i < parts.size(); i++) out += (i ? " · " : "") + parts[i];
  return out;
}

std::string modeText() {
  const auto& s = MESH_SETTINGS;
  const char* every = "lytter hele tiden";
  switch (s.onTrip ? s.tripCheck : s.homeCheck) {
    case MeshSettings::CHECK_15_MIN:
      every = "sjekker hvert kvarter";
      break;
    case MeshSettings::CHECK_HOUR:
      every = "sjekker hver time";
      break;
    case MeshSettings::CHECK_6_HOURS:
      every = "sjekker hver 6. time";
      break;
    case MeshSettings::CHECK_DAY:
      every = "sjekker en gang i døgnet";
      break;
    case MeshSettings::CHECK_ON_WAKE:
      every = "sjekker når du vekker den";
      break;
    default:
      break;
  }
  return std::string(s.onTrip ? "På tur" : "Hjemme") + " · " + every;
}

std::string lastCheckText() {
  const uint32_t at = mesh::Node::get().lastCheckUnix();
  return at ? "sist sjekket " + timeText(at) : std::string();
}

void drawUnreadLine(const GfxRenderer& renderer, const uint32_t unread) {
  auto& node = mesh::Node::get();
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
  renderer.fillRect(0, y, width, 96, false);
  renderer.fillRoundedRect(20, y + 16, width - 40, 64, 14, Color::Black);
  const int w = renderer.getTextWidth(UI_12_FONT_ID, line, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, (width - w) / 2, y + 34, line, false, EpdFontFamily::BOLD);
}

}  // namespace meshui
