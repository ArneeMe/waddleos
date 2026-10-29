#include "T5HomeActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>

#include <algorithm>
#include <vector>

#include "apps/map/MapActivity.h"
#include "apps/messages/MeshUi.h"
#include "apps/messages/MessagesActivity.h"
#include "apps/notes/NotesActivity.h"
#include "apps/sos/SosAlertActivity.h"
#include "core/MappedInputManager.h"
#include "core/components/UITheme.h"
#include "core/components/themes/BaseTheme.h"
#include "core/fontIds.h"
#include "core/waddle/WaddleDraw.h"
#include "core/CrossPointSettings.h"
#include "services/mesh/MeshNode.h"
#include "services/mesh/MeshSettings.h"

namespace {

constexpr int SIDE = 20;
constexpr const char* WEEKDAYS[] = {"søndag", "mandag", "tirsdag", "onsdag", "torsdag", "fredag", "lørdag"};
constexpr const char* MONTHS[] = {"januar", "februar", "mars",     "april",   "mai",      "juni",
                                  "juli",   "august",  "september", "oktober", "november", "desember"};
constexpr const char* APP_LABELS[] = {"Meldinger", "Kart", "Notater", "Bøker", "Innstillinger", "SOS"};

mesh::Node& node() { return mesh::Node::get(); }

}  // namespace

T5HomeActivity::T5HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool cleanInitialRefresh)
    : Activity("Home", renderer, mappedInput), cleanInitialRefresh(cleanInitialRefresh) {}

void T5HomeActivity::onEnter() {
  Activity::onEnter();
  // "Alle", then the three contacts most recently written with (or heard);
  // the rest are in Meldinger.
  chatCount = 0;
  if (node().configured()) {
    chats[chatCount++] = mesh::CONV_GROUP;
    std::vector<uint8_t> people;
    for (uint8_t m = 1; m <= mesh::MAX_MEMBERS; m++) {
      if (m != node().group().me && node().group().member(m)->present) people.push_back(m);
    }
    auto recent = [](const uint8_t m) {
      const uint32_t wrote = node().store().summary(m).lastTime;
      return wrote ? uint64_t(wrote) << 1 | 1 : uint64_t(node().member(m).heardAtMs);
    };
    std::stable_sort(people.begin(), people.end(), [&](const uint8_t a, const uint8_t b) {
      return recent(a) > recent(b);
    });
    for (const uint8_t m : people) {
      if (chatCount < 4) chats[chatCount++] = m;
    }
  }
  if (!available(focus)) focus = T_MESSAGES;
  requestUpdate();
}

bool T5HomeActivity::available(const int target) const {
  if (target >= T_CHAT_1 && target <= T_CHAT_4) return target - T_CHAT_1 < chatCount;
  if (target == T_MODE) return node().configured();
  return target >= 0 && target < T_COUNT;
}

void T5HomeActivity::activate(const int target) {
  if (target >= T_CHAT_1 && target <= T_CHAT_4) {
    activityManager.replaceActivity(std::make_unique<ChatActivity>(renderer, mappedInput, chats[target - T_CHAT_1]));
    return;
  }
  switch (target) {
    case T_MODE:
      // Going out, or home again: GPS, position sharing and checks follow.
      MESH_SETTINGS.onTrip = MESH_SETTINGS.onTrip ? 0 : 1;
      SETTINGS.saveToFile();
      node().startCheck();
      requestUpdate();
      break;
    case T_MESSAGES:
      activityManager.replaceActivity(std::make_unique<MessagesActivity>(renderer, mappedInput));
      break;
    case T_MAP:
      activityManager.replaceActivity(std::make_unique<MapActivity>(renderer, mappedInput));
      break;
    case T_NOTES:
      activityManager.replaceActivity(std::make_unique<NotesActivity>(renderer, mappedInput));
      break;
    case T_BOOKS:
      activityManager.goToLibrary();
      break;
    case T_SETTINGS:
      activityManager.goToSettings();
      break;
    case T_SOS:
      activityManager.replaceActivity(std::make_unique<SosActivity>(renderer, mappedInput));
      break;
    default:
      break;
  }
}

