#pragma once

class Activity;

namespace waddle::wifi {

// Starts joining the last used saved network, or the first saved one, with the
// same steps as the Wi-Fi screen. Returns false when no network is saved.
// Poll WiFi.status() for the result.
bool joinSaved(Activity& activity);
// The same, for background services that have no screen.
bool joinSaved();

// Turns Wi-Fi fully off.
void off();

}  // namespace waddle::wifi
