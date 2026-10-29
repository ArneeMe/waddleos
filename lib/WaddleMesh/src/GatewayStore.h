#pragma once

// What the gateway does on its own while the Pi is away (a power cut, the
// gateway on a power bank): it keeps the MSG, POS, SOS and ACK packets it hears,
// answers sync requests and fetches like the server, sends "stored" ACKs and
// relays. When the Pi is back it hands the kept packets over and forgets them
// (protocol/PROTOCOL.md, "Gateway serial link").
//
// Plain C++: the gateway's main.cpp saves the slots and the counter to flash.

#include <cstddef>
#include <cstdint>

#include "MeshPacket.h"
#include "MeshRadioRules.h"

namespace mesh {

struct RawPacket {
  uint8_t len = 0;
  uint8_t data[MAX_PACKET] = {};
};

class GatewayStore {
 public:
  static constexpr int SLOTS = 192;      // a few days of messages for a small group
  static constexpr int MAX_ANSWER = 12;  // same limit as the server over LoRa
  // The gateway speaks as sender 0 like the server, with counters from here up,
  // so the two never use the same one.
  static constexpr uint32_t FIRST_COUNTER = 0x80000000u;

  struct Slot {
    uint32_t seq = 0;  // 0 = empty; a higher one was stored later
    RawPacket packet;
  };

  // What to do after a packet: send these, then save a slot or the counter.
  struct Result {
    int count = 0;
    RawPacket packets[MAX_ANSWER];
    int savedSlot = -1;        // write this slot to flash
    uint32_t saveCounter = 0;  // write this counter limit to flash (before sending)
  };

  // group: the mesh id (0 = not known yet: nothing is kept or answered).
  // savedCounter: the counter limit last saved to flash (0 on a new gateway).
  GatewayStore(uint8_t group, uint32_t savedCounter);

  void setGroup(uint8_t group) { group_ = group; }
  uint8_t group() const { return group_; }
  // Who is in the network (from the Pi's PING; 0 = not known: everyone counts).
  // Alone, the gateway keeps, relays and answers only their packets.
  void setMembers(Members members) { members_ = members; }
  Members members() const { return members_; }

  // A packet heard over LoRa while the Pi is away. Stores it when it is kept,
  // and fills r with what to send back: a "stored" ACK, or the answer to a
  // SYNC_REQ or FETCH.
  void handle(const uint8_t* raw, size_t len, Result& r);

  // A copy to send on (hops - 1), the first time a packet is heard. False when
  // it is not to be relayed (seen before, no hops left, a sync request).
  bool relay(const uint8_t* raw, size_t len, RawPacket& out);

  // Puts back a slot read from flash at start.
  void restore(int index, const Slot& s);
  const Slot& slot(int index) const { return slots_[index]; }
  int count() const;
  void clear();

 private:
  bool ours(const uint8_t* raw, size_t len, Header& h) const;
  int find(const Header& h) const;
  int newestOf(Type type, uint8_t sender) const;
  int store(const Header& h, const uint8_t* raw, size_t len);
  bool hasAllPieces(const Header& h) const;
  void addAck(uint8_t to, uint8_t ackedSender, uint32_t ackedCounter, Result& r);
  void addSlot(int index, Result& r) const;
  void answerMessages(uint8_t member, uint8_t sender, uint32_t after, uint64_t before, Result& r) const;
  void answerSync(uint8_t member, const SyncReq& req, Result& r) const;

  uint8_t group_;
  Members members_ = 0;
  CounterReserve counters_;
  SeenPackets seen_;
  uint32_t nextSeq_ = 1;
  Slot slots_[SLOTS];
};

}  // namespace mesh
