#pragma once

#include <string>

#include "activities/Activity.h"

// WaddleOS: Innstillinger → Oppdater fastvare. Nothing happens until Arne taps
// "Se etter oppdatering": then it joins Wi-Fi and reads version.json from the
// waddle-latest release. If that build differs from this one, "Oppdater"
// downloads waddleos-x4pro.bin to the SD card and hands it to the normal
// firmware update screen, which checks it and asks before flashing.
class UpdateActivity final : public Activity {
 public:
  UpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::CHECKING || state == State::DOWNLOADING; }

 private:
  enum class State { IDLE, CHECKING, LATEST, AVAILABLE, DOWNLOADING, FAILED };
  enum class Action { NONE, CHECK, DOWNLOAD };

  State state = State::IDLE;
  Action pending = Action::NONE;  // what to do once Wi-Fi is up
  unsigned long connectStartedAt = 0;
  std::string latestBuild;    // short git hash of the newest build
  std::string latestVersion;  // "WaddleOS 1.42"
  std::string latestDate;   // "26. okt kl 21:22"
  std::string latestNote;   // its commit message
  std::string error;
  int percent = 0;

  // The one button on screen, filled by render() for touch input.
  int buttonX = 0, buttonY = 0, buttonW = 0, buttonH = 0;

  void press();
  void startWifi(Action action);
  void check();
  void download();
  void fail(const char* message);
};
