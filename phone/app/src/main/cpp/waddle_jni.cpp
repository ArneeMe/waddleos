// The app's way into the mesh node (Mesh.kt). Every call comes from the app's one
// mesh thread; answers are JSON text or bytes.

#include <ArduinoJson.h>
#include <MeshPairClient.h>
#include <MeshPlay.h>
#include <esp_random.h>
#include <jni.h>

#include <ctime>
#include <filesystem>
#include <memory>
#include <string>

#include "services/mesh/MeshConfig.h"
#include "services/mesh/MeshNode.h"

namespace sim {
extern uint32_t nowMs;
extern std::string rootDir;
}  // namespace sim

namespace {

std::unique_ptr<mesh::PairClient> pairClient;

mesh::Node& node() { return mesh::Node::get(); }

// The node's millis(): the phone's monotonic clock.
void tick() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  sim::nowMs = uint32_t(uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000);
}

std::string str(JNIEnv* env, jstring s) {
  if (!s) return "";
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out(c);
  env->ReleaseStringUTFChars(s, c);
  return out;
}

jstring jstr(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

std::string json(const JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

const char* personName(const uint8_t number) {
  return number == node().network().me ? "Du" : node().network().name(number);
}

// A conversation's name: "Alle", the person, or the group (its name, or who is in it).
std::string title(const uint8_t c) {
  if (c == mesh::CONV_EVERYONE) return "Alle";
  if (mesh::isDmConversation(c)) return node().network().name(c);
  const auto& g = node().groups().at(c - mesh::CONV_FIRST_GROUP);
  if (g.name[0]) return g.name;
  std::string names;
  for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
    if (n == node().network().me || !(g.members & mesh::memberBit(n))) continue;
    names += (names.empty() ? "" : ", ") + std::string(node().network().name(n));
  }
  return names;
}

// What the sender sees under their own message (as on the T5).
std::string delivery(const mesh::ChatMessage& m, const uint8_t c) {
  const mesh::Members me = mesh::memberBit(node().network().me);
  const mesh::ChatGroup* g = mesh::isGroupConversation(c) ? &node().groups().at(c - mesh::CONV_FIRST_GROUP) : nullptr;
  const mesh::Members wanted = c == mesh::CONV_EVERYONE ? node().network().everyone() & ~me
                               : g                      ? g->members & ~me
                                                        : mesh::memberBit(c);
  if (m.readBy && (m.readBy & wanted) == wanted) return "lest";
  if ((m.deliveredTo & wanted) == wanted) return mesh::isDmConversation(c) ? "levert" : "levert til alle";
  if (m.deliveredTo) return "levert til noen";
  if (m.stored) return "lagret på server";
  return "sendes ved neste synk";
}

std::string nameOrDu(const uint8_t person) { return person == node().network().me ? "Du" : personName(person); }

// The conversation's polls, games, votes and moves (MeshPlay.h).
std::vector<mesh::ChatMessage> playsOf(const uint8_t c) {
  return node().store().load(c, 400, mesh::MessageStore::Only::Plays);
}

std::vector<mesh::play::Entry> entriesOf(const std::vector<mesh::ChatMessage>& plays) {
  std::vector<mesh::play::Entry> out;
  for (const auto& m : plays) out.push_back({m.sender, m.counter, &m.text});
  return out;
}

// A poll or a game in the chat list: what the phone's card shows.
void addPlay(JsonObject o, const mesh::ChatMessage& m, const std::vector<mesh::play::Entry>& plays) {
  mesh::play::Play p;
  if (!mesh::play::read(m.text, p)) {
    o["text"] = mesh::play::preview(m.text);
    return;
  }
  const mesh::play::Ref ref{m.sender, m.counter};
  o["sender"] = m.sender;
  o["counter"] = m.counter;
  const uint8_t me = node().network().me;
  if (p.kind == mesh::play::Kind::Poll) {
    const auto t = mesh::play::tally(ref, plays);
    JsonObject poll = o["poll"].to<JsonObject>();
    poll["question"] = p.question;
    JsonArray options = poll["options"].to<JsonArray>();
    for (size_t i = 0; i < p.options.size(); i++) {
      JsonObject opt = options.add<JsonObject>();
      opt["text"] = p.options[i];
      opt["count"] = mesh::memberCount(t.votes[i]);
      opt["mine"] = (t.votes[i] & mesh::memberBit(me)) != 0;
      std::string who;
      for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
        if (!(t.votes[i] & mesh::memberBit(n))) continue;
        if (!who.empty()) who += ", ";
        who += nameOrDu(n);
      }
      opt["names"] = who;
    }
  } else if (p.kind == mesh::play::Kind::Game) {
    mesh::play::GameState s;
    mesh::play::replay(ref, plays, s);
    JsonObject game = o["game"].to<JsonObject>();
    game["status"] = mesh::play::status(s, me, nameOrDu);
    game["canMove"] = mesh::play::canMove(s, me);
    // Rows from the top, a digit per cell: 0 empty, 1 or 2 the player.
    std::string board;
    for (int r = mesh::play::FourInRow::ROWS - 1; r >= 0; r--) {
      for (int col = 0; col < mesh::play::FourInRow::COLS; col++) board += char('0' + s.board.cell[r][col]);
    }
    game["board"] = board;
    game["players"] = (s.opponent ? nameOrDu(s.opponent) : std::string("?")) + " mot " + nameOrDu(s.challenger);
  }
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_start(JNIEnv* env, jclass, jstring dir) {
  // Once per run of the app (pairFinish starts it again after pairing).
  static bool started = false;
  if (!started) {
    sim::rootDir = str(env, dir);
    std::filesystem::create_directories(sim::rootDir);
    tick();
    node().begin();
    started = true;
  }
  JsonDocument doc;
  const auto& net = node().network();
  doc["configured"] = net.configured;
  doc["me"] = net.me;
  doc["name"] = net.configured ? net.name(net.me) : "";
  return jstr(env, json(doc));
}

JNIEXPORT jint JNICALL Java_me_arnee_waddle_Mesh_changes(JNIEnv*, jclass) { return jint(node().changes()); }

JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_conversations(JNIEnv* env, jclass) {
  tick();
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  const auto& net = node().network();
  for (uint8_t c = 0; net.configured && c < mesh::CONVERSATIONS; c++) {
    if (mesh::isDmConversation(c) && (c == net.me || !net.member(c) || !net.member(c)->present)) continue;
    if (mesh::isGroupConversation(c)) {
      const auto& g = node().groups().at(c - mesh::CONV_FIRST_GROUP);
      if (!g.id) continue;
    }
    const auto& sum = node().store().summary(c);
    JsonObject o = list.add<JsonObject>();
    o["conv"] = c;
    o["title"] = title(c);
    o["unread"] = sum.unread;
    o["time"] = sum.lastTime;
    o["last"] = sum.count ? std::string(personName(sum.lastSender)) + ": " + sum.lastText : "";
    o["group"] = mesh::isGroupConversation(c);
  }
  return jstr(env, json(doc));
}

JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_chat(JNIEnv* env, jclass, jint conv) {
  tick();
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  const uint8_t c = uint8_t(conv);
  if (c < mesh::CONVERSATIONS) {
    const auto plays = playsOf(c);
    const auto entries = entriesOf(plays);
    for (const auto& m : node().store().load(c, 200, mesh::MessageStore::Only::Shown)) {
      JsonObject o = list.add<JsonObject>();
      const bool mine = mesh::person(m.sender) == node().network().me;
      o["mine"] = mine;
      o["from"] = node().network().name(m.sender);
      o["text"] = m.text;
      o["time"] = m.time;
      o["warning"] = m.chainWarning;
      if (mine) o["state"] = delivery(m, c);
      if (mesh::play::isPlay(m.text)) addPlay(o, m, entries);
    }
  }
  return jstr(env, json(doc));
}

JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_title(JNIEnv* env, jclass, jint conv) {
  return jstr(env, conv >= 0 && conv < mesh::CONVERSATIONS ? title(uint8_t(conv)) : "");
}

JNIEXPORT jboolean JNICALL Java_me_arnee_waddle_Mesh_canWrite(JNIEnv*, jclass, jint conv) {
  return conv >= 0 && conv < mesh::CONVERSATIONS && node().canWrite(uint8_t(conv));
}

JNIEXPORT jboolean JNICALL Java_me_arnee_waddle_Mesh_send(JNIEnv* env, jclass, jint conv, jstring text) {
  tick();
  return conv >= 0 && conv < mesh::CONVERSATIONS && node().sendText(uint8_t(conv), str(env, text));
}

JNIEXPORT void JNICALL Java_me_arnee_waddle_Mesh_markRead(JNIEnv*, jclass, jint conv) {
  tick();
  if (conv >= 0 && conv < mesh::CONVERSATIONS) node().markRead(uint8_t(conv));
}

JNIEXPORT jbyteArray JNICALL Java_me_arnee_waddle_Mesh_syncRequest(JNIEnv* env, jclass) {
  tick();
  const std::string body = node().serverRequest();
  jbyteArray out = env->NewByteArray(jsize(body.size()));
  env->SetByteArrayRegion(out, 0, jsize(body.size()), reinterpret_cast<const jbyte*>(body.data()));
  return out;
}

JNIEXPORT jint JNICALL Java_me_arnee_waddle_Mesh_syncAnswer(JNIEnv* env, jclass, jbyteArray answer, jlong date) {
  tick();
  const jsize n = env->GetArrayLength(answer);
  std::string bytes(size_t(n), '\0');
  env->GetByteArrayRegion(answer, 0, n, reinterpret_cast<jbyte*>(&bytes[0]));
  return jint(node().takeServerAnswer(bytes, uint32_t(date)));
}

// --- Groups ------------------------------------------------------------------

// The others in the network: [{"number", "name"}], by name.
JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_members(JNIEnv* env, jclass) {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  const auto& net = node().network();
  for (uint8_t n = 1; net.configured && n <= mesh::MAX_MEMBERS; n++) {
    if (n == net.me || !net.member(n)->present) continue;
    JsonObject o = list.add<JsonObject>();
    o["number"] = n;
    o["name"] = net.name(n);
  }
  return jstr(env, json(doc));
}

// A new group of me and these members (bit n-1 = member n): its conversation, or -1.
JNIEXPORT jint JNICALL Java_me_arnee_waddle_Mesh_createGroup(JNIEnv* env, jclass, jlong members, jstring name) {
  tick();
  return node().createGroup(mesh::Members(members), str(env, name));
}

// A group's members and whether I left it: {"members": [names], "left": bool}.
JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_group(JNIEnv* env, jclass, jint conv) {
  JsonDocument doc;
  if (mesh::isGroupConversation(uint8_t(conv))) {
    const auto& g = node().groups().at(conv - mesh::CONV_FIRST_GROUP);
    const auto& net = node().network();
    JsonArray names = doc["members"].to<JsonArray>();
    for (uint8_t n = 1; n <= mesh::MAX_MEMBERS; n++) {
      if (g.members & mesh::memberBit(n)) names.add(std::string(net.name(n)) + (n == net.me ? " (deg)" : ""));
    }
    doc["left"] = g.left;
  }
  return jstr(env, json(doc));
}

JNIEXPORT void JNICALL Java_me_arnee_waddle_Mesh_leaveGroup(JNIEnv*, jclass, jint conv) {
  tick();
  if (mesh::isGroupConversation(uint8_t(conv))) node().leaveGroup(uint8_t(conv));
}

// Where and how to reach the Pi: {"url", "token", "cert"}.
JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_server(JNIEnv* env, jclass) {
  JsonDocument doc;
  const auto& net = node().network();
  doc["url"] = net.serverUrl;
  doc["token"] = net.serverToken;
  doc["cert"] = net.serverCert;
  return jstr(env, json(doc));
}

// --- Pairing with the member's T5 (protocol/PROTOCOL.md, "Devices") ------------

JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_pairBegin(JNIEnv* env, jclass) {
  uint8_t random[mesh::KEY_SIZE + mesh::PAIR_NONCE_SIZE];
  esp_fill_random(random, sizeof(random));
  pairClient = std::make_unique<mesh::PairClient>(random);
  return jstr(env, pairClient->commitHex());
}

// The T5's public key and nonce in; {"tool", "nonce", "code"} out.
JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_pairAnswer(JNIEnv* env, jclass, jstring pub, jstring nonce) {
  JsonDocument doc;
  char code[8] = "";
  if (pairClient && pairClient->unitAnswer(str(env, pub), str(env, nonce), code)) {
    doc["tool"] = pairClient->toolHex();
    doc["nonce"] = pairClient->nonceHex();
    doc["code"] = code;
    return jstr(env, json(doc));
  }
  return jstr(env, "{}");
}

// The body for POST /api/mesh/phone.
JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_pairRequest(JNIEnv* env, jclass) {
  return jstr(env, pairClient ? pairClient->sealRequest("{}") : "");
}

// The T5's sealed copy in; "" when paired, else what went wrong.
JNIEXPORT jstring JNICALL Java_me_arnee_waddle_Mesh_pairFinish(JNIEnv* env, jclass, jstring box) {
  tick();
  std::string copy;
  if (!pairClient || !pairClient->open(str(env, box), mesh::PHONE_COPY_NONCE, copy)) {
    return jstr(env, "Svaret fra T5-en kunne ikke åpnes");
  }
  pairClient.reset();
  if (const char* err = mesh::takePhoneCopy(copy)) return jstr(env, err);
  node().begin();
  return jstr(env, "");
}


// Polls and Fire på rad (MeshPlay.h).
JNIEXPORT jboolean JNICALL Java_me_arnee_waddle_Mesh_startPoll(JNIEnv* env, jclass, jint conv, jstring question,
                                                                 jstring options) {
  tick();
  std::vector<std::string> list;
  const std::string all = str(env, options);  // one per line
  size_t at = 0;
  while (at <= all.size()) {
    const size_t end = std::min(all.find('\n', at), all.size());
    if (end > at) list.push_back(all.substr(at, end - at));
    at = end + 1;
  }
  const std::string text = mesh::play::writePoll(str(env, question), list);
  return conv >= 0 && conv < mesh::CONVERSATIONS && !text.empty() && node().sendText(uint8_t(conv), text);
}

JNIEXPORT jboolean JNICALL Java_me_arnee_waddle_Mesh_vote(JNIEnv*, jclass, jint conv, jint sender, jlong counter,
                                                            jint option) {
  tick();
  if (conv < 0 || conv >= mesh::CONVERSATIONS || option < 0 || option >= int(mesh::play::MAX_OPTIONS)) return false;
  return node().sendText(uint8_t(conv), mesh::play::writeVote({uint8_t(sender), uint32_t(counter)}, uint8_t(option)));
}

JNIEXPORT jboolean JNICALL Java_me_arnee_waddle_Mesh_startGame(JNIEnv*, jclass, jint conv) {
  tick();
  return conv >= 0 && conv < mesh::CONVERSATIONS &&
         node().sendText(uint8_t(conv), mesh::play::writeGame(mesh::play::Game::FourInRow));
}

// Drops my piece in column col (0-6), when it is my move.
JNIEXPORT jboolean JNICALL Java_me_arnee_waddle_Mesh_move(JNIEnv*, jclass, jint conv, jint sender, jlong counter,
                                                            jint col) {
  tick();
  if (conv < 0 || conv >= mesh::CONVERSATIONS) return false;
  const mesh::play::Ref ref{uint8_t(sender), uint32_t(counter)};
  const auto plays = playsOf(uint8_t(conv));
  mesh::play::GameState s;
  if (!mesh::play::replay(ref, entriesOf(plays), s) || !mesh::play::canMove(s, node().network().me) ||
      !s.board.canDrop(col)) {
    return false;
  }
  return node().sendText(uint8_t(conv), mesh::play::writeMove(ref, uint16_t(s.board.moves + 1), std::string(1, char('0' + col))));
}

}  // extern "C"
