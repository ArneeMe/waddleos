// Host tests for lib/WaddleMesh. Run: bash test/mesh/run.sh
// With an output path as argument it also writes the shared test vectors
// (protocol/vectors.json) that the Python server tests check against.

#include <MeshCrypto.h>
#include <MeshPacket.h>
#include <MeshRadioRules.h>

#include "../../src/services/mesh/MeshTime.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace mesh;

static int failures = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      failures++;                                                    \
    }                                                                \
  } while (0)

static std::string hex(const uint8_t* p, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += digits[p[i] >> 4];
    s += digits[p[i] & 15];
  }
  return s;
}

static void fill(uint8_t* p, size_t n, uint8_t start) {
  for (size_t i = 0; i < n; i++) p[i] = uint8_t(start + i);
}

static Header msgHeader(uint8_t sender, uint8_t recipient, uint32_t counter) {
  Header h;
  h.type = Type::Msg;
  h.group = 0x42;
  h.sender = sender;
  h.recipient = recipient;
  h.counter = counter;
  return h;
}

static void testHeader() {
  Header h = msgHeader(2, GROUP, 0x01020304);
  h.hops = 2;
  uint8_t buf[HEADER_SIZE];
  CHECK(writeHeader(h, buf) == HEADER_SIZE);
  Header r;
  CHECK(readHeader(buf, sizeof(buf), r));
  CHECK(r.type == Type::Msg && r.group == 0x42 && r.hops == 2 && r.sender == 2 && r.recipient == GROUP);
  CHECK(r.counter == 0x01020304 && r.pieceCount == 1 && !r.fragmented);

  uint8_t bad[HEADER_SIZE];
  memcpy(bad, buf, sizeof(bad));
  bad[0] = 0;
  CHECK(!readHeader(bad, sizeof(bad), r));  // wrong magic
  memcpy(bad, buf, sizeof(bad));
  bad[1] = 0x19;
  CHECK(!readHeader(bad, sizeof(bad), r));  // unknown type
  memcpy(bad, buf, sizeof(bad));
  bad[4] = 7;
  CHECK(!readHeader(bad, sizeof(bad), r));  // no member 7
  memcpy(bad, buf, sizeof(bad));
  bad[10] = 0x22;
  CHECK(!readHeader(bad, sizeof(bad), r));  // piece 2 of 2
  CHECK(!readHeader(buf, 5, r));            // too short
}

static void testBodies() {
  Position p;
  p.time = 1761580800;
  p.latE7 = 599138890;
  p.lonE7 = -107522450;
  p.altitude = -12;
  p.battery = 87;
  p.noFix = true;
  p.cancelled = true;
  uint8_t buf[POS_SIZE];
  writePosition(p, buf);
  Position q;
  CHECK(readPosition(buf, sizeof(buf), q));
  CHECK(q.time == p.time && q.latE7 == p.latE7 && q.lonE7 == p.lonE7 && q.altitude == -12 && q.battery == 87 &&
        q.noFix && q.cancelled);

  Ack a{3, 77, AckState::Stored};
  uint8_t ab[ACK_SIZE];
  writeAck(a, ab);
  Ack b;
  CHECK(readAck(ab, sizeof(ab), b) && b.sender == 3 && b.counter == 77 && b.state == AckState::Stored);
  ab[5] = 9;
  CHECK(!readAck(ab, sizeof(ab), b));

  SyncReq s;
  s.lastCounter[0] = 5;
  s.lastCounter[2] = 900;
  s.battery = 40;
  s.build = 0xab;
  s.flags = SYNC_ON_TRIP | SYNC_SOS;
  uint8_t sb[SYNC_REQ_SIZE];
  writeSyncReq(s, sb);
  SyncReq t;
  CHECK(readSyncReq(sb, sizeof(sb), t) && t.lastCounter[0] == 5 && t.lastCounter[1] == 0 &&
        t.lastCounter[2] == 900 && t.battery == 40 && t.build == 0xab && t.flags == 3);
}

