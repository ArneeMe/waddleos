#pragma once

// What each device (src/products/<name>/) gives the shared core: its board
// set-up, its home screen and its own background work. Every product folder has
// one Product.cpp that defines all of these; core/WaddleMain.cpp calls them.

#include <memory>
#include <string>

class Activity;
class GfxRenderer;
class MappedInputManager;
class WebServer;

namespace product {

// First thing at boot, before input, storage and display start.
void beginBoard();
// After settings, storage and the display are up, before the first screen.
void begin();
// Every pass of the main loop.
void loop();
// True while the product has work that must finish before auto sleep.
bool preventAutoSleep();

// Called after the sleep screen is drawn. Returns true when the product slept
// by itself (light sleep) and the device is now awake again; core then reopens
// the screen it slept from. Returns false to let core enter deep sleep.
bool sleep();

// Extra endpoints on the file transfer web server (the mesh unit's setup API).
void registerWebRoutes(WebServer& server);
// The firmware file in the waddle-latest release that fits this device.
const char* firmwareName();
// An extra line on the File Transfer screen ("" for none): the mesh unit's
// setup code, which `waddle mesh setup` asks you to compare.
std::string transferNote();

// The home screen.
std::unique_ptr<Activity> makeHome(GfxRenderer& renderer, MappedInputManager& input, bool cleanInitialRefresh);
// Where leaving a book goes: an activity, or nullptr for the home screen.
std::unique_ptr<Activity> afterBook(GfxRenderer& renderer, MappedInputManager& input, const std::string& bookPath);

}  // namespace product
