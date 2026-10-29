#pragma once

// Who this unit is and which network it belongs to.
//
// The unit makes its own keys on first start and keeps them in flash (NVS):
// the X25519 secret for DMs never leaves the unit; the sender key is handed to
// the other members by DM. Joining (`waddle mesh join`, protocol/PROTOCOL.md
// "Joining") gives the unit its member number, the network key, the admin's
// public keys, the server and the signed roster. Those, and everything else
// with keys in it, are kept on the SD card in files sealed with a key only
// this unit has (SealedFile), so taking the card gives away nothing.

#include <HalStorage.h>
#include <MeshCrypto.h>
#include <MeshRoster.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mesh {

struct Identity {
  uint8_t secret[KEY_SIZE];
  uint8_t publicKey[KEY_SIZE];
  uint8_t senderKey[KEY_SIZE];  // my key for packets to everyone
  uint8_t senderNumber = 1;     // its key number (header byte 13)
};

struct Member {
  bool present = false;
  bool admin = false;
  char name[ROSTER_NAME + 1] = "";
  uint8_t publicKey[KEY_SIZE] = {};
  uint8_t pairKey[KEY_SIZE] = {};  // our DM key, derived on load
};

struct Group {  // the network
  bool configured = false;
  uint8_t id = 0;  // network id
  uint8_t me = 0;  // my member number, 1-32
  Member members[MAX_MEMBERS];
  uint8_t networkKey[KEY_SIZE] = {};
  uint8_t adminSign[SIGN_KEY_SIZE] = {};
  uint8_t adminBox[KEY_SIZE] = {};
  uint16_t rosterVersion = 0;
  std::vector<uint8_t> roster;  // the signed roster, as received
  std::string serverUrl;        // "https://home.example:8443", empty = no Wi-Fi server
  std::string serverToken;      // this unit's token for the server
  std::string serverCert;       // the server's certificate (PEM), the only one trusted
  uint32_t counterFloor = 0;    // message counters start above this

  Member* member(uint8_t number) { return isMember(number) ? &members[number - 1] : nullptr; }
  const Member* member(uint8_t number) const { return isMember(number) ? &members[number - 1] : nullptr; }
  const char* name(uint8_t number) const;
  // Member bits of everyone in the network.
  Members everyone() const;
};

// Loads the unit's keys, making them on the very first start.
const Identity& identity();
// A new sender key (number + 1), after someone left the network.
void newSenderKey();
// The end of the reserved counter range saved in flash (0 on a new unit).
uint32_t savedCounterLimit();
void saveCounterLimit(uint32_t limit);

// Reads the network from the SD card and derives the DM keys. False when the
// unit has not joined yet (it then stays quiet on the radio).
bool loadGroup(Group& group);
// A newer roster signed by the admin: takes it in and saves it. False when it
// is not newer or not signed by the admin.
bool applyRoster(Group& group, const uint8_t* data, size_t len);

// Joining. start: the link from the invitation, my name. Keeps the invitation
// and gives the sealed request (protocol/PROTOCOL.md, "The pieces in detail").
// Returns an error text, or nullptr.
const char* startJoin(const std::string& link, const std::string& name, std::string& boxHex, char code[8]);
// The admin's welcome: opens it with the kept invitation and saves the network.
const char* finishJoin(const std::string& boxHex);

// Files on the SD card sealed with a key derived from this unit's secret.
bool saveSealed(const char* path, const std::vector<uint8_t>& plain);
bool loadSealed(const char* path, std::vector<uint8_t>& plain);

// Append-only files of sealed records, same key: [length u16][nonce 12]
// [ciphertext][tag 16] per record. A record cut short by a power cut, or one that
// does not open, ends the reading there.
bool appendSealed(const char* path, const uint8_t* plain, size_t len);
class SealedLog {
 public:
  explicit SealedLog(const char* path);
  ~SealedLog();
  explicit operator bool() const { return bool(file_); }
  // The next record, false at the end.
  bool next(std::vector<uint8_t>& plain);

 private:
  HalFile file_;
  uint8_t key_[KEY_SIZE];
};

// Hex and base64url helpers.
std::string toHex(const uint8_t* data, size_t len);
bool fromHex(const char* hex, uint8_t* out, size_t len);
std::vector<uint8_t> fromBase64Url(const std::string& text);

}  // namespace mesh
