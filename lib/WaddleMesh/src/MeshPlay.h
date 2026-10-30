#pragma once

// Polls and games in a conversation (protocol/PROTOCOL.md, "Polls and games").
// They travel as ordinary messages whose text starts with byte 1, so keys,
// acknowledgements, syncs and the phone work as for any other message. A poll
// or a challenge is the message itself; a vote or a move points at it by its
// sender and counter. What a poll counts and where a game stands come from
// replaying the conversation's play messages, so nothing else has to be synced.
// Plain C++, like MeshPacket.h, so the host tests build it too.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "MeshPacket.h"

namespace mesh::play {

constexpr char MARK = '\x01';
constexpr size_t MAX_OPTIONS = 4;
constexpr size_t MAX_QUESTION = 200;
constexpr size_t MAX_OPTION = 60;

enum class Kind : char { Poll = 'A', Vote = 'a', Game = 'S', Move = 'm' };
enum class Game : char { FourInRow = 'F' };

// A poll or a game: the device that sent it and its counter.
struct Ref {
  uint8_t sender = 0;
  uint32_t counter = 0;
  bool operator==(const Ref& o) const { return sender == o.sender && counter == o.counter; }
};

struct Play {
  Kind kind = Kind::Poll;
  std::string question;              // Poll
  std::vector<std::string> options;  // Poll: 2 to MAX_OPTIONS
  Game game = Game::FourInRow;       // Game
  Ref of;                            // Vote, Move: the poll or game
  uint8_t option = 0;                // Vote: index into the poll's options
  uint16_t number = 0;               // Move: 1 for the first move
  std::string move;                  // Move: what the game makes of it ("3": column 3)
};

inline bool isPlay(const std::string& text) { return !text.empty() && text[0] == MARK; }
// A vote or a move: shown on its poll or game, not as a message of its own.
bool isReply(const std::string& text);
// False for anything that is not a whole play message of a known kind (a game
// from a newer version, for one).
bool read(const std::string& text, Play& out);

// Empty when the poll does not fit the rules above.
std::string writePoll(const std::string& question, const std::vector<std::string>& options);
std::string writeVote(Ref poll, uint8_t option);
std::string writeGame(Game game);
std::string writeMove(Ref game, uint16_t number, const std::string& move);

// What the conversation list shows for a play message.
std::string preview(const std::string& text);

// A play message as the conversation has it, oldest first.
struct Entry {
  uint8_t sender = 0;  // device
  uint32_t counter = 0;
  const std::string* text = nullptr;
};

// Each person's newest vote counts (their unit and phone are one person).
struct Tally {
  Members votes[MAX_OPTIONS] = {};
};
Tally tally(Ref poll, const std::vector<Entry>& entries);

// Fire på rad: 7 columns, 6 rows, four in a line wins.
struct FourInRow {
  static constexpr int COLS = 7;
  static constexpr int ROWS = 6;
  uint8_t cell[ROWS][COLS] = {};  // row 0 is the bottom; 0 empty, else the player (1 or 2)
  int moves = 0;
  int lastCol = -1;
  int winner = 0;  // 0 playing, 1 or 2, 3 a draw

  // The player whose move it is: 1 makes the odd moves.
  int turn() const { return moves % 2 + 1; }
  bool canDrop(int col) const;
  bool drop(int col);
};

// A game as the moves so far leave it. The one who takes the challenge makes
// the first move and is player 1; the challenger is player 2. In a group,
// whoever moves first takes it.
struct GameState {
  Game game = Game::FourInRow;
  uint8_t challenger = 0;  // person
  uint8_t opponent = 0;    // person; 0 until someone has moved
  FourInRow board;

  // The person whose move it is, or 0 when anyone but the challenger may take it.
  uint8_t toMove() const;
  // Player 1 or 2 for a person, 0 for someone outside the game.
  int playerOf(uint8_t person) const;
};
// False when `game` is not a challenge in entries.
bool replay(Ref game, const std::vector<Entry>& entries, GameState& out);
// Whether `who` (a person) may make the next move.
bool canMove(const GameState& state, uint8_t who);
// "Din tur", "Venter på Kari", "Kari vant!" ... as `me` sees it.
std::string status(const GameState& state, uint8_t me, const std::function<std::string(uint8_t)>& nameOf);

}  // namespace mesh::play
