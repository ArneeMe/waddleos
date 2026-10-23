#include "BluOsClient.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <utility>

namespace waddle::bluos {

namespace {

constexpr uint16_t PORT = 11000;
constexpr uint32_t TIMEOUT_MS = 3000;
constexpr size_t MAX_BODY = 8 * 1024;  // Status and Presets are about 1-2 KB

// Text between <tag> and </tag>, or empty.
std::string tagText(const std::string& xml, const char* tag) {
  const std::string open = std::string("<") + tag + ">";
  const std::string close = std::string("</") + tag + ">";
  const size_t start = xml.find(open);
  if (start == std::string::npos) return {};
  const size_t from = start + open.size();
  const size_t end = xml.find(close, from);
  return end == std::string::npos ? std::string() : xml.substr(from, end - from);
}

// Value of name="..." inside one element string, or empty.
std::string attr(const std::string& element, const char* name) {
  const std::string key = std::string(" ") + name + "=\"";
  const size_t start = element.find(key);
  if (start == std::string::npos) return {};
  const size_t from = start + key.size();
  const size_t end = element.find('"', from);
  return end == std::string::npos ? std::string() : element.substr(from, end - from);
}

// The few XML escapes BluOS uses in names and titles.
std::string unescape(std::string s) {
  const std::pair<const char*, const char*> entities[] = {
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
  for (const auto& [from, to] : entities) {
    for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + 1)) {
      s.replace(pos, strlen(from), to);
    }
  }
  return s;
}

}  // namespace

bool Client::get(const char* path, std::string* body) const {
  NetworkClient client;
  client.setTimeout(TIMEOUT_MS);
  if (!client.connect(host.c_str(), PORT, TIMEOUT_MS)) {
    LOG_ERR("BLUOS", "connect %s failed", host.c_str());
    return false;
  }
  client.printf("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host.c_str());

  std::string response;
  const uint32_t deadline = millis() + TIMEOUT_MS;
  while ((client.connected() || client.available()) && millis() < deadline && response.size() < MAX_BODY) {
    while (client.available() && response.size() < MAX_BODY) {
      response.push_back(static_cast<char>(client.read()));
    }
    delay(1);
  }
  client.stop();

  // "HTTP/1.1 200 OK"
  if (response.size() < 12 || response.compare(9, 3, "200") != 0) {
    LOG_ERR("BLUOS", "GET %s failed", path);
    return false;
  }
  if (body) {
    const size_t split = response.find("\r\n\r\n");
    *body = split == std::string::npos ? std::string() : response.substr(split + 4);
  }
  return true;
}

bool Client::status(Status& out) const {
  std::string xml;
  if (!get("/Status", &xml)) return false;
  const std::string volume = tagText(xml, "volume");
  out.volume = volume.empty() ? -1 : atoi(volume.c_str());
  out.mute = tagText(xml, "mute") == "1";
  const std::string state = tagText(xml, "state");
  out.playing = state == "play" || state == "stream";
  out.service = tagText(xml, "service");
  out.streamUrl = unescape(tagText(xml, "streamUrl"));
  out.title = unescape(tagText(xml, "title1"));
  out.artist = unescape(tagText(xml, "artist"));
  if (out.artist.empty()) out.artist = unescape(tagText(xml, "title2"));
  return true;
}

bool Client::presets(std::vector<Preset>& out) const {
  std::string xml;
  if (!get("/Presets", &xml)) return false;
  out.clear();
  for (size_t pos = xml.find("<preset "); pos != std::string::npos; pos = xml.find("<preset ", pos + 1)) {
    const size_t end = xml.find('>', pos);
    if (end == std::string::npos) break;
    const std::string element = xml.substr(pos, end - pos);
    Preset preset;
    preset.id = atoi(attr(element, "id").c_str());
    preset.name = unescape(attr(element, "name"));
    preset.url = unescape(attr(element, "url"));
    if (preset.id > 0) out.push_back(std::move(preset));
  }
  return true;
}

bool Client::playPreset(const int id) const {
  char path[32];
  snprintf(path, sizeof(path), "/Preset?id=%d", id);
  return get(path);
}

bool Client::volumeStep(const int db) const {
  char path[32];
  snprintf(path, sizeof(path), "/Volume?db=%d", db);
  return get(path);
}

bool Client::play() const { return get("/Play"); }
bool Client::pause() const { return get("/Pause"); }
bool Client::next() const { return get("/Skip"); }
bool Client::previous() const { return get("/Back"); }

}  // namespace waddle::bluos
