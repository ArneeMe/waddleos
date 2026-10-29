#include "MeshControl.h"

#include <cstring>

namespace mesh {

namespace {
constexpr uint8_t NEW_GROUP = 1;               // invitation flags bit 0
constexpr size_t INVITE_SIZE = 10 + KEY_SIZE;  // 0, 'G', id (2), key number, flags, members (4), key; then the name
constexpr size_t SENDER_KEY_SIZE = 3 + KEY_SIZE;
constexpr size_t SHORT_SIZE = 5;  // 0, kind, id (2), one byte

void putId(std::string& out, const uint16_t id) {
  out += char(id & 0xFF);
  out += char(id >> 8);
}
uint16_t readId(const std::string& text) { return uint16_t(uint8_t(text[2]) | (uint8_t(text[3]) << 8)); }
}  // namespace

std::string writeControl(const ControlMsg& c) {
  std::string out{'\0', char(c.kind)};
  switch (c.kind) {
    case Control::SenderKey:
      out += char(c.keyNumber);
      out.append(reinterpret_cast<const char*>(c.key), KEY_SIZE);
      break;
    case Control::Invite:
      putId(out, c.groupId);
      out += char(c.keyNumber);
      out += char(c.fresh ? NEW_GROUP : 0);
      for (int i = 0; i < 4; i++) out += char((c.members >> (8 * i)) & 0xFF);
      out.append(reinterpret_cast<const char*>(c.key), KEY_SIZE);
      out += c.name;
      break;
    case Control::Leave:
      putId(out, c.groupId);
      out += '\0';
      break;
    case Control::AskKey:
      putId(out, c.groupId);
      out += char(c.keyNumber);
      break;
  }
  return out;
}

bool readControl(const std::string& text, ControlMsg& out) {
  if (text.size() < 2 || !isControl(text)) return false;
  out = ControlMsg{};
  out.kind = Control(text[1]);
  switch (out.kind) {
    case Control::SenderKey:
      if (text.size() < SENDER_KEY_SIZE) return false;
      out.keyNumber = uint8_t(text[2]);
      memcpy(out.key, &text[3], KEY_SIZE);
      return true;
    case Control::Invite:
      if (text.size() < INVITE_SIZE) return false;
      out.groupId = readId(text);
      out.keyNumber = uint8_t(text[4]);
      out.fresh = (uint8_t(text[5]) & NEW_GROUP) != 0;
      for (int i = 0; i < 4; i++) out.members |= Members(uint8_t(text[6 + i])) << (8 * i);
      memcpy(out.key, &text[10], KEY_SIZE);
      out.name = text.substr(INVITE_SIZE);
      return true;
    case Control::Leave:
    case Control::AskKey:
      if (text.size() < SHORT_SIZE) return false;
      out.groupId = readId(text);
      if (out.kind == Control::AskKey) out.keyNumber = uint8_t(text[4]);
      return true;
  }
  return false;
}

}  // namespace mesh
