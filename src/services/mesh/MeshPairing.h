#pragma once

// The unit's side of the setup code (MeshCrypto.h, pairCode): a computer
// (`waddle mesh`) or the member's phone commits to a one-time key, the unit
// answers with a fresh nonce, the other side reveals, and both show the same six
// digits. After that, bodies sealed by that key open here, and answers are sealed
// for it. No web server here: MeshWebApi.cpp and the simulator call these.

#include <cstdint>
#include <string>

namespace mesh::pairing {

// The commitment in (hex); my nonce out (hex), or "" when it is not one.
std::string start(const std::string& commitHex);
// The key and nonce, revealed (hex): true when they match the commitment.
bool reveal(const std::string& toolHex, const std::string& nonceHex);
// The six digits for the screen, or nullptr before a reveal.
const char* code();
// A body that key sealed (nonce byte 2), opened; "" when it is not from it.
std::string open(const std::string& toolHex, const std::string& boxHex);
// plain sealed for that key with this nonce byte, as hex.
std::string seal(const std::string& plain, uint8_t nonceByte);

}  // namespace mesh::pairing
