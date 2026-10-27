#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "waddle/WaddleVersion.h"
#include "waddle/WaddleDraw.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  (void)pageWidth;
  waddle::drawSplash(renderer, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, waddle::version());
  renderer.displayBuffer();
}
