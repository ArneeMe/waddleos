#pragma once

class Activity;

namespace waddle::wifi {

// Starts joining the last used saved network, or the first saved one, with the
// same steps as the Wi-Fi screen. Returns false when no network is saved.
// Poll WiFi.status() for the result.
bool joinSaved(Activity& activity);
// The same, for background services that have no screen.
bool joinSaved();

// True once a join has clearly failed (the network is not in range, or the
// password was refused), so callers need not wait for their full timeout.
// startedAtMs: millis() when the join started; the first second is ignored,
// as the status from before the join can linger.
bool joinFailed(unsigned long startedAtMs);

// Turns Wi-Fi fully off.
void off();

}  // namespace waddle::wifi
