#include "PlayUi.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>

#include "MeshUi.h"
#include "MessagesActivity.h"
#include "core/MappedInputManager.h"
#include "core/fontIds.h"
#include "services/mesh/MeshNode.h"

using mesh::play::Entry;
using mesh::play::GameState;
using mesh::play::Kind;
using mesh::play::Play;
using mesh::play::Ref;

namespace {

constexpr int PAD = 16;
constexpr int OPTION_H = 64;
constexpr int OPTION_GAP = 8;
constexpr int MINI = 22;  // a cell of the board on the chat card

mesh::Node& node() { return mesh::Node::get(); }
uint8_t me() { return node().network().me; }

std::string nameOf(const uint8_t person) { return person == me() ? "Du" : node().network().name(person); }

// "Kari og Ola", "Kari, Ola og Per".
std::string names(const mesh::Members who) {
  std::vector<std::string> list;
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    if (who & mesh::memberBit(n)) list.push_back(nameOf(n));
  }
  std::string out;
  for (size_t i = 0; i < list.size(); i++) {
    if (i) out += i + 1 == list.size() ? " og " : ", ";
    out += list[i];
  }
  return out;
}

// A piece: player 1 is black, player 2 a ring.
void drawPiece(const GfxRenderer& renderer, const int x, const int y, const int size, const int player) {
  if (player == 1) {
    renderer.fillRoundedRect(x, y, size, size, size / 2, Color::Black);
  } else if (player == 2) {
    renderer.drawRoundedRect(x, y, size, size, std::max(2, size / 7), size / 2, true);
  } else {
    renderer.drawRoundedRect(x, y, size, size, 1, size / 2, true);
  }
}

void drawBoard(const GfxRenderer& renderer, const mesh::play::FourInRow& board, const int x, const int y,
               const int cell) {
  using B = mesh::play::FourInRow;
  renderer.drawRoundedRect(x - 4, y - 4, B::COLS * cell + 8, B::ROWS * cell + 8, 2, 8, true);
  const int gap = std::max(2, cell / 10);
  for (int r = 0; r < B::ROWS; r++) {
    for (int c = 0; c < B::COLS; c++) {
      const int px = x + c * cell + gap, py = y + (B::ROWS - 1 - r) * cell + gap;
      drawPiece(renderer, px, py, cell - 2 * gap, board.cell[r][c]);
    }
  }
  // A dot on the newest piece.
  if (board.lastCol >= 0) {
    int r = B::ROWS - 1;
    while (r > 0 && !board.cell[r][board.lastCol]) r--;
    const int dot = std::max(4, cell / 5);
    const int cx = x + board.lastCol * cell + cell / 2, cy = y + (B::ROWS - 1 - r) * cell + cell / 2;
    const bool onBlack = board.cell[r][board.lastCol] == 1;
    renderer.fillRoundedRect(cx - dot / 2, cy - dot / 2, dot, dot, dot / 2, onBlack ? Color::White : Color::Black);
  }
}

int pollHeight(const GfxRenderer& renderer, const Play& p, const int width) {
  const int lines = int(renderer.wrappedText(UI_12_FONT_ID, p.question.c_str(), width - 2 * PAD, 3, EpdFontFamily::BOLD).size());
  return PAD + lines * renderer.getLineHeight(UI_12_FONT_ID) + 8 + renderer.getLineHeight(SMALL_FONT_ID) + 12 +
         int(p.options.size()) * (OPTION_H + OPTION_GAP) + PAD - OPTION_GAP;
}

int gameHeight() { return PAD + mesh::play::FourInRow::ROWS * MINI + 8 + PAD; }

}  // namespace

