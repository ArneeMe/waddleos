#include "MessagesActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Memory.h>

#include "MeshUi.h"
#include "core/MappedInputManager.h"
#include "core/activities/util/KeyboardEntryActivity.h"
#include "core/fontIds.h"
#include "services/mesh/MeshNode.h"

namespace {

constexpr int SIDE = 20;
constexpr int ROW_H = 84;
constexpr int BUBBLE_MAX_W = 380;
constexpr int TEXT_FONT = NOTOSANS_14_FONT_ID;

mesh::Node& node() { return mesh::Node::get(); }

const char* conversationName(const uint8_t conversation) {
  return conversation == mesh::CONV_GROUP ? "Gruppe" : node().group().name(conversation);
}

// What the sender sees under their own message.
std::string deliveryText(const mesh::ChatMessage& m, const uint8_t conversation) {
  const auto& group = node().group();
  uint8_t others = 0;
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    if (n != group.me && group.member(n)->present) others |= uint8_t(1u << (n - 1));
  }
  const uint8_t wanted = conversation == mesh::CONV_GROUP ? others : uint8_t(1u << (conversation - 1));
  std::string when = meshui::timeText(m.time);
  if (!when.empty()) when += " · ";
  if (m.readBy && (m.readBy & wanted) == wanted) return when + "lest";
  if ((m.deliveredTo & wanted) == wanted) return when + (conversation == mesh::CONV_GROUP ? "levert til alle" : "levert");
  if (m.deliveredTo) return when + "levert til noen";
  if (m.stored) return when + "lagret på server";
  if (m.sent) return when + "sendt";
  return when + "sender …";
}

}  // namespace

// --- Conversation list -------------------------------------------------------

MessagesActivity::MessagesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Messages", renderer, mappedInput) {}

void MessagesActivity::onEnter() {
  Activity::onEnter();
  conversations.clear();
  conversations.push_back(mesh::CONV_GROUP);
  const auto& group = node().group();
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    if (n != group.me && group.member(n)->present) conversations.push_back(n);
  }
  requestUpdate();
}

bool MessagesActivity::preventAutoSleep() { return node().wifiSyncRunning(); }

void MessagesActivity::open(const uint8_t conversation) {
  activityManager.replaceActivity(std::make_unique<ChatActivity>(renderer, mappedInput, conversation));
}

void MessagesActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.goHome();
    return;
  }
  if (!node().configured()) {
    // Setup happens from the computer while File Transfer is open.
    int tx = 0, ty = 0;
    if (mappedInput.wasReleased(Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) activityManager.goToFileTransfer();
    return;
  }
  const int count = static_cast<int>(conversations.size());
  if (mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::PageForward)) {
    focus = (focus + 1) % count;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::PageBack)) {
    focus = (focus + count - 1) % count;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    open(conversations[focus]);
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (syncBox.contains(x, y)) {
      node().startWifiSync();
      node().requestSync();
      requestUpdate();
      return;
    }
    for (size_t i = 0; i < rows.size(); i++) {
      if (rows[i].contains(x, y)) {
        open(conversations[i]);
        return;
      }
    }
  }
  if (node().changes() != shownChanges) requestUpdate();
}

void MessagesActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  shownChanges = node().changes();
  renderer.clearScreen();
  int y = meshui::drawTitle(renderer, "Meldinger");

  if (!node().configured()) {
    const auto lines = renderer.wrappedText(UI_12_FONT_ID,
                                            "Enheten er ikke satt opp ennå. Trykk her for å starte "
                                            "Filoverføring, og kjør \"waddle mesh setup\" på datamaskinen.",
                                            width - 2 * SIDE, 6);
    for (const auto& line : lines) {
      renderer.drawText(UI_12_FONT_ID, SIDE, y + 20, line.c_str());
      y += renderer.getLineHeight(UI_12_FONT_ID);
    }
    renderer.displayBuffer();
    return;
  }

  // Server status and the sync button.
  const char* server = node().wifiSyncRunning()                                  ? "Synker med serveren …"
                       : node().serverState() == mesh::ServerState::Online   ? "Server: tilkoblet"
                       : node().serverState() == mesh::ServerState::Offline  ? "Server: ikke nådd"
                                                                             : "Server: ukjent";
  renderer.drawText(SMALL_FONT_ID, SIDE, y + 8, server);
  syncBox = {width - SIDE - 110, y - 4, 110, 44};
  renderer.drawRoundedRect(syncBox.x, syncBox.y, syncBox.w, syncBox.h, 2, 10, true);
  const int syncW = renderer.getTextWidth(UI_12_FONT_ID, "Synk", EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, syncBox.x + (syncBox.w - syncW) / 2, syncBox.y + 8, "Synk", true, EpdFontFamily::BOLD);
  y += 56;
  renderer.drawLine(SIDE, y, width - SIDE, y);

  rows.assign(conversations.size(), Box{});
  for (size_t i = 0; i < conversations.size(); i++) {
    const uint8_t c = conversations[i];
    const auto& sum = node().store().summary(c);
    rows[i] = {0, y, width, ROW_H};
    if (static_cast<int>(i) == focus) renderer.fillRect(0, y + 1, 6, ROW_H - 2);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 12, conversationName(c), true, EpdFontFamily::BOLD);
    std::string preview = sum.count ? sum.lastText : std::string("Ingen meldinger ennå");
    if (sum.count && c == mesh::CONV_GROUP) {
      preview = std::string(sum.lastSender == node().group().me ? "Du" : node().group().name(sum.lastSender)) + ": " + preview;
    } else if (sum.count && sum.lastSender == node().group().me) {
      preview = "Du: " + preview;
    }
    renderer.drawText(UI_10_FONT_ID, SIDE, y + 46, renderer.truncatedText(UI_10_FONT_ID, preview.c_str(), width - 2 * SIDE - 70).c_str());
    if (sum.unread) {
      char n[8];
      snprintf(n, sizeof(n), "%u", static_cast<unsigned>(sum.unread));
      const int cx = width - SIDE - 22;
      renderer.fillRoundedRect(cx - 22, y + 20, 44, 44, 22, Color::Black);
      const int nw = renderer.getTextWidth(UI_12_FONT_ID, n, EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, cx - nw / 2, y + 28, n, false, EpdFontFamily::BOLD);
    }
    y += ROW_H;
    renderer.drawLine(SIDE, y, width - SIDE, y);
  }
  renderer.displayBuffer();
}

