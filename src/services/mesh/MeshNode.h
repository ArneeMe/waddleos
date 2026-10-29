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
#include "MeshGroups.h"
#include "MessageStore.h"

namespace mesh {

enum class ServerState : uint8_t { Unknown, Online, Offline };

struct MemberStatus {
  uint32_t heardAtMs = 0;  // last packet heard straight from the member's radio (0 = never)
  int rssi = 0;
  uint8_t battery = 0;  // from their last sync request or position, 0 = unknown
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

  // Loads keys, the network and history and starts the radio. Before it has
  // joined a network (`waddle mesh join`) the unit stays quiet.
  void begin();
  void loop();
  bool configured() const { return group_.configured; }
  const Group& group() const { return group_; }
  const Identity& me() const { return identity(); }
  MessageStore& store() { return store_; }

  // Writes a message to a conversation (MessageStore.h). False when it is too
  // long or the unit has no key for that conversation.
  bool sendText(uint8_t conversation, const std::string& text);
  // Whether this unit has the key to write to the conversation.
  bool canWrite(uint8_t conversation) const;

  // Groups made on units (protocol/PROTOCOL.md, "Groups").
  const ChatGroups& groups() const { return groups_; }
  // Makes a group of me and these members (bits) and invites them. Returns
  // its conversation, or CONV_GROUP when it could not.
  uint8_t createGroup(Members members, const std::string& name);
  // Leaves a group: the others are told and make a new key.
  void leaveGroup(uint8_t conversation);
  // Takes a member out (a lost unit): the rest get a new key.
  void removeFromGroup(uint8_t conversation, uint8_t member);
  // The conversation was shown: clears unread and tells the sender "read".
  void markRead(uint8_t conversation);

  void startSos();
  void cancelSos();
  bool sosActive() const { return sosActive_; }
  // Who has my active SOS, and who has seen it; and whether the server has it.
  Members sosReceivedBy() const { return sosReceivedBy_; }
  Members sosSeenBy() const { return sosSeenBy_; }
  bool sosOnServer() const { return sosOnServer_; }
  // The user saw another member's SOS alert: tell them.
  void acknowledgeSos(uint8_t from);
  // A member whose SOS is active and not yet shown on screen, or 0.
  uint8_t takeNewSos();
  bool hasNewSos() const;
  const MemberStatus& member(uint8_t number) const;

  // Asks the server and the other units for what this unit missed (over LoRa).
  void requestSync();
  // A check for messages: asks over LoRa (and the server over Wi-Fi at home),
  // then listens for the answers for a little while. Runs by itself at the
  // interval set in MeshSettings, and when the unit is woken.
  void startCheck();
  // Whether the radio should stay on while the unit sleeps: the interval is
  // "all the time", or an SOS is going on.
  bool listensAlways() const;
  // Milliseconds until the next check (UINT32_MAX: only when woken).
  uint32_t msUntilCheck() const;
  // Unix time of the last check that reached anyone, 0 = none yet.
  uint32_t lastCheckUnix() const { return lastCheckUnix_; }
  // Joins Wi-Fi and talks to the server directly. Runs in loop(); at most 30 s.
  void startWifiSync();
  bool wifiSyncRunning() const { return wifi_ != WifiStep::Idle; }
  ServerState serverState() const;
  uint32_t lastServerContactMs() const { return serverAtMs_; }

  // Goes up by one for every change the screens should show.
  uint32_t changes() const { return changes_; }
  // True while packets wait to go out or Wi-Fi is on: do not sleep yet.
  bool busy() const;
  size_t outboxSize() const { return outbox_.size(); }
  // For the diagnostics page (MeshWebApi.cpp, /api/mesh/diag).
  size_t pendingSize() const { return pending_.size(); }
  uint32_t airtimeLastHourMs() const { return budget_.usedLastHour(millis()); }
  // Milliseconds until the node next has something to do (for light sleep).
  uint32_t msUntilWork() const;