namespace playui {

std::vector<Entry> entries(const std::vector<mesh::ChatMessage>& plays) {
  std::vector<Entry> out;
  out.reserve(plays.size());
  for (const auto& m : plays) out.push_back({m.sender, m.counter, &m.text});
  return out;
}

int cardHeight(const GfxRenderer& renderer, const mesh::ChatMessage& m, const int width) {
  Play p;
  if (!mesh::play::read(m.text, p)) return 0;
  if (p.kind == Kind::Poll) return pollHeight(renderer, p, width);
  if (p.kind == Kind::Game) return gameHeight();
  return 0;
}

std::string gameStatus(const GameState& s) { return mesh::play::status(s, me(), nameOf); }

void drawCard(const GfxRenderer& renderer, const mesh::ChatMessage& m, const std::vector<Entry>& plays, const int x,
              const int y, const int width, std::vector<Hit>& hits) {
  Play p;
  if (!mesh::play::read(m.text, p)) return;
  const Ref ref{m.sender, m.counter};
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  if (p.kind == Kind::Poll) {
    renderer.drawRoundedRect(x, y, width, pollHeight(renderer, p, width), 2, 16, true);
    int ty = y + PAD;
    for (const auto& l : renderer.wrappedText(UI_12_FONT_ID, p.question.c_str(), width - 2 * PAD, 3, EpdFontFamily::BOLD)) {
      renderer.drawText(UI_12_FONT_ID, x + PAD, ty, l.c_str(), true, EpdFontFamily::BOLD);
      ty += lineH;
    }
    const auto t = mesh::play::tally(ref, plays);
    mesh::Members voted = 0;
    for (size_t i = 0; i < p.options.size(); i++) voted |= t.votes[i];
    const int count = mesh::memberCount(voted);
    char head[48];
    if (count) {
      snprintf(head, sizeof(head), "Avstemning · %d har stemt", count);
    } else {
      snprintf(head, sizeof(head), "Avstemning · trykk for å stemme");
    }
    renderer.drawText(SMALL_FONT_ID, x + PAD, ty + 8, head);
    ty += 8 + renderer.getLineHeight(SMALL_FONT_ID) + 12;
    for (size_t i = 0; i < p.options.size(); i++) {
      const bool mine = t.votes[i] & mesh::memberBit(me());
      const int ox = x + PAD, ow = width - 2 * PAD;
      if (mine) {
        renderer.fillRoundedRect(ox, ty, ow, OPTION_H, 12, Color::Black);
      } else {
        renderer.drawRoundedRect(ox, ty, ow, OPTION_H, 2, 12, true);
      }
      char n[8];
      snprintf(n, sizeof(n), "%d", mesh::memberCount(t.votes[i]));
      const int nw = renderer.getTextWidth(UI_12_FONT_ID, n, EpdFontFamily::BOLD);
      const std::string who = names(t.votes[i]);
      const int textY = who.empty() ? ty + (OPTION_H - lineH) / 2 : ty + 6;
      renderer.drawText(UI_12_FONT_ID, ox + 14, textY,
                        renderer.truncatedText(UI_12_FONT_ID, p.options[i].c_str(), ow - 40 - nw).c_str(), !mine);
      renderer.drawText(UI_12_FONT_ID, ox + ow - 14 - nw, ty + (OPTION_H - lineH) / 2, n, !mine, EpdFontFamily::BOLD);
      if (!who.empty()) {
        renderer.drawText(SMALL_FONT_ID, ox + 14, ty + 6 + lineH,
                          renderer.truncatedText(SMALL_FONT_ID, who.c_str(), ow - 40 - nw).c_str(), !mine);
      }
      hits.push_back({ox, ty, ow, OPTION_H, ref, int(i)});
      ty += OPTION_H + OPTION_GAP;
    }
    return;
  }
  if (p.kind == Kind::Game) {
    GameState s;
    mesh::play::replay(ref, plays, s);
    const int h = gameHeight();
    renderer.drawRoundedRect(x, y, width, h, 2, 16, true);
    drawBoard(renderer, s.board, x + PAD + 4, y + PAD + 4, MINI);
    const int tx = x + PAD + mesh::play::FourInRow::COLS * MINI + 28;
    const int tw = x + width - PAD - tx;
    renderer.drawText(UI_12_FONT_ID, tx, y + PAD, "Fire på rad", true, EpdFontFamily::BOLD);
    const std::string vs = s.opponent ? nameOf(s.opponent) + " mot " + nameOf(s.challenger)
                                      : nameOf(s.challenger) + " utfordrer";
    renderer.drawText(UI_10_FONT_ID, tx, y + PAD + lineH + 4, renderer.truncatedText(UI_10_FONT_ID, vs.c_str(), tw).c_str());
    int ty = y + PAD + 2 * lineH + 16;
    for (const auto& l : renderer.wrappedText(UI_12_FONT_ID, gameStatus(s).c_str(), tw, 2, EpdFontFamily::BOLD)) {
      renderer.drawText(UI_12_FONT_ID, tx, ty, l.c_str(), true, EpdFontFamily::BOLD);
      ty += lineH;
    }
    hits.push_back({x, y, width, h, ref, -1});
  }
}

}  // namespace playui