// --- One conversation --------------------------------------------------------

ChatActivity::ChatActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const uint8_t conversation)
    : Activity("Chat", renderer, mappedInput), conversation(conversation) {}

const char* ChatActivity::title() const { return conversationName(conversation); }

void ChatActivity::reload() {
  auto loaded = node().store().load(conversation, 40);
  RenderLock lock(*this);
  messages = std::move(loaded);
}

void ChatActivity::onEnter() {
  Activity::onEnter();
  reload();
  node().markRead(conversation);
  requestUpdate();
}

void ChatActivity::write() {
  char heading[48];
  snprintf(heading, sizeof(heading), "Til %s", title());
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, heading, "", mesh::MAX_TEXT,
                                                           InputType::Text);
  if (!keyboard) return;
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const std::string text = std::get<KeyboardResult>(result.data).text;
      if (!text.empty()) node().sendText(conversation, text);
    }
    scroll = 0;
    reload();
    requestUpdate();
  });
}

void ChatActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.replaceActivity(std::make_unique<MessagesActivity>(renderer, mappedInput));
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    write();
    return;
  }
  if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::PageBack)) {
    if (scroll + 1 < static_cast<int>(messages.size())) scroll += 3;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::PageForward)) {
    scroll = std::max(0, scroll - 3);
    requestUpdate();
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (y >= inputY) {
      write();
      return;
    }
    // Upper half: older messages; lower half: newer.
    scroll = y < renderer.getScreenHeight() / 2 ? scroll + 3 : std::max(0, scroll - 3);
    requestUpdate();
  }
  if (node().changes() != shownChanges) {
    shownChanges = node().changes();
    reload();
    node().markRead(conversation);
    requestUpdate();
  }
}

void ChatActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const uint8_t me = node().group().me;
  renderer.clearScreen();
  const int top = meshui::drawTitle(renderer, title());

  // Input bar.
  inputY = height - 82;
  renderer.drawLine(0, inputY, width, inputY, 2, true);
  renderer.drawRoundedRect(SIDE, inputY + 16, width - 2 * SIDE - 116, 54, 2, 14, true);
  renderer.drawText(UI_12_FONT_ID, SIDE + 16, inputY + 30, "Skriv melding …", true);
  renderer.fillRoundedRect(width - SIDE - 104, inputY + 16, 104, 54, 14, Color::Black);
  const int sendW = renderer.getTextWidth(UI_12_FONT_ID, "Send", EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, width - SIDE - 52 - sendW / 2, inputY + 30, "Send", false, EpdFontFamily::BOLD);

  // Bubbles from the bottom up.
  const int lineH = renderer.getLineHeight(TEXT_FONT);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  int y = inputY - 12;
  const int last = static_cast<int>(messages.size()) - 1 - scroll;
  for (int i = last; i >= 0; i--) {
    const auto& m = messages[i];
    const bool mine = m.sender == me;
    const auto lines = renderer.wrappedText(TEXT_FONT, m.text.c_str(), BUBBLE_MAX_W - 32, 12);
    int textW = 0;
    for (const auto& l : lines) textW = std::max(textW, renderer.getTextWidth(TEXT_FONT, l.c_str()));
    const int bw = std::min(BUBBLE_MAX_W, textW + 32);
    const int bh = static_cast<int>(lines.size()) * lineH + 20;
    const bool showName = !mine && conversation == mesh::CONV_GROUP;
    const int below = mine ? smallH + 6 : 0;
    const int above = mine ? 0 : smallH + 6;
    const int needed = above + bh + below + 14;
    if (y - needed < top) break;
    y -= below;
    if (mine) {
      const std::string state = deliveryText(m, conversation);
      const int sw = renderer.getTextWidth(SMALL_FONT_ID, state.c_str());
      renderer.drawText(SMALL_FONT_ID, width - SIDE - sw, y + 2, state.c_str());
    }
    y -= bh;
    const int bx = mine ? width - SIDE - bw : SIDE;
    if (mine) {
      renderer.fillRoundedRect(bx, y, bw, bh, 16, Color::Black);
    } else {
      renderer.drawRoundedRect(bx, y, bw, bh, 2, 16, true);
    }
    int ty = y + 10;
    for (const auto& l : lines) {
      renderer.drawText(TEXT_FONT, bx + 16, ty, l.c_str(), !mine);
      ty += lineH;
    }
    if (!mine) {
      std::string head = (showName ? std::string(node().group().name(m.sender)) + "  " : std::string()) +
                         meshui::timeText(m.time);
      if (m.chainWarning) head += "  (!) en melding kan mangle";
      y -= above;
      renderer.drawText(SMALL_FONT_ID, bx + 4, y, head.c_str(), true, EpdFontFamily::BOLD);
    }
    y -= 14;
  }
  if (messages.empty()) renderer.drawCenteredText(UI_12_FONT_ID, top + 40, "Ingen meldinger ennå. Si hei!");

  // Fast refresh while chatting, a clean one now and then against ghosting.
  renderer.displayBuffer(++renders % 8 == 0 ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
