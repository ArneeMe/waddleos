#pragma once

// The mesh unit's messaging brain: takes packets from the radio (and from the
// server over Wi-Fi), decrypts, stores, acknowledges and relays them, and sends
// what the user writes. Runs from the main loop and during light sleep.
// See protocol/PROTOCOL.md for the rules it follows.

#include <MeshCrypto.h>
#include <MeshPacket.h>
#include <MeshRadioRules.h>

#include <memory>
#include <string>
#include <vector>

#include "MeshConfig.h"
#include "MessageStore.h"

namespace mesh {

enum class ServerState : uint8_t { Unknown, Online, Offline };

struct MemberStatus {
  uint32_t heardAtMs = 0;  // last packet heard from the member (0 = never)
  int rssi = 0;
  bool hasPosition = false;
  Position position;
  uint32_t positionAtMs = 0;
  bool sos = false;  // an SOS from this member is active
  Position sosPosition;
  uint32_t sosCounter = 0;  // their newest SOS packet, for "seen"
};

class Node {
 public:
  static Node& get();

  // Loads keys, group and history and starts the radio. Without a group from
  // `waddle mesh setup` the unit stays quiet.
  void begin();
  void loop();
  bool configured() const { return group_.configured; }
  const Group& group() const { return group_; }
  const Identity& me() const { return identity(); }
  MessageStore& store() { return store_; }

  // Writes a message to a conversation (CONV_GROUP or a member number).
  // False when it is too long or the unit has no group.
  bool sendText(uint8_t conversation, const std::string& text);
  // The conversation was shown: clears unread and tells the sender "read".
  void markRead(uint8_t conversation);

  void startSos();
  void cancelSos();
  bool sosActive() const { return sosActive_; }
  // Who has my active SOS: bit per member (bit n-1), and SERVER_BIT for the server.
  static constexpr uint8_t SERVER_BIT = 0x80;
  uint8_t sosReceivedBy() const { return sosReceivedBy_; }
  uint8_t sosSeenBy() const { return sosSeenBy_; }
  // The user saw another member's SOS alert: tell them.
  void acknowledgeSos(uint8_t from);
  // A member whose SOS is active and not yet shown on screen, or 0.
  uint8_t takeNewSos();
  bool hasNewSos() const;
  const MemberStatus& member(uint8_t number) const;

  // Asks the server and the other units for what this unit missed (over LoRa).
  void requestSync();
  // Joins Wi-Fi and talks to the server directly. Runs in loop(); at most 30 s.
  void startWifiSync();
  bool wifiSyncRunning() const { return wifi_ != WifiStep::Idle; }
  ServerState serverState() const;
  uint32_t lastServerContactMs() const { return serverAtMs_; }

  // Goes up by one for every change the screens should show.
  uint32_t changes() const { return changes_; }
  // True while packets wait to go out or Wi-Fi is on: do not sleep yet.
  bool busy() const;
  // Milliseconds until the node next has something to do (for light sleep).
  uint32_t msUntilWork() const;

 private:
  struct Outgoing {
    std::vector<uint8_t> raw;
    uint32_t dueMs = 0;
    bool cancelIfHeard = false;  // a relay or sync answer: skip it if another unit sent it first
  };
  struct Pending {
    uint32_t counter = 0;
    uint8_t conversation = 0;
    uint8_t tries = 0;
    uint32_t nextTryMs = 0;
  };
  enum class WifiStep : uint8_t { Idle, Joining };

  Group group_;
  MessageStore store_;
  LoraParams params_;
  AirtimeBudget budget_;
  SeenPackets seen_;
  Reassembler pieces_;
  std::unique_ptr<CounterReserve> counters_;
  std::vector<Outgoing> outbox_;
  std::vector<Pending> pending_;
  MemberStatus members_[MAX_MEMBERS];
  uint32_t changes_ = 0;
  uint32_t serverAtMs_ = 0;
  bool wifiServerFailed_ = false;
  WifiStep wifi_ = WifiStep::Idle;
  uint32_t wifiStartedMs_ = 0;
  uint32_t lastWifiSyncMs_ = 0;
  uint32_t lastSyncReqMs_ = 0;
  uint32_t nextPositionMs_ = 0;
  bool sosActive_ = false;
  uint32_t nextSosMs_ = 0;
  uint8_t newSos_ = 0;
  std::vector<uint32_t> sosCounters_;  // my SOS packets of the current alarm
  uint8_t sosReceivedBy_ = 0;
  uint8_t sosSeenBy_ = 0;

  uint32_t nextCounter();
  Header header(Type type, uint8_t recipient);
  void queue(const uint8_t* raw, size_t len, uint32_t delayMs, bool cancelIfHeard);
  void sendSealed(const Header& h, const uint8_t* key, const uint8_t* plain, size_t len, bool store);
  void sendAck(uint8_t toMember, uint8_t ackedSender, uint32_t counter, AckState state);
  void sendPosition(Type type, bool cancelled);
  void handle(const uint8_t* raw, size_t len, int rssi, bool viaWifi);
  void handleMessage(const Header& h, const uint8_t* cipher, size_t len);
  void handlePosition(const Header& h, const uint8_t* cipher, size_t len);
  void handleAck(const Header& h, const uint8_t* body, size_t len);
  void handleSyncReq(const Header& h, const uint8_t* body, size_t len);
  const uint8_t* keyFor(const Header& h) const;
  uint8_t conversationOfMine(uint32_t counter);
  void sendOutbox();
  void retryPending();
  void runWifiSync();
  void savePositions();
  void loadPositions();
};

}  // namespace mesh
