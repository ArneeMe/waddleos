#include "WaddleWifi.h"

#include <Logging.h>
#include <WiFi.h>

#include "WaddlePrivacy.h"
#include "core/WifiCredentialStore.h"
#include "core/activities/RenderLock.h"

namespace waddle::wifi {

bool joinSaved(Activity&) { return joinSaved(); }

bool joinSaved() {
  // The store is only loaded by the Wi-Fi screens, so load it here too.
  {
    RenderLock lock;
    WIFI_STORE.loadFromFile();
  }
  auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred && WIFI_STORE.getCredentialCount() > 0) cred = WIFI_STORE.getCredentialAt(0);
  if (!cred) {
    LOG_ERR("WIFI", "no saved Wi-Fi network");
    return false;
  }
  // Same sequence as WifiSelectionActivity::attemptConnection().
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setHostname(privacy::HOSTNAME);
  LOG_INF("WIFI", "joining %s", cred->ssid.c_str());
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  return true;
}

void off() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

}  // namespace waddle::wifi