void T5HomeActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Confirm)) {
    activate(focus);
    return;
  }
  const int step = mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::PageForward)  ? 1
                   : mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::PageBack) ? -1
                                                                                                       : 0;
  if (step) {
    do {
      focus = (focus + step + T_COUNT) % T_COUNT;
    } while (!available(focus));
    requestUpdate();
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    for (int t = 0; t < T_COUNT; t++) {
      if (available(t) && boxes[t].contains(x, y)) {
        activate(t);
        return;
      }
    }
  }
  uint8_t hour = 0, minute = 0;
  if ((halClock.getTime(hour, minute) && minute != shownMinute) || node().changes() != shownChanges) requestUpdate();
}

void T5HomeActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  shownChanges = node().changes();
  renderer.clearScreen();
  for (auto& b : boxes) b = {};

  // Battery, top right.
  {
    const uint16_t percent = std::min<uint16_t>(powerManager.getBatteryPercentage(), 100);
    char text[8];
    snprintf(text, sizeof(text), "%u%%", percent);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, text);
    renderer.drawText(SMALL_FONT_ID, width - SIDE - textW, 6, text);
    BaseTheme::drawBatteryOutline(renderer, width - SIDE - textW - 26, 8, 18, 11);
    GUI.fillBatteryIcon(renderer, Rect{width - SIDE - textW - 26, 8, 18, 11}, percent);
  }

  // Clock and date.
  struct tm now {};
  const bool hasTime = halClock.localTime(now) && now.tm_year >= 120;
  char clock[8] = "--:--";
  if (hasTime) {
    snprintf(clock, sizeof(clock), "%02d:%02d", now.tm_hour, now.tm_min);
    shownMinute = now.tm_min;
  }
  waddle::drawPixelText(renderer, SIDE, 36, clock, 14);
  int y = 36 + 7 * 14 + 14;
  if (hasTime) {
    char date[40];
    snprintf(date, sizeof(date), "%s %d. %s", WEEKDAYS[now.tm_wday % 7], now.tm_mday, MONTHS[now.tm_mon % 12]);
    renderer.drawText(UI_12_FONT_ID, SIDE, y, date);
  }
  y += 40;

  if (node().sosActive()) {
    renderer.fillRoundedRect(SIDE, y, width - 2 * SIDE, 44, 10, Color::Black);
    renderer.drawCenteredText(UI_12_FONT_ID, y + 9, "SOS er aktiv · trykk SOS for å avbryte", false,
                              EpdFontFamily::BOLD);
    y += 54;
  }
  // The mode, as a wide button: tap to switch.
  if (node().configured()) {
    Box& b = boxes[T_MODE];
    b = {SIDE, y - 6, width - 2 * SIDE, 64};
    if (focus == T_MODE) {
      renderer.fillRoundedRect(b.x, b.y, b.w, b.h, 12, Color::Black);
    } else {
      renderer.drawRoundedRect(b.x, b.y, b.w, b.h, 2, 12, true);
    }
    const bool dark = focus == T_MODE;
    renderer.drawText(UI_10_FONT_ID, b.x + 16, b.y + 6, meshui::modeText().c_str(), !dark, EpdFontFamily::BOLD);
    std::string under = meshui::lastCheckText();
    under += std::string(under.empty() ? "" : " · ") + (MESH_SETTINGS.onTrip ? "trykk når du er hjemme" : "trykk når du drar på tur");
    renderer.drawText(SMALL_FONT_ID, b.x + 16, b.y + 36, under.c_str(), !dark);
    y += 70;
  } else {
    renderer.drawText(SMALL_FONT_ID, SIDE, y, "Ikke med i et nettverk: kjør \"waddle mesh join\"");
    y += 30;
  }
  renderer.drawLine(SIDE, y, width - SIDE, y, 2, true);
  y += 12;

  // Everyone, then each contact.
  renderer.drawText(SMALL_FONT_ID, SIDE, y, "KONTAKTER", true, EpdFontFamily::BOLD);
  y += 30;
  const auto& group = node().group();
  for (int i = 0; i < chatCount; i++) {
    const uint8_t c = chats[i];
    const auto& sum = node().store().summary(c);
    boxes[T_CHAT_1 + i] = {0, y, width, 74};
    if (focus == T_CHAT_1 + i) renderer.fillRect(0, y + 1, 6, 72);
    const std::string name = meshui::conversationName(c);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 4, name.c_str(), true, EpdFontFamily::BOLD);
    if (c != mesh::CONV_GROUP) {
      // How they are: heard, battery, distance.
      const int nameW = renderer.getTextWidth(UI_12_FONT_ID, name.c_str(), EpdFontFamily::BOLD);
      const int maxW = width - 2 * SIDE - nameW - 16 - 60;
      renderer.drawText(SMALL_FONT_ID, SIDE + nameW + 16, y + 10,
                        renderer.truncatedText(SMALL_FONT_ID, meshui::contactText(c).c_str(), maxW).c_str());
    }
    std::string preview = "Ingen meldinger ennå";
    if (sum.count) {
      const char* who = sum.lastSender == group.me ? "Du" : group.name(sum.lastSender);
      preview = (c == mesh::CONV_GROUP || sum.lastSender == group.me) ? std::string(who) + ": " + sum.lastText
                                                                       : sum.lastText;
    }
    renderer.drawText(UI_10_FONT_ID, SIDE, y + 38,
                      renderer.truncatedText(UI_10_FONT_ID, preview.c_str(), width - 2 * SIDE - 70).c_str());
    if (sum.unread) {
      char n[8];
      snprintf(n, sizeof(n), "%u", static_cast<unsigned>(sum.unread));
      const int cx = width - SIDE - 22;
      renderer.fillRoundedRect(cx - 22, y + 14, 44, 44, 22, Color::Black);
      const int nw = renderer.getTextWidth(UI_12_FONT_ID, n, EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, cx - nw / 2, y + 22, n, false, EpdFontFamily::BOLD);
    }
    y += 74;
    renderer.drawLine(SIDE, y, width - SIDE, y);
  }

  // App buttons, two rows of three.
  const int bw = (width - 2 * SIDE - 2 * 14) / 3;
  const int bh = 100;
  const int gridY = std::max(y + 24, renderer.getScreenHeight() - 2 * (bh + 14) - 120);
  for (int i = 0; i < 6; i++) {
    const int target = T_MESSAGES + i;
    Box& b = boxes[target];
    b = {SIDE + (i % 3) * (bw + 14), gridY + (i / 3) * (bh + 14), bw, bh};
    const bool filled = target == T_SOS || focus == target;
    if (filled) {
      renderer.fillRoundedRect(b.x, b.y, b.w, b.h, 14, Color::Black);
    } else {
      renderer.drawRoundedRect(b.x, b.y, b.w, b.h, 3, 14, true);
    }
    if (target == T_SOS && focus == target) renderer.drawRoundedRect(b.x + 6, b.y + 6, b.w - 12, b.h - 12, 2, 10, false);
    const int lw = renderer.getTextWidth(UI_12_FONT_ID, APP_LABELS[i], EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, b.x + (b.w - lw) / 2, b.y + 36, APP_LABELS[i], !filled, EpdFontFamily::BOLD);
  }

  // The penguin and a reminder of the SOS button, when there is room.
  const int py = renderer.getScreenHeight() - 96;
  if (gridY + 2 * (bh + 14) > py) {
    renderer.displayBuffer(cleanInitialRefresh ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    cleanInitialRefresh = false;
    return;
  }
  waddle::drawPenguin(renderer, SIDE, py, 2);
  // Each unit greets its own member; before setup, the device's owner.
  char hello[40];
  snprintf(hello, sizeof(hello), "Hei %s!", node().configured() ? group.name(group.me) : WADDLE_OWNER_NAME);
  renderer.drawText(UI_12_FONT_ID, SIDE + 76, py + 6, hello, true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, SIDE + 76, py + 40, "Hold knappen i 3 sek for SOS");

  renderer.displayBuffer(cleanInitialRefresh ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanInitialRefresh = false;
}