 private:
#ifdef MESH_SIM
  friend struct SimAccess;  // test/sim: plays a misbehaving member
#endif
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
  ChatGroups groups_;
  SenderKeys senders_;
  std::vector<Fetch> fetches_;  // missing messages to ask the server for over Wi-Fi
  std::vector<uint32_t> controlCounters_;  // my invitations and notices this session
  std::vector<uint32_t> controlWaiting_;
  std::vector<uint64_t> opened_;  // sender << 32 | counter of messages opened lately   // of those, the ones nobody has answered yet
  uint32_t senderKeyCounters_[MAX_MEMBERS] = {};  // my sender key DM to each member, until confirmed
  Members senderKeyConfirmed_ = 0;                 // who has my current sender key
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
  uint32_t lastResendMs_ = 0;
  bool wifiWasOn_ = false;  // Wi-Fi was on before the sync: leave it on
  uint32_t nextCheckMs_ = 0;
  uint32_t checkUntilMs_ = 0;  // listening for the answers of a check
  uint32_t lastCheckUnix_ = 0;
  uint32_t nextPositionMs_ = 0;
  bool sosActive_ = false;
  uint32_t nextSosMs_ = 0;
  uint8_t newSos_ = 0;
  std::vector<uint32_t> sosCounters_;  // my SOS packets of the current alarm
  Members sosReceivedBy_ = 0;
  Members sosSeenBy_ = 0;
  bool sosOnServer_ = false;
  struct AskedKey {
    uint32_t id = 0;  // member << 24 | group id << 8 | key number
    uint32_t atMs = 0;
  };
  std::vector<AskedKey> askedKeys_;
  std::vector<std::vector<uint8_t>> rosterPieces_;  // of the roster coming in

  uint32_t nextCounter();
  Header header(Type type, uint8_t recipient);
  void queue(const uint8_t* raw, size_t len, uint32_t delayMs, bool cancelIfHeard);
  void sendSealed(const Header& h, const uint8_t* key, const uint8_t* plain, size_t len, bool store,
                  uint32_t delayMs = 0);
  void sendAck(uint8_t toMember, uint8_t ackedSender, uint32_t counter, AckState state);
  void sendPosition(Type type, bool cancelled);
  void handle(const uint8_t* raw, size_t len, int rssi, bool viaWifi);
  void handleMessage(const Header& h, const uint8_t* cipher, size_t len);
  bool listeningForCheck() const;
  // A message is here (opened this session, or in its chat).
  bool haveMessage(const Header& h) const;
  void handlePosition(const Header& h, const uint8_t* cipher, size_t len);
  void handleAck(const Header& h, const uint8_t* body, size_t len);
  void handleSyncReq(const Header& h, const uint8_t* body, size_t len);
  const uint8_t* keyFor(const Header& h) const;
  const uint8_t* sendingKey(uint8_t conversation, uint8_t* keyNumber = nullptr) const;
  uint8_t recipientOf(uint8_t conversation) const;
  // The conversation of a packet, or -1 for a group this unit is not in.
  int conversationOf(const Header& h) const;
  void handleFetch(const Header& h, const uint8_t* body, size_t len);
  void requestFetch(uint8_t sender, uint32_t after, uint32_t before);
  void fillGap(uint8_t conversation, const ChatMessage& late);
  uint8_t syncFlags() const;
  uint32_t sendControl(uint8_t to, const std::string& payload, uint32_t delayMs = 0);
  void sendToMember(int slot, uint8_t to, char kind, uint8_t about);
  // False when it cannot be taken yet (not confirmed, so it comes again).
  bool handleControl(uint8_t from, const std::string& text);
  void missingKey(const Header& h);
  void askForKey(uint8_t to, uint16_t groupId, uint8_t number);
  void answerKeyRequest(uint8_t from, uint16_t groupId, uint8_t number);
  std::vector<std::vector<uint8_t>> savedRosterPackets() const;
  bool confirmControl(uint8_t from, uint32_t counter);
  void resendUnconfirmed();
  // Queues my stored packet with this counter again; false when it is not there.
  bool resendStored(uint32_t counter, uint32_t delayMs);
  void rotate(int slot);
  void takeOut(int slot, uint8_t member);
  void openWaiting(uint16_t groupId);
  void openWaitingFrom(uint8_t sender);
  void loadKeyConfirmations();
  void saveKeyConfirmations();
  void dropMissingMembers();
  void handleRoster(const Header& h, const uint8_t* cipher, size_t len);
  // After a new roster: newcomers get my sender key, those gone leave the groups.
  void rosterChanged(Members before);
  // delayMs: when every unit sends its key at once (a new member list heard
  // by all), spread out so they do not talk over each other.
  void sendSenderKey(uint8_t to, uint32_t delayMs = 0);
  uint8_t conversationOfMine(uint32_t counter);
  void sendOutbox();
  void retryPending();
  // Returns how many packets the server sent.
  size_t runWifiSync();
  void savePositions();
  void loadPositions();
};

}  // namespace mesh
