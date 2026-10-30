#include "MeshPlay.h"

#include <cstdio>
#include <cstdlib>

namespace mesh::play {

namespace {

constexpr char SEP = '\x1f';  // between a poll's question and its options
constexpr size_t REF_SIZE = 10;  // sender (2 hex) + counter (8 hex)

std::string refText(const Ref r) {
  char s[REF_SIZE + 1];
  snprintf(s, sizeof(s), "%02x%08lx", r.sender, static_cast<unsigned long>(r.counter));
  return s;
}

// `digits` lowercase hex digits at text[at]; false for anything else.
bool hex(const std::string& text, const size_t at, const size_t digits, uint32_t& out) {
  if (text.size() < at + digits) return false;
  out = 0;
  for (size_t i = at; i < at + digits; i++) {
    const char c = text[i];
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (v < 0) return false;
    out = out << 4 | uint32_t(v);
  }
  return true;
}

bool readRef(const std::string& text, Ref& out) {
  uint32_t sender = 0;
  if (!hex(text, 2, 2, sender) || !hex(text, 4, 8, out.counter)) return false;
  out.sender = uint8_t(sender);
  return isDevice(out.sender);
}

bool validOption(const std::string& s) { return !s.empty() && s.size() <= MAX_OPTION && s.find(SEP) == std::string::npos; }

}  // namespace

bool isReply(const std::string& text) {
  return text.size() >= 2 && text[0] == MARK && (text[1] == char(Kind::Vote) || text[1] == char(Kind::Move));
}

bool read(const std::string& text, Play& out) {
  if (!isPlay(text) || text.size() < 2) return false;
  out = Play{};
  out.kind = Kind(text[1]);
  switch (out.kind) {
    case Kind::Poll: {
      size_t at = 2;
      bool first = true;
      while (true) {
        const size_t end = text.find(SEP, at);
        const std::string part = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (first) {
          out.question = part;
          first = false;
        } else {
          out.options.push_back(part);
        }
        if (end == std::string::npos) break;
        at = end + 1;
      }
      if (out.question.empty() || out.question.size() > MAX_QUESTION) return false;
      if (out.options.size() < 2 || out.options.size() > MAX_OPTIONS) return false;
      for (const auto& o : out.options) {
        if (!validOption(o)) return false;
      }
      return true;
    }
    case Kind::Vote: {
      uint32_t option = 0;
      if (text.size() != 2 + REF_SIZE + 1 || !readRef(text, out.of) || !hex(text, 2 + REF_SIZE, 1, option)) return false;
      out.option = uint8_t(option);
      return out.option < MAX_OPTIONS;
    }
    case Kind::Game:
      out.game = Game(text.size() == 3 ? text[2] : 0);
      return out.game == Game::FourInRow;
    case Kind::Move: {
      uint32_t number = 0;
      if (!readRef(text, out.of) || !hex(text, 2 + REF_SIZE, 4, number) || number == 0) return false;
      out.number = uint16_t(number);
      out.move = text.substr(2 + REF_SIZE + 4);
      return !out.move.empty();
    }
  }
  return false;
}

std::string writePoll(const std::string& question, const std::vector<std::string>& options) {
  if (question.empty() || question.size() > MAX_QUESTION || question.find(SEP) != std::string::npos) return "";
  if (options.size() < 2 || options.size() > MAX_OPTIONS) return "";
  std::string text{MARK, char(Kind::Poll)};
  text += question;
  for (const auto& o : options) {
    if (!validOption(o)) return "";
    text += SEP;
    text += o;
  }
  return text;
}

std::string writeVote(const Ref poll, const uint8_t option) {
  std::string text{MARK, char(Kind::Vote)};
  text += refText(poll);
  text += "0123456789abcdef"[option & 0xF];
  return text;
}

std::string writeGame(const Game game) { return std::string{MARK, char(Kind::Game), char(game)}; }

std::string writeMove(const Ref game, const uint16_t number, const std::string& move) {
  char n[5];
  snprintf(n, sizeof(n), "%04x", number);
  return std::string{MARK, char(Kind::Move)} + refText(game) + n + move;
}

std::string preview(const std::string& text) {
  Play p;
  if (!read(text, p)) return "(trenger nyere versjon)";
  switch (p.kind) {
    case Kind::Poll:
      return "Avstemning: " + p.question;
    case Kind::Vote:
      return "stemte";
    case Kind::Game:
      return "utfordrer til fire på rad";
    case Kind::Move:
      return "gjorde et trekk";
  }
  return "";
}

Tally tally(const Ref poll, const std::vector<Entry>& entries) {
  Tally t;
  for (const auto& e : entries) {
    Play p;
    if (!e.text || !read(*e.text, p) || p.kind != Kind::Vote || !(p.of == poll)) continue;
    const Members who = memberBit(person(e.sender));
    for (auto& v : t.votes) v &= ~who;
    t.votes[p.option] |= who;
  }
  return t;
}

// --- Fire på rad ---

bool FourInRow::canDrop(const int col) const {
  return winner == 0 && col >= 0 && col < COLS && cell[ROWS - 1][col] == 0;
}

bool FourInRow::drop(const int col) {
  if (!canDrop(col)) return false;
  int row = 0;
  while (cell[row][col]) row++;
  const int player = turn();
  cell[row][col] = uint8_t(player);
  moves++;
  lastCol = col;
  // Four in a line through the new piece, in any of the four directions.
  static constexpr int DIRS[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
  for (const auto& d : DIRS) {
    int run = 1;
    for (const int sign : {1, -1}) {
      int r = row + sign * d[0], c = col + sign * d[1];
      while (r >= 0 && r < ROWS && c >= 0 && c < COLS && cell[r][c] == player) {
        run++;
        r += sign * d[0];
        c += sign * d[1];
      }
    }
    if (run >= 4) winner = player;
  }
  if (!winner && moves == ROWS * COLS) winner = 3;
  return true;
}

uint8_t GameState::toMove() const { return board.turn() == 1 ? opponent : challenger; }

int GameState::playerOf(const uint8_t who) const {
  if (who == 0) return 0;
  if (who == opponent) return 1;
  if (who == challenger) return 2;
  return 0;
}

bool replay(const Ref game, const std::vector<Entry>& entries, GameState& out) {
  out = GameState{};
  bool found = false;
  std::vector<Play> moves;
  std::vector<uint8_t> movers;
  for (const auto& e : entries) {
    Play p;
    if (!e.text || !read(*e.text, p)) continue;
    if (p.kind == Kind::Game && e.sender == game.sender && e.counter == game.counter) {
      found = true;
      out.game = p.game;
      out.challenger = person(e.sender);
    } else if (p.kind == Kind::Move && p.of == game) {
      moves.push_back(p);
      movers.push_back(person(e.sender));
    }
  }
  if (!found) return false;
  // Moves can arrive out of order: take move n + 1 wherever it is, until none
  // is left. The first valid one wins (both of a person's devices may answer).
  for (bool progress = true; progress && !out.board.winner;) {
    progress = false;
    for (size_t i = 0; i < moves.size(); i++) {
      if (moves[i].number != out.board.moves + 1) continue;
      const uint8_t who = movers[i];
      if (out.board.moves == 0) {
        if (who == out.challenger) continue;
      } else if (who != out.toMove()) {
        continue;
      }
      const int col = moves[i].move.size() == 1 ? moves[i].move[0] - '0' : -1;
      if (!out.board.drop(col)) continue;
      if (out.board.moves == 1) out.opponent = who;
      progress = true;
      break;
    }
  }
  return true;
}

bool canMove(const GameState& s, const uint8_t who) {
  if (s.board.winner || !who) return false;
  const uint8_t next = s.toMove();
  return next == who || (next == 0 && s.challenger != who);
}

std::string status(const GameState& s, const uint8_t me, const std::function<std::string(uint8_t)>& nameOf) {
  if (s.board.winner == 3) return "Uavgjort";
  if (s.board.winner) {
    const uint8_t who = s.board.winner == 1 ? s.opponent : s.challenger;
    return who == me ? "Du vant!" : nameOf(who) + " vant!";
  }
  const uint8_t next = s.toMove();
  if (next == 0) return s.challenger == me ? "Venter på at noen tar utfordringen" : "Trykk for å ta utfordringen";
  return next == me ? "Din tur" : "Venter på " + nameOf(next);
}

}  // namespace mesh::play