// --- Fire på rad ---------------------------------------------------------------

FourInRowActivity::FourInRowActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const uint8_t conversation, const Ref game)
    : Activity("FourInRow", renderer, mappedInput), conversation(conversation), game(game) {}

void FourInRowActivity::reload() {
  const auto plays = node().store().load(conversation, 400, mesh::MessageStore::Only::Plays);
  GameState loaded;
  const bool found = mesh::play::replay(game, playui::entries(plays), loaded);
  RenderLock lock(*this);
  state = loaded;
  known = found;
}

void FourInRowActivity::onEnter() {
  Activity::onEnter();
  reload();
  requestUpdate();
}

bool FourInRowActivity::myTurn() const { return known && mesh::play::canMove(state, me()); }

void FourInRowActivity::drop() {
  if (!myTurn() || !state.board.canDrop(column)) return;
  const auto text = mesh::play::writeMove(game, uint16_t(state.board.moves + 1), std::string(1, char('0' + column)));
  if (node().sendText(conversation, text)) reload();
  requestUpdate();
}

void FourInRowActivity::loop() {
  using Button = MappedInputManager::Button;
  using B = mesh::play::FourInRow;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.replaceActivity(std::make_unique<ChatActivity>(renderer, mappedInput, conversation));
    return;
  }
  if (mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::Up)) {
    column = (column + B::COLS - 1) % B::COLS;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Right) || mappedInput.wasReleased(Button::Down)) {
    column = (column + 1) % B::COLS;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    drop();
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    // A tap on the board (or just above it) picks the column; the button drops.
    if (dropH && y >= dropY && y < dropY + dropH) {
      drop();
      return;
    }
    if (y >= boardY - cell && y < boardY + B::ROWS * cell && x >= boardX && x < boardX + B::COLS * cell) {
      column = (x - boardX) / cell;
      requestUpdate();
    }
  }
  if (node().changes() != shownChanges) {
    shownChanges = node().changes();
    reload();
    requestUpdate();
  }
}

void FourInRowActivity::render(RenderLock&&) {
  using B = mesh::play::FourInRow;
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  int y = meshui::drawTitle(renderer, "Fire på rad");
  if (!known) {
    renderer.drawCenteredText(UI_12_FONT_ID, y + 60, "Fant ikke spillet");
    renderer.displayBuffer();
    return;
  }

  // Who plays which piece.
  const int legend = 26;
  int lx = 20;
  const auto player = [&](const uint8_t who, const int piece) {
    drawPiece(renderer, lx, y + 20, legend, piece);
    lx += legend + 10;
    const std::string n = who ? nameOf(who) : "?";
    renderer.drawText(UI_12_FONT_ID, lx, y + 20, n.c_str(), true, EpdFontFamily::BOLD);
    lx += renderer.getTextWidth(UI_12_FONT_ID, n.c_str(), EpdFontFamily::BOLD) + 30;
  };
  player(state.opponent, 1);
  player(state.challenger, 2);
  y += 70;
  renderer.drawCenteredText(UI_12_FONT_ID, y, playui::gameStatus(state).c_str(), true, EpdFontFamily::BOLD);
  y += 60;

  cell = (width - 40) / B::COLS;
  boardX = (width - B::COLS * cell) / 2;
  // The piece waiting above the chosen column.
  const bool turn = myTurn();
  if (turn) {
    const int size = cell - 2 * std::max(2, cell / 10);
    drawPiece(renderer, boardX + column * cell + (cell - size) / 2, y, size, state.board.turn());
  }
  boardY = y + cell + 8;
  drawBoard(renderer, state.board, boardX, boardY, cell);

  dropY = boardY + B::ROWS * cell + 30;
  dropH = 0;
  if (turn) {
    dropH = 70;
    const bool can = state.board.canDrop(column);
    const char* label = can ? "Slipp her" : "Kolonnen er full";
    if (can) {
      renderer.fillRoundedRect(20, dropY, width - 40, dropH, 14, Color::Black);
    } else {
      renderer.drawRoundedRect(20, dropY, width - 40, dropH, 2, 14, true);
    }
    const int lw = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, (width - lw) / 2, dropY + 20, label, !can, EpdFontFamily::BOLD);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