static void testGroups() {
  // Member 4 exists; member 5 does not.
  uint8_t raw[HEADER_SIZE];
  Header h = msgHeader(4, GROUP, 1), r;
  writeHeader(h, raw);
  CHECK(readHeader(raw, sizeof(raw), r) && r.sender == 4 && r.key == 0);
  raw[4] = 5;
  CHECK(!readHeader(raw, sizeof(raw), r));

  // Kari's (member 2) third group, key number 4.
  const uint8_t id = groupId(2, 3);
  CHECK(id == 0x23 && groupCreator(id) == 2);
  h = msgHeader(2, GROUP_FLAG | id, 7);
  h.key = 4;
  writeHeader(h, raw);
  CHECK(readHeader(raw, sizeof(raw), r) && r.recipient == (GROUP_FLAG | id) && r.key == 4);
  CHECK(isGroup(GROUP_FLAG | id) && !isGroup(GROUP) && !isGroup(GROUP_FLAG | groupId(6, 0)));
  raw[5] = GROUP_FLAG | groupId(6, 0);  // made by member 6, who does not exist
  CHECK(!readHeader(raw, sizeof(raw), r));

  // Groups go to everyone; DMs only to the two.
  CHECK(canRead(3, 1, GROUP_FLAG | id) && canRead(3, 1, GROUP) && canRead(2, 1, 2) && !canRead(3, 1, 2));
  CHECK(memberBit(4) == 8 && memberBit(0) == 0 && memberCount(0x0F) == 4);

  // The key number is sealed with the header: changing it breaks the message.
  uint8_t key[KEY_SIZE];
  fill(key, KEY_SIZE, 9);
  static Sealed sealed;
  const char* text = "Hyttetur";
  CHECK(seal(h, key, reinterpret_cast<const uint8_t*>(text), strlen(text), sealed));
  Header got;
  readHeader(sealed.packets[0], sealed.lengths[0], got);
  uint8_t out[64];
  CHECK(open(got, key, sealed.packets[0] + HEADER_SIZE, sealed.lengths[0] - HEADER_SIZE, out));
  got.key = 5;
  CHECK(!open(got, key, sealed.packets[0] + HEADER_SIZE, sealed.lengths[0] - HEADER_SIZE, out));

  // Fetch.
  uint8_t fb[FETCH_SIZE];
  writeFetch(Fetch{3, 30, 41}, fb);
  Fetch f;
  CHECK(readFetch(fb, sizeof(fb), f) && f.sender == 3 && f.after == 30 && f.before == 41);
  writeFetch(Fetch{3, 41, 30}, fb);
  CHECK(!readFetch(fb, sizeof(fb), f));
}

// Seals, then receives the pieces in the given order and opens the result.
static bool roundTrip(const Header& h, const uint8_t key[KEY_SIZE], const uint8_t* plain, size_t len,
                      const int* order, Sealed& sealed) {
  if (!seal(h, key, plain, len, sealed)) return false;
  Reassembler r;
  const uint8_t* joined = nullptr;
  size_t joinedLen = 0;
  bool done = false;
  for (int i = 0; i < sealed.count; i++) {
    const int k = order ? order[i] : i;
    Header got;
    if (!readHeader(sealed.packets[k], sealed.lengths[k], got)) return false;
    done = r.add(got, sealed.packets[k], sealed.lengths[k], 1000, &joined, &joinedLen);
    if (done != (i == sealed.count - 1)) return false;
  }
  if (!done || joinedLen != len + TAG_SIZE) return false;
  uint8_t out[3000];
  Header first;
  readHeader(sealed.packets[0], sealed.lengths[0], first);
  if (!open(first, key, joined, joinedLen, out)) return false;
  return memcmp(out, plain, len) == 0;
}

