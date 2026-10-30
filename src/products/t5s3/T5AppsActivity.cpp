#include "T5AppsActivity.h"

#include <GfxRenderer.h>

#include "apps/messages/MeshUi.h"
#include "core/MappedInputManager.h"
#include "core/fontIds.h"

namespace {

constexpr int SIDE = 20;
constexpr int ROW_H = 84;
constexpr const char* LABELS[] = {"Bøker", "Nyheter"};
constexpr const char* NOTES[] = {"Biblioteket på minnekortet", "NRK, hentes når du åpner den"};

}  // namespace

T5AppsActivity::T5AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("T5Apps", renderer, mappedInput) {}

void T5AppsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void T5AppsActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasReleased(Button::Down)) {
    focus = (focus + 1) % ROWS;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Up)) {
    focus = (focus + ROWS - 1) % ROWS;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    openT5App(static_cast<Row>(focus));
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    for (int r = 0; r < ROWS; r++) {
      if (y >= boxes[r].y && y < boxes[r].y + boxes[r].h) {
        openT5App(static_cast<Row>(r));
        return;
      }
    }
  }
}

void T5AppsActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  int y = meshui::drawTitle(renderer, "Apper") + 8;
  for (int r = 0; r < ROWS; r++) {
    boxes[r] = {y, ROW_H};
    if (focus == r) renderer.fillRect(0, y, 6, ROW_H - 4, true);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 10, LABELS[r], true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, SIDE, y + 44, NOTES[r]);
    renderer.drawText(UI_12_FONT_ID, width - SIDE - 16, y + 24, ">", true, EpdFontFamily::BOLD);
    y += ROW_H;
    renderer.drawLine(SIDE, y - 2, width - SIDE, y - 2);
  }
  renderer.displayBuffer();
}
