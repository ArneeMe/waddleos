#pragma once

// Messages on the SD card, so they survive restarts.
//
//   /.mesh/packets/<sender>.bin  every MSG packet as it was sent or received,
//                                 [length][packet] records, to answer sync requests
//   /.mesh/chat/<conversation>.log what the screens show: messages and their
//                                 delivery states, as appended records
//   /.mesh/read.txt               how many messages of each conversation were seen
//
// A conversation is the whole group (0), a DM with member 1-4, or a small
// group (CONV_SMALL + its member bits).

#include <MeshPacket.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mesh {

constexpr uint8_t CONV_GROUP = 0;
constexpr uint8_t CONV_SMALL = 1 + MAX_MEMBERS;
constexpr int CONVERSATIONS = CONV_SMALL + 16;

// The conversation a packet from sender to recipient belongs to, seen by me.
inline uint8_t conversationOf(uint8_t me, uint8_t sender, uint8_t recipient) {
  if (recipient == GROUP) return CONV_GROUP;
  if (isSmallGroup(recipient)) return uint8_t(CONV_SMALL + (recipient & 0x0F));
  return sender == me ? recipient : sender;
}
// The recipient byte for a conversation.
inline uint8_t recipientOf(uint8_t conversation) {
  if (conversation == CONV_GROUP) return GROUP;
  if (conversation >= CONV_SMALL) return uint8_t(SMALL_GROUP | (conversation - CONV_SMALL));
  return conversation;
}
inline bool isSmallConversation(uint8_t conversation) {
  return conversation >= CONV_SMALL && conversation < CONVERSATIONS;
}

enum class Delivery : uint8_t { Sending = 0, Sent = 1, Stored = 2, Delivered = 3, Read = 4 };

struct ChatMessage {
  uint8_t sender = 0;
  uint32_t counter = 0;
  uint32_t time = 0;
  uint8_t prevHash[4] = {};
  bool chainWarning = false;  // the previous message from this sender is missing or different
  std::string text;
  // For my own messages: who has it (bit per member) and the server.
  uint8_t deliveredTo = 0;
  uint8_t readBy = 0;
  bool stored = false;
  bool sent = false;  // went out over the radio or to the server at least once
};

struct ConversationSummary {
  uint32_t count = 0;      // messages in the conversation
  uint32_t unread = 0;     // from others, not yet seen
  uint32_t lastTime = 0;
  uint8_t lastSender = 0;
  std::string lastText;    // the start of the newest message
};

class MessageStore {
 public:
  // Reads the summaries of all conversations (at start).
  void begin(uint8_t me);

  // Raw packets, for sync answers.
  void savePacket(uint8_t sender, const uint8_t* raw, size_t len);
  bool hasPacket(const Header& h);
  // Every stored MSG piece from sender with a counter above `after`, oldest first.
  // `readableBy` drops DMs between two other members.
  std::vector<std::vector<uint8_t>> packetsAfter(uint8_t sender, uint32_t after, uint8_t readableBy, size_t limit);
  // The highest MSG counter stored from sender.
  uint32_t lastCounter(uint8_t sender) const { return isMember(sender) ? lastCounter_[sender - 1] : 0; }

  // Chat history.
  void addMessage(uint8_t conversation, const ChatMessage& m);
  void addState(uint8_t conversation, uint8_t sender, uint32_t counter, Delivery state, uint8_t byMember);
  // The newest `limit` messages of a conversation, oldest first, with states applied.
  std::vector<ChatMessage> load(uint8_t conversation, size_t limit) const;
  bool hasMessage(uint8_t conversation, uint8_t sender, uint32_t counter) const;
  // The hash and counter of sender's newest message in the conversation (for
  // the chain). An older message arriving later fills a gap and is not checked.
  void lastHash(uint8_t conversation, uint8_t sender, uint8_t out[4]) const;
  uint32_t lastChainCounter(uint8_t conversation, uint8_t sender) const;

  const ConversationSummary& summary(uint8_t conversation) const { return summaries_[conversation]; }
  uint32_t totalUnread() const;
  void markRead(uint8_t conversation);

 private:
  uint8_t me_ = 0;
  uint32_t lastCounter_[MAX_MEMBERS] = {};
  ConversationSummary summaries_[CONVERSATIONS];
  uint32_t seen_[CONVERSATIONS] = {};  // read.txt
  // Newest message hash per conversation and sender.
  uint8_t chain_[CONVERSATIONS][MAX_MEMBERS][4] = {};
  uint32_t chainCounter_[CONVERSATIONS][MAX_MEMBERS] = {};
  void updateChain(uint8_t conversation, const ChatMessage& m);

  void saveSeen();
};

// The plaintext hash used for the chain: BLAKE2b of time, previous hash and text.
void messageHash(const ChatMessage& m, uint8_t out[4]);

}  // namespace mesh