static void testSealOpen() {
  uint8_t key[KEY_SIZE];
  fill(key, sizeof(key), 1);
  static Sealed sealed;

  const char* text = "Hei fra skogen!";
  const Header h = msgHeader(1, GROUP, 10);
  CHECK(roundTrip(h, key, reinterpret_cast<const uint8_t*>(text), strlen(text), nullptr, sealed));
  CHECK(sealed.count == 1 && sealed.lengths[0] == HEADER_SIZE + strlen(text) + TAG_SIZE);

  // Long message, pieces arriving backwards.
  uint8_t longText[900];
  fill(longText, sizeof(longText), 7);
  const int backwards[] = {4, 3, 2, 1, 0};
  CHECK(roundTrip(h, key, longText, sizeof(longText), backwards, sealed));
  CHECK(sealed.count == 5);
  for (int i = 0; i < sealed.count; i++) CHECK(sealed.lengths[i] <= MAX_PACKET);

  // Too long for 15 pieces.
  static uint8_t huge[MAX_PIECES * MAX_PIECE];
  CHECK(!seal(h, key, huge, sizeof(huge), sealed));

  // A relay lowers hops: still opens. A changed counter or byte: does not.
  CHECK(seal(h, key, reinterpret_cast<const uint8_t*>(text), strlen(text), sealed));
  uint8_t* pkt = sealed.packets[0];
  const size_t len = sealed.lengths[0];
  uint8_t out[256];
  Header got;
  pkt[3] = 1;
  CHECK(readHeader(pkt, len, got) && open(got, key, pkt + HEADER_SIZE, len - HEADER_SIZE, out));
  pkt[6] ^= 1;
  CHECK(readHeader(pkt, len, got) && !open(got, key, pkt + HEADER_SIZE, len - HEADER_SIZE, out));
  pkt[6] ^= 1;
  pkt[HEADER_SIZE + 2] ^= 0x80;
  CHECK(readHeader(pkt, len, got) && !open(got, key, pkt + HEADER_SIZE, len - HEADER_SIZE, out));
  pkt[HEADER_SIZE + 2] ^= 0x80;
  uint8_t wrong[KEY_SIZE];
  fill(wrong, sizeof(wrong), 2);
  CHECK(readHeader(pkt, len, got) && !open(got, wrong, pkt + HEADER_SIZE, len - HEADER_SIZE, out));
}

static void testPairKey() {
  uint8_t aSecret[KEY_SIZE], bSecret[KEY_SIZE], aPublic[KEY_SIZE], bPublic[KEY_SIZE];
  fill(aSecret, KEY_SIZE, 0x10);
  fill(bSecret, KEY_SIZE, 0x60);
  publicKey(aSecret, aPublic);
  publicKey(bSecret, bPublic);
  uint8_t ab[KEY_SIZE], ba[KEY_SIZE];
  pairKey(aSecret, aPublic, bPublic, ab);
  pairKey(bSecret, bPublic, aPublic, ba);
  CHECK(memcmp(ab, ba, KEY_SIZE) == 0);
}

static void testCivilTime() {
  CHECK(unixFromCivil(1970, 1, 1, 0, 0, 0) == 0);
  CHECK(unixFromCivil(2026, 10, 27, 18, 2, 11) == 1793124131);
  CHECK(unixFromCivil(2024, 2, 29, 23, 59, 59) == 1709251199);  // leap day
  CHECK(unixFromCivil(2020, 3, 1, 0, 0, 0) == 1583020800);
  CHECK(unixFromCivil(2025, 1, 1, 12, 0, 0) == 1735732800);
}

