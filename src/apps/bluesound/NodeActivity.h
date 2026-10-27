#pragma once

#include <string>
#include <vector>

#include "BluOsClient.h"
#include "core/activities/Activity.h"

// WaddleOS: remote for a Bluesound Node. Joins the last used Wi-Fi network
// while open, shows the Node's presets as sources (Spotify, TV, ...), volume
// and play controls. Wi-Fi is turned off again on exit.
class NodeActivity final : public Activity {
 public:
  NodeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class State { CONNECTING, READY, NO_WIFI, NO_NODE };

  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  State state = State::CONNECTING;
  waddle::bluos::Client node;
  waddle::bluos::Status status;
  std::vector<waddle::bluos::Preset> presets;
  unsigned long connectStartedAt = 0;
  unsigned long lastPollAt = 0;
  unsigned long lastNoteAt = 0;
  int noteFrame = 0;  // the penguin "sings" while music plays

  // Hit areas, filled by render() for touch input.
  std::vector<Box> presetBoxes;
  Box volDownBox, volUpBox, prevBox, playBox, nextBox;

  void startWifi();
  void loadNode();
  void refreshStatus(bool force);
  int activePreset() const;
  void selectPreset(int index);
  void changeVolume(int db);
  void togglePlay();
  void skip(bool forward);
  void goHome();
};
