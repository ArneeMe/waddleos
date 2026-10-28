#include "MessagesActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Memory.h>

#include <algorithm>

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

// Member bits of everyone in the group.
uint8_t everyone() { return node().group().everyone(); }

bool isDm(const uint8_t conversation) { return mesh::isDmConversation(conversation); }

const mesh::ChatGroup* chatGroup(const uint8_t conversation) {
  return mesh::isGroupConversation(conversation) ? &node().groups().at(conversation - mesh::CONV_GROUPS) : nullptr;
}

// What the sender sees under their own message.
std::string deliveryText(const mesh::ChatMessage& m, const uint8_t conversation) {
  const uint8_t me = mesh::memberBit(node().group().me);
  const mesh::ChatGroup* g = chatGroup(conversation);
  const uint8_t wanted = conversation == mesh::CONV_GROUP ? uint8_t(everyone() & ~me)
                         : g                              ? uint8_t(g->members & ~me)
                                                          : mesh::memberBit(conversation);
  std::string when = meshui::timeText(m.time);
  if (!when.empty()) when += " · ";
  if (m.readBy && (m.readBy & wanted) == wanted) return when + "lest";
  if ((m.deliveredTo & wanted) == wanted) return when + (isDm(conversation) ? "levert" : "levert til alle");
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
  // Groups I am in, and those I left that still have messages to read.
  for (uint8_t c = mesh::CONV_GROUPS; c < mesh::CONVERSATIONS; c++) {
    const mesh::ChatGroup* g = chatGroup(c);
    if (g->id && (!g->left || node().store().summary(c).count)) conversations.push_back(c);
  }
  if (node().configured() && mesh::memberCount(everyone()) >= 3) conversations.push_back(NEW_GROUP);
  requestUpdate();
}

bool MessagesActivity::preventAutoSleep() { return node().wifiSyncRunning(); }

void MessagesActivity::open(const uint8_t conversation) {
  if (conversation == NEW_GROUP) {
    activityManager.replaceActivity(std::make_unique<NewGroupActivity>(renderer, mappedInput));
    return;
  }
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
      if (rows[i].h && rows[i].contains(x, y)) {
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

  // As many rows as fit, scrolled so the focused one shows.
  const int fit = std::max(1, (renderer.getScreenHeight() - y) / ROW_H);
  const int count = static_cast<int>(conversations.size());
  if (focus < first) first = focus;
  if (focus >= first + fit) first = focus - fit + 1;
  rows.assign(conversations.size(), Box{});
  for (int i = first; i < std::min(count, first + fit); i++) {
    const uint8_t c = conversations[i];
    rows[i] = {0, y, width, ROW_H};
    if (i == focus) renderer.fillRect(0, y + 1, 6, ROW_H - 2);
    if (c == NEW_GROUP) {
      renderer.drawText(UI_12_FONT_ID, SIDE, y + 12, "+ Ny gruppe", true, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, SIDE, y + 46, "Skriv til noen av dere");
      y += ROW_H;
      renderer.drawLine(SIDE, y, width - SIDE, y);
      continue;
    }
    const auto& sum = node().store().summary(c);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 12,
                      renderer.truncatedText(UI_12_FONT_ID, meshui::conversationName(c).c_str(), width - 2 * SIDE - 70).c_str(),
                      true, EpdFontFamily::BOLD);
    std::string preview = sum.count ? sum.lastText : std::string("Ingen meldinger ennå");
    if (sum.count && !isDm(c)) {
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
    : Activity("Chat", renderer, mappedInput), conversation(conversation), name(meshui::conversationName(conversation)) {}

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
  snprintf(heading, sizeof(heading), "Til %s", name.c_str());
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
    if (y < titleBottom && chatGroup(conversation)) {
      activityManager.replaceActivity(std::make_unique<GroupInfoActivity>(renderer, mappedInput, conversation));
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
  // A group's title opens who is in it.
  const std::string title = chatGroup(conversation) ? name + "  ›" : name;
  const int top = meshui::drawTitle(renderer, title.c_str());
  titleBottom = top;

  // Input bar.
  inputY = height - 82;
  renderer.drawLine(0, inputY, width - 1, inputY, 2, true);
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
    const bool showName = !mine && !isDm(conversation);
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

// --- Ny gruppe -----------------------------------------------------------------

NewGroupActivity::NewGroupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NewGroup", renderer, mappedInput) {}

void NewGroupActivity::onEnter() {
  Activity::onEnter();
  members.clear();
  const auto& group = node().group();
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    if (n != group.me && group.member(n)->present) members.push_back(n);
  }
  requestUpdate();
}

void NewGroupActivity::toggle(const int index) {
  chosen ^= mesh::memberBit(members[index]);
  problem = nullptr;
  requestUpdate();
}

void NewGroupActivity::finish() {
  const uint8_t bits = chosen | mesh::memberBit(node().group().me);
  const int people = mesh::memberCount(bits);
  if (people < 2) {
    problem = "Velg minst én";
    requestUpdate();
    return;
  }
  // One person is the DM. A group with exactly these people already: open it.
  if (people == 2) {
    for (const uint8_t n : members) {
      if (chosen & mesh::memberBit(n)) return openChat(n);
    }
  }
  for (uint8_t c = mesh::CONV_GROUPS; c < mesh::CONVERSATIONS; c++) {
    const mesh::ChatGroup* g = chatGroup(c);
    if (g->id && !g->left && g->members == bits) return openChat(c);
  }
  // A name is optional; without one the group is called by its members.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, "Navn på gruppen (kan stå tomt)", "",
                                                           mesh::GROUP_NAME - 1, InputType::Text);
  if (!keyboard) return;
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const std::string name = std::get<KeyboardResult>(result.data).text;
    const uint8_t conversation = node().createGroup(chosen, name);
    if (conversation == mesh::CONV_GROUP) {
      problem = "Kunne ikke lage gruppen (16 grupper er maks: forlat en først)";
      requestUpdate();
      return;
    }
    openChat(conversation);
  });
}

