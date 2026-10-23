#pragma once

#include <string>
#include <vector>

// WaddleOS: minimal client for the BluOS Custom Integration API (Bluesound
// Node). Plain HTTP GET on port 11000 on the local network, XML replies, no
// account. Calls block for at most a few seconds.
namespace waddle::bluos {

struct Preset {
  int id = 0;
  std::string name;
  std::string url;
};

struct Status {
  int volume = -1;           // 0..100, -1 = unknown
  bool mute = false;
  bool playing = false;      // state is play or stream
  std::string service;       // "Spotify", "Capture", ...
  std::string streamUrl;     // matches Preset::url for inputs
  std::string title;         // song or input name
  std::string artist;
};

class Client {
 public:
  explicit Client(std::string host) : host(std::move(host)) {}

  bool status(Status& out) const;
  bool presets(std::vector<Preset>& out) const;
  bool playPreset(int id) const;
  bool volumeStep(int db) const;
  bool play() const;
  bool pause() const;
  bool next() const;
  bool previous() const;

 private:
  std::string host;
  bool get(const char* path, std::string* body = nullptr) const;
};

}  // namespace waddle::bluos
