#include "UpdateTrial.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include <cstring>

#include "OtaBootSwitch.h"

namespace update_trial {

namespace {

constexpr const char* NVS_NAMESPACE = "update";
constexpr uint8_t MAX_CRASHES = 2;
constexpr unsigned long CONFIRM_AFTER_MS = 60000;

bool onTrial = false;

bool crashed(const esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

}  // namespace

void start(const esp_partition_t* from) {
  if (!from) return;
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString("from", from->label);
  prefs.putUChar("crashes", 0);
  prefs.putBool("rolledBack", false);
  prefs.end();
}

void onBoot() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  const String from = prefs.getString("from", "");
  if (from.isEmpty()) {
    prefs.end();
    return;
  }
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && from == running->label) {
    // Still (or again) the old build: the switch did not happen.
    prefs.remove("from");
    prefs.end();
    return;
  }
  uint8_t crashes = prefs.getUChar("crashes", 0);
  if (crashed(esp_reset_reason())) prefs.putUChar("crashes", ++crashes);
  if (crashes >= MAX_CRASHES) {
    const esp_partition_t* old =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, from.c_str());
    prefs.remove("from");
    prefs.putBool("rolledBack", true);
    prefs.end();
    LOG_ERR("UPDATE", "New build crashed %u times: back to %s", crashes, from.c_str());
    if (old && ota_boot::switchTo(old)) esp_restart();
    return;
  }
  prefs.end();
  onTrial = true;
  LOG_INF("UPDATE", "New build on trial (%u crashes so far)", crashes);
}

void tick() {
  if (!onTrial || millis() < CONFIRM_AFTER_MS) return;
  onTrial = false;
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.remove("from");
  prefs.remove("crashes");
  prefs.end();
  // Harmless when the bootloader has no rollback of its own.
  esp_ota_mark_app_valid_cancel_rollback();
  LOG_INF("UPDATE", "New build confirmed");
}

bool rolledBack() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  const bool back = prefs.getBool("rolledBack", false);
  prefs.end();
  return back;
}

}  // namespace update_trial
