#pragma once

// Control DMs: a DM whose text starts with byte 0 changes keys or groups instead
// of being shown (protocol/PROTOCOL.md, "Control DMs").

#include <cstdint>
#include <string>

#include "MeshCrypto.h"
#include "MeshPacket.h"

namespace mesh {

enum class Control : char {
  SenderKey = 'S',  // my sender key
  Invite = 'G',     // a group's key (and who is in it)
  Leave = 'L',      // I leave the group
  Remove = 'R',     // please take this member out (to the lowest remaining member)
  AskKey = 'K',     // please send me this key
};

struct ControlMsg {
  Control kind = Control::SenderKey;
  uint16_t groupId = 0;    // G, L, R, K (K: 0 = your sender key)
  uint8_t keyNumber = 0;   // S, G, K
  uint8_t member = 0;      // R: who to take out
  bool fresh = false;      // G: a group starting afresh (key number 1)
  Members members = 0;     // G: everyone in the group, the sender included
  uint8_t key[KEY_SIZE] = {};  // S, G
  std::string name;        // G: the group's name (may be empty)
};

inline bool isControl(const std::string& text) { return !text.empty() && text[0] == '\0'; }
// The DM text for c.
std::string writeControl(const ControlMsg& c);
// False for anything that is not a whole control DM of a known kind.
bool readControl(const std::string& text, ControlMsg& out);

}  // namespace mesh
