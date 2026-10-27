#pragma once

#include <esp_partition.h>

// Rollback for a bad update, done by the firmware itself (the factory
// bootloader's own rollback cannot be relied on).
//
// Before an update switches to the new build, the old build writes down where
// it lives. The new build is "on trial" until it has run for a minute. If it
// crashes (panic or watchdog) twice before that, it switches back to the old
// build and restarts. A build that hangs without crashing stays put: that
// needs a hand.
namespace update_trial {

// Called by the update just before switching to the new build.
void start(const esp_partition_t* from);
// Called first thing in setup(): rolls back after repeated crashes.
void onBoot();
// Called from loop(): confirms the build once it has run for a minute.
void tick();
// True when the last update was rolled back (until the next update).
bool rolledBack();

}  // namespace update_trial
