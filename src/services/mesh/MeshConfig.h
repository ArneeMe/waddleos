#pragma once

// Who this unit is and who is in its group.
//
// The unit makes its own keys on first start and keeps them in flash (NVS):
// the X25519 secret for DMs never leaves the unit; the sender key is handed to
// the other members at setup. `waddle mesh setup` sends the group (the others'
// keys, the server) through the file transfer web server; it is kept in flash
// too, not on the SD card, so taking the card gives away no keys.

#include <MeshCrypto.h>

#include <cstdint>
#include <string>

namespace mesh {

struct Identity {
  uint8_t secret[KEY_SIZE];
  uint8_t publicKey[KEY_SIZE];
  uint8_t senderKey[KEY_SIZE];  // my group sending key
};

struct Member {
  bool present = false;
  char name[24] = "";
  uint8_t publicKey[KEY_SIZE] = {};
  uint8_t senderKey[KEY_SIZE] = {};
  uint8_t pairKey[KEY_SIZE] = {};  // our DM key, derived on load
};

struct Group {
  bool configured = false;
  uint8_t id = 0;
  uint8_t me = 0;  // my member number, 1-4
  Member members[MAX_MEMBERS];
  std::string serverUrl;    // "https://home.example:8443", empty = no Wi-Fi server
  std::string serverToken;  // this unit's key for the server
  std::string serverCert;   // the server's certificate (PEM), the only one trusted
  uint32_t counterFloor = 0;  // message counters start above this (unix time of setup)

  Member* member(uint8_t number) { return isMember(number) ? &members[number - 1] : nullptr; }
  const Member* member(uint8_t number) const { return isMember(number) ? &members[number - 1] : nullptr; }
  const char* name(uint8_t number) const;
  // Member bits of everyone in the mesh.
  uint8_t everyone() const;
};

// Where setups before keys moved to flash left the group; moved on start.
constexpr const char* OLD_GROUP_PATH = "/.mesh/group.json";

// Loads the unit's keys, making them on the very first start.
const Identity& identity();
// Makes new keys and forgets the group (`waddle mesh reset-keys`, after a
// unit is lost). The unit must be set up again.
void resetIdentity();
// The end of the reserved counter range saved in flash (0 on a new unit).
uint32_t savedCounterLimit();
void saveCounterLimit(uint32_t limit);

// Reads the group from flash and derives the DM keys. False when there is no
// valid group yet (the unit then stays quiet on the radio).
bool loadGroup(Group& group);
// Checks and saves a group JSON (from the setup tool). Returns an error text, or nullptr.
const char* saveGroupJson(const char* json, size_t len);

// Hex helpers for keys in JSON.
std::string toHex(const uint8_t* data, size_t len);
bool fromHex(const char* hex, uint8_t* out, size_t len);

}  // namespace mesh