static void testRadioRules() {
  LoraParams p;
  const uint32_t small = airtimeMs(p, 30);
  const uint32_t full = airtimeMs(p, MAX_PACKET);
  CHECK(small > 100 && small < 300);
  CHECK(full > 500 && full < 1100);
  std::printf("airtime SF%d BW%.0f: 30 B = %u ms, 200 B = %u ms\n", p.spreadingFactor, p.bandwidthKHz, small, full);

  AirtimeBudget budget(1000);
  CHECK(budget.allows(600, 0));
  budget.spend(600, 0);
  CHECK(!budget.allows(600, 1000));
  CHECK(budget.allows(600, 3600001));

  SeenPackets seen;
  Header h = msgHeader(1, GROUP, 5);
  CHECK(seen.remember(h));
  CHECK(!seen.remember(h));
  h.pieceIndex = 1;
  h.pieceCount = 2;
  CHECK(seen.remember(h));

  uint32_t save = 0;
  CounterReserve fresh(0);
  CHECK(fresh.take(&save) == 1 && save == CounterReserve::STEP);
  CHECK(fresh.take(&save) == 2 && save == 0);
  CounterReserve rebooted(CounterReserve::STEP);
  CHECK(rebooted.take(&save) == CounterReserve::STEP + 1 && save == 2 * CounterReserve::STEP);
}