void NewGroupActivity::openChat(const uint8_t conversation) {
  activityManager.replaceActivity(std::make_unique<ChatActivity>(renderer, mappedInput, conversation));
}

void NewGroupActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.replaceActivity(std::make_unique<MessagesActivity>(renderer, mappedInput));
    return;
  }
  const int count = static_cast<int>(members.size()) + 1;
  if (mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::PageForward)) {
    focus = (focus + 1) % count;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::PageBack)) {
    focus = (focus + count - 1) % count;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    if (focus < static_cast<int>(members.size())) {
      toggle(focus);
    } else {
      finish();
    }
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (doneBox.contains(x, y)) {
      finish();
      return;
    }
    for (size_t i = 0; i < rows.size(); i++) {
      if (rows[i].contains(x, y)) {
        toggle(static_cast<int>(i));
        return;
      }
    }
  }
}

void NewGroupActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  int y = meshui::drawTitle(renderer, "Ny gruppe");
  renderer.drawText(UI_10_FONT_ID, SIDE, y + 8, "Hvem skal være med? Du er alltid med.");
  y += 50;
  rows.assign(members.size(), Box{});
  for (size_t i = 0; i < members.size(); i++) {
    rows[i] = {0, y, width, ROW_H};
    if (static_cast<int>(i) == focus) renderer.fillRect(0, y + 1, 6, ROW_H - 2);
    const bool on = chosen & mesh::memberBit(members[i]);
    const int box = 40;
    const int bx = SIDE, by = y + (ROW_H - box) / 2;
    if (on) {
      renderer.fillRoundedRect(bx, by, box, box, 8, Color::Black);
      renderer.drawLine(bx + 9, by + 21, bx + 17, by + 29, 3, false);
      renderer.drawLine(bx + 17, by + 29, bx + 31, by + 11, 3, false);
    } else {
      renderer.drawRoundedRect(bx, by, box, box, 2, 8, true);
    }
    renderer.drawText(UI_12_FONT_ID, bx + box + 20, y + 26, node().group().name(members[i]), true,
                      EpdFontFamily::BOLD);
    y += ROW_H;
    renderer.drawLine(SIDE, y, width - SIDE, y);
  }
  y += 30;
  doneBox = {SIDE, y, width - 2 * SIDE, 70};
  const bool focused = focus == static_cast<int>(members.size());
  renderer.fillRoundedRect(doneBox.x, doneBox.y, doneBox.w, doneBox.h, 14, Color::Black);
  if (focused) renderer.drawRoundedRect(doneBox.x + 6, doneBox.y + 6, doneBox.w - 12, doneBox.h - 12, 2, 10, false);
  const char* label = "Skriv til dem";
  const int lw = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, doneBox.x + (doneBox.w - lw) / 2, doneBox.y + 20, label, false, EpdFontFamily::BOLD);
  if (problem) renderer.drawCenteredText(UI_10_FONT_ID, doneBox.y + doneBox.h + 24, problem);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// --- A group: who is in it, Fjern, Forlat gruppe ------------------------------------

