#include "NotesActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <ctime>

#include "apps/messages/MeshUi.h"
#include "core/MappedInputManager.h"
#include "core/activities/util/KeyboardEntryActivity.h"
#include "core/fontIds.h"

namespace {
constexpr const char* DIR = "/Notater";
constexpr int SIDE = 20;
constexpr int ROW_H = 76;
constexpr size_t MAX_NOTE = 1000;
}  // namespace

NotesActivity::NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Notes", renderer, mappedInput) {}

void NotesActivity::reload() {
  Storage.mkdir(DIR);
  std::vector<std::string> names;
  for (const auto& f : Storage.listFiles(DIR, 200)) {
    const std::string name = f.c_str();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0) names.push_back(name);
  }
  // Names are dates, so newest first is reverse order.
  std::sort(names.rbegin(), names.rend());
  std::vector<std::string> texts;
  for (const auto& n : names) {
    const String text = Storage.readFile((std::string(DIR) + "/" + n).c_str());
    texts.emplace_back(text.c_str());
  }
  RenderLock lock(*this);
  files = std::move(names);
  previews = std::move(texts);
  focus = std::min<int>(focus, static_cast<int>(files.size()));
}

void NotesActivity::onEnter() {
  Activity::onEnter();
  reload();
  requestUpdate();
}

void NotesActivity::edit(const int index) {
  const std::string initial = index >= 0 ? previews[index] : std::string();
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, index >= 0 ? "Notat" : "Nytt notat",
                                                           initial, MAX_NOTE, InputType::Text);
  if (!keyboard) return;
  startActivityForResult(std::move(keyboard), [this, index](const ActivityResult& result) {
    if (!result.isCancelled) {
      const std::string text = std::get<KeyboardResult>(result.data).text;
      std::string path;
      if (index >= 0) {
        path = std::string(DIR) + "/" + files[index];
      } else {
        const time_t now = time(nullptr);
        struct tm t {};
        localtime_r(&now, &t);
        char name[40];
        strftime(name, sizeof(name), "/Notater/%Y-%m-%d %H-%M-%S.txt", &t);
        path = name;
      }
      if (text.empty()) {
        if (index >= 0) Storage.remove(path.c_str());
      } else {
        Storage.writeFile(path.c_str(), String(text.c_str()));
      }
    }
    reload();
    requestUpdate();
  });
}

void NotesActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) {
    activityManager.goHome();
    return;
  }
  const int count = static_cast<int>(files.size()) + 1;
  if (mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::PageForward)) {
    focus = (focus + 1) % count;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::PageBack)) {
    focus = (focus + count - 1) % count;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    edit(focus - 1);
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (newBox.contains(x, y)) {
      edit(-1);
      return;
    }
    for (size_t i = 0; i < rows.size(); i++) {
      if (rows[i].contains(x, y)) {
        edit(static_cast<int>(i));
        return;
      }
    }
  }
}

void NotesActivity::render(RenderLock&&) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  renderer.clearScreen();
  int y = meshui::drawTitle(renderer, "Notater");

  newBox = {SIDE, y, width - 2 * SIDE, 60};
  if (focus == 0) {
    renderer.fillRoundedRect(newBox.x, newBox.y, newBox.w, newBox.h, 14, Color::Black);
  } else {
    renderer.drawRoundedRect(newBox.x, newBox.y, newBox.w, newBox.h, 2, 14, true);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, y + 16, "+ Nytt notat", focus != 0, EpdFontFamily::BOLD);
  y += 80;

  rows.assign(files.size(), Box{});
  for (size_t i = 0; i < files.size() && y + ROW_H < height; i++) {
    rows[i] = {0, y, width, ROW_H};
    if (static_cast<int>(i) + 1 == focus) renderer.fillRect(0, y + 1, 6, ROW_H - 2);
    // "2026-10-27 14-32-05.txt" -> "2026-10-27 14:32".
    std::string when = files[i].substr(0, 16);
    if (when.size() == 16) when[13] = ':';
    renderer.drawText(SMALL_FONT_ID, SIDE, y + 8, when.c_str());
    std::string first = previews[i].substr(0, previews[i].find('\n'));
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 32,
                      renderer.truncatedText(UI_12_FONT_ID, first.c_str(), width - 2 * SIDE).c_str());
    y += ROW_H;
    renderer.drawLine(SIDE, y, width - SIDE, y);
  }
  if (files.empty()) renderer.drawCenteredText(UI_12_FONT_ID, y + 20, "Ingen notater ennå");
  renderer.displayBuffer();
}