// Fixed inputs and the packets they give, for the Python side.
static void writeVectors(const char* path) {
  FILE* f = std::fopen(path, "w");
  if (!f) {
    failures++;
    return;
  }
  uint8_t aSecret[KEY_SIZE], bSecret[KEY_SIZE], aPublic[KEY_SIZE], bPublic[KEY_SIZE], dmKey[KEY_SIZE];
  fill(aSecret, KEY_SIZE, 0x10);
  fill(bSecret, KEY_SIZE, 0x60);
  publicKey(aSecret, aPublic);
  publicKey(bSecret, bPublic);
  pairKey(aSecret, aPublic, bPublic, dmKey);
  uint8_t senderKey[KEY_SIZE];
  fill(senderKey, KEY_SIZE, 1);

  std::fprintf(f, "{\n  \"note\": \"Written by test/mesh/run.sh from the C++ code. Do not edit.\",\n");
  std::fprintf(f, "  \"senderKey\": \"%s\",\n", hex(senderKey, KEY_SIZE).c_str());
  std::fprintf(f, "  \"memberA\": {\"secret\": \"%s\", \"public\": \"%s\"},\n", hex(aSecret, KEY_SIZE).c_str(),
               hex(aPublic, KEY_SIZE).c_str());
  std::fprintf(f, "  \"memberB\": {\"secret\": \"%s\", \"public\": \"%s\"},\n", hex(bSecret, KEY_SIZE).c_str(),
               hex(bPublic, KEY_SIZE).c_str());
  std::fprintf(f, "  \"pairKey\": \"%s\",\n", hex(dmKey, KEY_SIZE).c_str());

  uint8_t groupKey[KEY_SIZE];
  fill(groupKey, KEY_SIZE, 3);
  std::fprintf(f, "  \"groupKey\": \"%s\",\n", hex(groupKey, KEY_SIZE).c_str());

  static Sealed sealed;
  std::fprintf(f, "  \"messages\": [\n");
  struct Case {
    const char* name;
    Header h;
    const uint8_t* key;
    std::string plain;
  };
  // MSG plaintext: time, previous hash, text.
  std::string groupPlain(8, '\0');
  put32(reinterpret_cast<uint8_t*>(&groupPlain[0]), 1761580800);
  groupPlain += "Hei fra skogen!";
  std::string dmPlain(8, '\0');
  put32(reinterpret_cast<uint8_t*>(&dmPlain[0]), 1761580900);
  uint8_t prev[4];
  shortHash(reinterpret_cast<const uint8_t*>(groupPlain.data()), groupPlain.size(), prev);
  memcpy(&dmPlain[4], prev, 4);
  dmPlain += std::string(400, 'x');
  Position pos;
  pos.time = 1761581000;
  pos.latE7 = 599138890;
  pos.lonE7 = 107522450;
  pos.altitude = 23;
  pos.battery = 64;
  std::string posPlain(POS_SIZE, '\0');
  writePosition(pos, reinterpret_cast<uint8_t*>(&posPlain[0]));

  Header gh = msgHeader(1, GROUP, 10);
  Header dh = msgHeader(1, 2, 11);
  Header ph = msgHeader(1, GROUP, 12);
  ph.type = Type::Pos;
  // A group made by member 1 (its first), under the group's key number 2.
  Header sh = msgHeader(1, GROUP_FLAG | groupId(1, 0), 13);
  sh.key = 2;
  const Case cases[] = {{"group message", gh, senderKey, groupPlain},
                        {"direct message in 3 pieces", dh, dmKey, dmPlain},
                        {"position", ph, senderKey, posPlain},
                        {"group message with key 2", sh, groupKey, groupPlain}};
  constexpr size_t CASES = sizeof(cases) / sizeof(cases[0]);
  for (size_t c = 0; c < CASES; c++) {
    const Case& k = cases[c];
    seal(k.h, k.key, reinterpret_cast<const uint8_t*>(k.plain.data()), k.plain.size(), sealed);
    std::fprintf(f, "    {\"name\": \"%s\", \"plain\": \"%s\", \"packets\": [", k.name,
                 hex(reinterpret_cast<const uint8_t*>(k.plain.data()), k.plain.size()).c_str());
    for (int i = 0; i < sealed.count; i++) {
      std::fprintf(f, "%s\"%s\"", i ? ", " : "", hex(sealed.packets[i], sealed.lengths[i]).c_str());
    }
    std::fprintf(f, "]}%s\n", c + 1 < CASES ? "," : "");
  }
  std::fprintf(f, "  ],\n");

  uint8_t ab[ACK_SIZE], sb[SYNC_REQ_SIZE], hb[HEADER_SIZE];
  writeAck(Ack{1, 10, AckState::Stored}, ab);
  Header ackH;
  ackH.type = Type::Ack;
  ackH.group = 0x42;
  ackH.sender = SERVER;
  ackH.recipient = 1;
  ackH.counter = 3;
  writeHeader(ackH, hb);
  std::fprintf(f, "  \"ack\": \"%s%s\",\n", hex(hb, HEADER_SIZE).c_str(), hex(ab, ACK_SIZE).c_str());
  SyncReq s;
  s.lastCounter[0] = 10;
  s.lastCounter[1] = 0;
  s.lastCounter[2] = 7;
  s.lastCounter[3] = 2;
  s.flags = SYNC_ON_TRIP | (4 << SYNC_CHECK_SHIFT);
  s.battery = 64;
  s.build = 0x2a;
  writeSyncReq(s, sb);
  Header syncH;
  syncH.type = Type::SyncReq;
  syncH.group = 0x42;
  syncH.sender = 2;
  syncH.recipient = SERVER;
  syncH.counter = 4;
  writeHeader(syncH, hb);
  std::fprintf(f, "  \"syncReq\": \"%s%s\",\n", hex(hb, HEADER_SIZE).c_str(), hex(sb, SYNC_REQ_SIZE).c_str());
  uint8_t fb[FETCH_SIZE];
  writeFetch(Fetch{1, 10, 13}, fb);
  Header fetchH = syncH;
  fetchH.type = Type::Fetch;
  fetchH.counter = 5;
  writeHeader(fetchH, hb);
  std::fprintf(f, "  \"fetch\": \"%s%s\"\n}\n", hex(hb, HEADER_SIZE).c_str(), hex(fb, FETCH_SIZE).c_str());
  std::fclose(f);
}

int main(int argc, char** argv) {
  testHeader();
  testBodies();
  testGroups();
  testSealOpen();
  testPairKey();
  testRadioRules();
  testCivilTime();
  if (argc > 1) writeVectors(argv[1]);
  std::printf(failures ? "%d FAILED\n" : "all mesh tests passed\n", failures);
  return failures ? 1 : 0;
}
