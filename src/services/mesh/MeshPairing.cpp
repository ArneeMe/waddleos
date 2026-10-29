#include "MeshPairing.h"

#include <MeshCrypto.h>
#include <esp_random.h>

#include <cstring>
#include <vector>

#include "MeshConfig.h"

namespace mesh::pairing {

namespace {
struct State {
  uint8_t commit[32] = {};
  uint8_t unitNonce[PAIR_NONCE_SIZE] = {};
  uint8_t tool[KEY_SIZE] = {};
  bool committed = false;
  bool paired = false;
  char code[8] = "";
};
State state;

void keyFor(uint8_t key[KEY_SIZE]) {
  const Identity& id = identity();
  boxKey(id.secret, id.publicKey, state.tool, key);
}
}  // namespace

std::string start(const std::string& commitHex) {
  if (!fromHex(commitHex.c_str(), state.commit, sizeof(state.commit))) return "";
  esp_fill_random(state.unitNonce, sizeof(state.unitNonce));
  state.committed = true;
  state.paired = false;
  return toHex(state.unitNonce, sizeof(state.unitNonce));
}

bool reveal(const std::string& toolHex, const std::string& nonceHex) {
  uint8_t tool[KEY_SIZE], toolNonce[PAIR_NONCE_SIZE], commit[32];
  if (!fromHex(toolHex.c_str(), tool, KEY_SIZE) || !fromHex(nonceHex.c_str(), toolNonce, PAIR_NONCE_SIZE)) {
    return false;
  }
  pairCommit(tool, toolNonce, commit);
  if (!state.committed || memcmp(commit, state.commit, sizeof(commit)) != 0) return false;
  state.committed = false;
  memcpy(state.tool, tool, KEY_SIZE);
  pairCode(tool, identity().publicKey, toolNonce, state.unitNonce, state.code);
  state.paired = true;
  return true;
}

const char* code() { return state.paired ? state.code : nullptr; }

std::string open(const std::string& toolHex, const std::string& boxHex) {
  uint8_t tool[KEY_SIZE];
  // Only from the key whose code the person compared on the screen.
  if (!state.paired || !fromHex(toolHex.c_str(), tool, KEY_SIZE) || memcmp(tool, state.tool, KEY_SIZE) != 0) {
    return "";
  }
  const size_t boxLen = boxHex.size() / 2;
  if (boxLen <= TAG_SIZE || boxLen > 4096) return "";
  std::vector<uint8_t> box(boxLen);
  std::string plain(boxLen - TAG_SIZE, '\0');
  uint8_t key[KEY_SIZE];
  keyFor(key);
  const bool ok = fromHex(boxHex.c_str(), box.data(), boxLen) &&
                  openBox(key, 2, box.data(), boxLen, reinterpret_cast<uint8_t*>(&plain[0]));
  memset(key, 0, sizeof(key));
  return ok ? plain : "";
}

std::string seal(const std::string& plain, const uint8_t nonceByte) {
  if (!state.paired) return "";
  std::vector<uint8_t> box(plain.size() + TAG_SIZE);
  uint8_t key[KEY_SIZE];
  keyFor(key);
  sealBox(key, nonceByte, reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), box.data());
  memset(key, 0, sizeof(key));
  return toHex(box.data(), box.size());
}

}  // namespace mesh::pairing