GroupInfoActivity::GroupInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const uint8_t conversation)
    : Activity("GroupInfo", renderer, mappedInput), conversation(conversation) {}

void GroupInfoActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void GroupInfoActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.replaceActivity(std::make_unique<ChatActivity>(renderer, mappedInput, conversation));
    return;
  }
  int x = 0, y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) {
    if (node().changes() != shownChanges) requestUpdate();
    return;
  }
  // Every button asks once more before it acts.
  for (size_t i = 0; i < removeBoxes.size(); i++) {
    if (!removeBoxes[i].second.contains(x, y)) continue;
    const uint8_t member = removeBoxes[i].first;
    if (armed == member) {
      node().removeFromGroup(conversation, member);
      armed = 0;
    } else {
      armed = member;
    }
    requestUpdate();
    return;
  }
  if (leaveBox.contains(x, y)) {
    if (armed == LEAVE) {
      node().leaveGroup(conversation);
      activityManager.replaceActivity(std::make_unique<MessagesActivity>(renderer, mappedInput));
      return;
    }
    armed = LEAVE;
    requestUpdate();
    return;
  }
  armed = 0;
  requestUpdate();
}

void GroupInfoActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  shownChanges = node().changes();
  const mesh::ChatGroup& g = node().groups().at(conversation - mesh::CONV_GROUPS);
  const auto& group = node().group();
  renderer.clearScreen();
  int y = meshui::drawTitle(renderer, meshui::conversationName(conversation).c_str());
  renderer.drawText(SMALL_FONT_ID, SIDE, y + 8, "MEDLEMMER", true, EpdFontFamily::BOLD);
  y += 40;
  removeBoxes.clear();
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    if (!(g.members & mesh::memberBit(n))) continue;
    const std::string who = n == group.me ? std::string(group.name(n)) + " (deg)" : group.name(n);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 22, who.c_str(), true, EpdFontFamily::BOLD);
    if (n != group.me && !g.left) {
      // "Fjern": for a lost unit. The rest get a new key.
      Box b{width - SIDE - 150, y + 12, 150, 52};
      const char* label = armed == n ? "Sikker?" : "Fjern";
      if (armed == n) {
        renderer.fillRoundedRect(b.x, b.y, b.w, b.h, 12, Color::Black);
      } else {
        renderer.drawRoundedRect(b.x, b.y, b.w, b.h, 2, 12, true);
      }
      const int lw = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, b.x + (b.w - lw) / 2, b.y + 14, label, armed != n, EpdFontFamily::BOLD);
      removeBoxes.emplace_back(n, b);
    }
    y += ROW_H - 8;
    renderer.drawLine(SIDE, y, width - SIDE, y);
  }
  y += 30;
  if (g.left) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Du har forlatt gruppen. Meldingene fra før kan du fortsatt lese.");
    leaveBox = {};
  } else {
    leaveBox = {SIDE, y, width - 2 * SIDE, 64};
    const char* label = armed == LEAVE ? "Sikker? Trykk igjen" : "Forlat gruppe";
    renderer.drawRoundedRect(leaveBox.x, leaveBox.y, leaveBox.w, leaveBox.h, 3, 14, true);
    const int lw = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, leaveBox.x + (leaveBox.w - lw) / 2, leaveBox.y + 18, label, true,
                      EpdFontFamily::BOLD);
    y += 90;
    const auto lines = renderer.wrappedText(SMALL_FONT_ID,
                                            "Den som går ut eller fjernes, kan lese det som ble skrevet før, "
                                            "men ikke det som kommer etterpå.",
                                            width - 2 * SIDE, 3);
    for (const auto& line : lines) {
      renderer.drawText(SMALL_FONT_ID, SIDE, y, line.c_str());
      y += renderer.getLineHeight(SMALL_FONT_ID);
    }
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
