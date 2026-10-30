#pragma once

// Polls and Fire på rad in a conversation (lib/WaddleMesh MeshPlay.h): their
// cards in the chat, and the game's own screen.

#include <MeshPlay.h>

#include <string>
#include <vector>

#include "core/activities/Activity.h"
#include "services/mesh/MessageStore.h"

class GfxRenderer;

namespace playui {

// Something on a card that can be tapped: an option of a poll, or a game.
struct Hit {
  int x = 0, y = 0, w = 0, h = 0;
  mesh::play::Ref ref;
  int option = -1;  // the poll option; -1 for a game
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// The conversation's play messages (MessageStore::Only::Plays) for replaying.
// The entries point into `plays`, which must outlive them.
std::vector<mesh::play::Entry> entries(const std::vector<mesh::ChatMessage>& plays);

// The card's height for a poll or a game, 0 for anything else.
int cardHeight(const GfxRenderer& renderer, const mesh::ChatMessage& m, int width);
// Draws the card with its top at y and adds what can be tapped to hits.
void drawCard(const GfxRenderer& renderer, const mesh::ChatMessage& m, const std::vector<mesh::play::Entry>& plays,
              int x, int y, int width, std::vector<Hit>& hits);

// "Din tur", "Venter på Kari", "Kari vant!" ...
std::string gameStatus(const mesh::play::GameState& state);

}  // namespace playui

// Fire på rad, full screen: tap a column, then "Slipp her".
class FourInRowActivity final : public Activity {
 public:
  FourInRowActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t conversation,
                    mesh::play::Ref game);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  uint8_t conversation;
  mesh::play::Ref game;
  mesh::play::GameState state;
  bool known = false;  // the challenge is in the conversation
  int column = 3;      // where the next piece goes
  uint32_t shownChanges = 0;
  int boardX = 0, boardY = 0, cell = 1;
  int dropY = 0, dropH = 0;  // the "Slipp her" button

  void reload();
  bool myTurn() const;
  void drop();
};
