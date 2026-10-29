// Host tests for lib/WaddleMesh. Run: bash test/mesh/run.sh
// With an output path as argument it also writes the shared test vectors
// (protocol/vectors.json) that the Python server tests check against.

#include <GatewayStore.h>
#include <MeshControl.h>
#include <MeshCrypto.h>
#include <MeshPacket.h>
#include <MeshRadioRules.h>
#include <MeshRoster.h>
#include <monocypher.h>

#include "../../src/services/mesh/MeshTime.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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
  h.network = 0x42;
  h.sender = sender;
  h.recipient = recipient;
  h.counter = counter;
  return h;
}

static void testHeader() {
  Header h = msgHeader(2, EVERYONE, 0x01020304);
  h.hops = 2;
  uint8_t buf[HEADER_SIZE];
  CHECK(writeHeader(h, buf) == HEADER_SIZE);
  Header r;
  CHECK(readHeader(buf, sizeof(buf), r));
  CHECK(r.type == Type::Msg && r.network == 0x42 && r.hops == 2 && r.sender == 2 && r.recipient == EVERYONE);
  CHECK(r.counter == 0x01020304 && r.pieceCount == 1 && !r.fragmented);

  uint8_t bad[HEADER_SIZE];
  memcpy(bad, buf, sizeof(bad));
  bad[0] = 0;
  CHECK(!readHeader(bad, sizeof(bad), r));  // wrong magic
  memcpy(bad, buf, sizeof(bad));
  bad[1] = 0x39;
  CHECK(!readHeader(bad, sizeof(bad), r));  // unknown type
  memcpy(bad, buf, sizeof(bad));
  bad[1] = 0x21;
  CHECK(!readHeader(bad, sizeof(bad), r));  // version 2
  memcpy(bad, buf, sizeof(bad));
  bad[4] = 33;
  CHECK(!readHeader(bad, sizeof(bad), r));  // no member 33
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
  s.rosterVersion = 7;
  s.lastCounter[31] = 12;  // member 32
  uint8_t sb[SYNC_REQ_MAX];
  const size_t sn = writeSyncReq(s, sb);
  CHECK(sn == SYNC_REQ_HEAD + 3 * 5);  // only the members with counters
  SyncReq t;
  CHECK(readSyncReq(sb, sn, t) && t.lastCounter[0] == 5 && t.lastCounter[1] == 0 && t.lastCounter[2] == 900 &&
        t.lastCounter[31] == 12 && t.battery == 40 && t.build == 0xab && t.flags == 3 && t.rosterVersion == 7);
  CHECK(!readSyncReq(sb, sn - 1, t));
}

static void testRoster() {
  uint8_t seed[32], other[32], sign[SIGN_KEY_SIZE], otherSign[SIGN_KEY_SIZE];
  fill(seed, 32, 1);
  fill(other, 32, 2);
  signPublicKey(seed, sign);
  signPublicKey(other, otherSign);
  Roster r;
  r.network = 9;
  r.version = 12;
  for (uint8_t n : {1, 5, 32}) {
    RosterEntry e;
    e.number = n;
    e.admin = n == 1;
    e.name = n == 32 ? std::string(30, 'x') : "Ola";  // cut to 20
    fill(e.publicKey, KEY_SIZE, n);
    r.members.push_back(e);
  }
  auto bytes = writeRoster(r, seed);
  Roster got;
  CHECK(readRoster(bytes.data(), bytes.size(), sign, got));
  CHECK(got.network == 9 && got.version == 12 && got.members.size() == 3 && got.member(5) && !got.member(2));
  CHECK(got.member(1)->admin && !got.member(5)->admin && got.member(32)->name.size() == ROSTER_NAME);
  CHECK(got.everyone() == (memberBit(1) | memberBit(5) | memberBit(32)));
  CHECK(!readRoster(bytes.data(), bytes.size(), otherSign, got));  // not the admin's
  bytes[3] ^= 1;                                                     // version changed
  CHECK(!readRoster(bytes.data(), bytes.size(), sign, got));

  // The unit and the admin get the same join key.
  uint8_t unitSecret[KEY_SIZE], unitPublic[KEY_SIZE], adminSecret[KEY_SIZE], adminPublic[KEY_SIZE];
  uint8_t password[PASSWORD_SIZE], k1[KEY_SIZE], k2[KEY_SIZE];
  fill(unitSecret, KEY_SIZE, 3);
  fill(adminSecret, KEY_SIZE, 4);
  fill(password, PASSWORD_SIZE, 5);
  publicKey(unitSecret, unitPublic);
  publicKey(adminSecret, adminPublic);
  joinKey(unitSecret, unitPublic, adminPublic, password, k1);
  uint8_t input[KEY_SIZE + PASSWORD_SIZE + 2 * KEY_SIZE];
  crypto_x25519(input, adminSecret, unitPublic);
  memcpy(input + KEY_SIZE, password, PASSWORD_SIZE);
  memcpy(input + KEY_SIZE + PASSWORD_SIZE, unitPublic, KEY_SIZE);
  memcpy(input + 2 * KEY_SIZE + PASSWORD_SIZE, adminPublic, KEY_SIZE);
  crypto_blake2b(k2, KEY_SIZE, input, sizeof(input));
  CHECK(memcmp(k1, k2, KEY_SIZE) == 0);
}

static void testSetupBox() {
  uint8_t unitSecret[KEY_SIZE], unitPublic[KEY_SIZE], toolSecret[KEY_SIZE], toolPublic[KEY_SIZE];
  fill(unitSecret, KEY_SIZE, 5);
  fill(toolSecret, KEY_SIZE, 50);
  publicKey(unitSecret, unitPublic);
  publicKey(toolSecret, toolPublic);
  uint8_t k1[KEY_SIZE], k2[KEY_SIZE];
  boxKey(unitSecret, unitPublic, toolPublic, k1);
  // The tool's side: X25519(tool secret, unit public) gives the same shared secret.
  uint8_t input[3 * KEY_SIZE];
  crypto_x25519(input, toolSecret, unitPublic);
  memcpy(input + KEY_SIZE, toolPublic, KEY_SIZE);
  memcpy(input + 2 * KEY_SIZE, unitPublic, KEY_SIZE);
  crypto_blake2b(k2, KEY_SIZE, input, sizeof(input));
  CHECK(memcmp(k1, k2, KEY_SIZE) == 0);
  const char* json = "{\"group\":66}";
  uint8_t box[64], back[64];
  sealBox(k1, 2, reinterpret_cast<const uint8_t*>(json), strlen(json), box);
  CHECK(openBox(k1, 2, box, strlen(json) + TAG_SIZE, back) && memcmp(back, json, strlen(json)) == 0);
  CHECK(!openBox(k1, 1, box, strlen(json) + TAG_SIZE, back));  // other nonce
  box[0] ^= 1;
  CHECK(!openBox(k1, 2, box, strlen(json) + TAG_SIZE, back));
  uint8_t toolNonce[PAIR_NONCE_SIZE], unitNonce[PAIR_NONCE_SIZE];
  fill(toolNonce, sizeof(toolNonce), 20);
  fill(unitNonce, sizeof(unitNonce), 40);
  char code[8], other[8];
  pairCode(toolPublic, unitPublic, toolNonce, unitNonce, code);
  CHECK(strlen(code) == 7 && code[3] == ' ');
  unitNonce[0] ^= 1;
  pairCode(toolPublic, unitPublic, toolNonce, unitNonce, other);
  CHECK(strcmp(code, other) != 0);
}

static void testGroups() {
  // Member 32 exists; member 33 does not.
  uint8_t raw[HEADER_SIZE];
  Header h = msgHeader(32, EVERYONE, 1), r;
  writeHeader(h, raw);
  CHECK(readHeader(raw, sizeof(raw), r) && r.sender == 32 && r.key == 0 && r.groupId == 0);
  raw[4] = 33;
  CHECK(!readHeader(raw, sizeof(raw), r));

  // Kari's (member 2) third group, key number 4.
  const uint16_t id = groupId(2, 3);
  CHECK(id == 0x0203 && groupCreator(id) == 2);
  h = msgHeader(2, TO_GROUP, 7);
  h.groupId = id;
  h.key = 4;
  writeHeader(h, raw);
  CHECK(readHeader(raw, sizeof(raw), r) && r.recipient == TO_GROUP && r.groupId == id && r.key == 4);
  put16(raw + 11, groupId(40, 0));  // made by member 40, who cannot exist
  CHECK(!readHeader(raw, sizeof(raw), r));
  put16(raw + 11, 0);  // a group packet needs an id
  CHECK(!readHeader(raw, sizeof(raw), r));

  // Groups go to everyone; DMs only to the two.
  CHECK(canRead(3, 1, TO_GROUP) && canRead(3, 1, EVERYONE) && canRead(2, 1, 2) && !canRead(3, 1, 2));
  CHECK(memberBit(4) == 8 && memberBit(32) == 0x80000000u && memberBit(0) == 0 && memberCount(0x0F) == 4);

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
  const Header h = msgHeader(1, EVERYONE, 10);
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
  dmKey(aSecret, aPublic, bPublic, ab);
  dmKey(bSecret, bPublic, aPublic, ba);
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
  // Any number of small packets, as long as their time fits.
  AirtimeBudget hour(216000);
  for (uint32_t i = 0; i < 400; i++) {
    CHECK(hour.allows(219, i * 5000));
    hour.spend(219, i * 5000);
  }
  CHECK(hour.usedLastHour(399 * 5000) < 216000);

  SeenPackets seen;
  Header h = msgHeader(1, EVERYONE, 5);
  CHECK(seen.remember(h));
  CHECK(!seen.remember(h));
  h.pieceIndex = 1;
  h.pieceCount = 2;
  CHECK(seen.remember(h));
  // A forged packet with a copied header does not hide the real one.
  const uint8_t real[] = {1, 2, 3}, forged[] = {9, 9, 9};
  h.counter = 6;
  CHECK(seen.remember(h, forged, sizeof(forged)));
  CHECK(seen.remember(h, real, sizeof(real)));
  CHECK(!seen.remember(h, real, sizeof(real)));

  uint32_t save = 0;
  CounterReserve fresh(0);
  CHECK(fresh.take(&save) == 1 && save == CounterReserve::STEP);
  CHECK(fresh.take(&save) == 2 && save == 0);
  CounterReserve rebooted(CounterReserve::STEP);
  CHECK(rebooted.take(&save) == CounterReserve::STEP + 1 && save == 2 * CounterReserve::STEP);
}

// A packet as a unit sends it: header and a body of n bytes.
static RawPacket unitPacket(Header h, const uint8_t* body, size_t n) {
  RawPacket p;
  writeHeader(h, p.data);
  memcpy(p.data + HEADER_SIZE, body, n);
  p.len = uint8_t(HEADER_SIZE + n);
  return p;
}

static RawPacket msgPacket(uint8_t sender, uint8_t recipient, uint32_t counter, uint8_t piece = 0, uint8_t pieces = 1) {
  Header h = msgHeader(sender, recipient, counter);
  h.fragmented = pieces > 1;
  h.pieceIndex = piece;
  h.pieceCount = pieces;
  uint8_t body[20] = {};
  return unitPacket(h, body, sizeof(body));
}

static RawPacket syncPacket(uint8_t sender, uint32_t counter, const SyncReq& s) {
  Header h = msgHeader(sender, SERVER, counter);
  h.type = Type::SyncReq;
  uint8_t body[SYNC_REQ_MAX];
  return unitPacket(h, body, writeSyncReq(s, body));
}

static void testGatewayStore() {
  static GatewayStore store(0x42, 0);
  static GatewayStore::Result r;
  auto feed = [&](const RawPacket& p) { store.handle(p.data, p.len, r); };
  auto header = [](const RawPacket& p) {
    Header h;
    readHeader(p.data, p.len, h);
    return h;
  };

  // A whole message is kept and answered with "stored", from counter 0x80000001.
  feed(msgPacket(1, EVERYONE, 10));
  CHECK(r.savedSlot >= 0 && r.count == 1 && store.count() == 1);
  CHECK(r.saveCounter == GatewayStore::FIRST_COUNTER + CounterReserve::STEP);
  Header ah = header(r.packets[0]);
  Ack a;
  CHECK(ah.type == Type::Ack && ah.sender == SERVER && ah.recipient == 1 && ah.counter == 0x80000001u);
  CHECK(readAck(r.packets[0].data + HEADER_SIZE, ACK_SIZE, a) && a.sender == 1 && a.counter == 10 &&
        a.state == AckState::Stored);
  // The same packet again: not kept twice.
  feed(msgPacket(1, EVERYONE, 10));
  CHECK(r.savedSlot == -1 && store.count() == 1 && r.saveCounter == 0);

  // A message in two pieces is acked once both are here.
  feed(msgPacket(2, 3, 20, 1, 2));
  CHECK(r.count == 0 && r.savedSlot >= 0);
  feed(msgPacket(2, 3, 20, 0, 2));
  CHECK(r.count == 1);

  // Other meshes, the server's own packets and unknown groups are ignored.
  RawPacket other = msgPacket(1, EVERYONE, 11);
  other.data[2] = 0x43;
  feed(other);
  CHECK(r.savedSlot == -1 && r.count == 0);
  RawPacket server = msgPacket(1, EVERYONE, 12);
  server.data[4] = SERVER;
  feed(server);
  CHECK(r.savedSlot == -1);
  GatewayStore unknown(0, 0);
  unknown.handle(msgPacket(1, EVERYONE, 13).data, msgPacket(1, EVERYONE, 13).len, r);
  CHECK(r.savedSlot == -1 && unknown.count() == 0);
  // Once the Pi said who is in the network, others are ignored too.
  GatewayStore known(0x42, 0);
  known.setMembers(memberBit(1) | memberBit(2));
  known.handle(msgPacket(5, EVERYONE, 14).data, msgPacket(5, EVERYONE, 14).len, r);
  CHECK(r.savedSlot == -1 && known.count() == 0);
  known.handle(msgPacket(2, EVERYONE, 14).data, msgPacket(2, EVERYONE, 14).len, r);
  CHECK(r.savedSlot >= 0 && known.count() == 1);

  // A sync from member 3: member 1's group message and member 2's DM to 3, in
  // order, with hops 0. Member 4 does not get the DM.
  RawPacket groupMsg = msgPacket(1, TO_GROUP, 11);
  put16(groupMsg.data + 11, groupId(1, 0));
  feed(groupMsg);
  SyncReq s;
  feed(syncPacket(3, 5, s));
  CHECK(r.count == 4);
  CHECK(header(r.packets[0]).counter == 10 && header(r.packets[1]).counter == 11);
  CHECK(header(r.packets[2]).sender == 2 && header(r.packets[2]).pieceIndex == 0);
  CHECK(header(r.packets[3]).pieceIndex == 1 && header(r.packets[3]).hops == 0);
  feed(syncPacket(4, 5, s));
  CHECK(r.count == 2);
  s.lastCounter[0] = 10;
  feed(syncPacket(4, 6, s));
  CHECK(r.count == 1 && header(r.packets[0]).counter == 11);

  // At most 12 packets per answer.
  for (uint32_t c = 100; c < 130; c++) feed(msgPacket(4, EVERYONE, c));
  SyncReq none;
  feed(syncPacket(3, 7, none));
  CHECK(r.count == GatewayStore::MAX_ANSWER);

  // A fetch gives only the range asked for.
  Header fh = msgHeader(3, SERVER, 8);
  fh.type = Type::Fetch;
  uint8_t fb[FETCH_SIZE];
  writeFetch(Fetch{4, 100, 104}, fb);
  feed(unitPacket(fh, fb, sizeof(fb)));
  CHECK(r.count == 3 && header(r.packets[0]).counter == 101 && header(r.packets[2]).counter == 103);

  // Positions: only each member's newest is kept, and it comes with a sync.
  Header ph = msgHeader(2, EVERYONE, 50);
  ph.type = Type::Pos;
  uint8_t pb[POS_SIZE + TAG_SIZE] = {};
  const int before = store.count();
  feed(unitPacket(ph, pb, sizeof(pb)));
  ph.counter = 51;
  feed(unitPacket(ph, pb, sizeof(pb)));
  ph.counter = 49;  // an older one, heard late
  feed(unitPacket(ph, pb, sizeof(pb)));
  CHECK(r.savedSlot == -1 && store.count() == before + 1);
  SyncReq caughtUp;
  for (uint32_t& c : caughtUp.lastCounter) c = 1000;
  feed(syncPacket(1, 9, caughtUp));
  CHECK(r.count == 1 && header(r.packets[0]).type == Type::Pos && header(r.packets[0]).counter == 51);

  // An SOS is always answered; an ACK for member 1's message comes with its sync.
  Header sh = msgHeader(2, EVERYONE, 60);
  sh.type = Type::Sos;
  feed(unitPacket(sh, pb, sizeof(pb)));
  CHECK(r.count == 1 && r.savedSlot >= 0);
  Header kh = msgHeader(3, 1, 70);
  kh.type = Type::Ack;
  uint8_t kb[ACK_SIZE];
  writeAck(Ack{1, 10, AckState::Read}, kb);
  feed(unitPacket(kh, kb, sizeof(kb)));
  CHECK(r.savedSlot >= 0 && r.count == 0);
  feed(syncPacket(1, 10, caughtUp));
  CHECK(r.count == 2);  // member 2's position and SOS
  caughtUp.lastCounter[0] = 10;
  feed(syncPacket(1, 11, caughtUp));
  CHECK(r.count == 3 && header(r.packets[2]).type == Type::Ack);

  // Relaying: once, with one hop less; never a sync request.
  RawPacket out;
  const RawPacket m = msgPacket(1, EVERYONE, 200);
  CHECK(store.relay(m.data, m.len, out) && header(out).hops == DEFAULT_HOPS - 1);
  CHECK(memcmp(out.data + 4, m.data + 4, m.len - 4) == 0);
  CHECK(!store.relay(m.data, m.len, out));
  const RawPacket sync = syncPacket(1, 12, s);
  CHECK(!store.relay(sync.data, sync.len, out));

  // After a restart: the slots come back from flash, counters go on past the saved limit.
  static GatewayStore again(0x42, GatewayStore::FIRST_COUNTER + CounterReserve::STEP);
  for (int i = 0; i < GatewayStore::SLOTS; i++) again.restore(i, store.slot(i));
  CHECK(again.count() == store.count());
  GatewayStore::Slot broken = store.slot(0);
  broken.packet.data[0] = 0;
  again.restore(0, broken);  // a half-written slot is skipped
  again.handle(msgPacket(1, EVERYONE, 300).data, msgPacket(1, EVERYONE, 300).len, r);
  CHECK(r.count == 1 && header(r.packets[0]).counter == GatewayStore::FIRST_COUNTER + CounterReserve::STEP + 1);
  CHECK(r.saveCounter == GatewayStore::FIRST_COUNTER + 2 * CounterReserve::STEP);

  // Full: the oldest makes room. Cleared: empty.
  for (uint32_t c = 1000; c < 1000 + GatewayStore::SLOTS; c++) feed(msgPacket(3, EVERYONE, c));
  CHECK(store.count() == GatewayStore::SLOTS);
  feed(syncPacket(2, 13, none));
  CHECK(header(r.packets[0]).sender == 3);  // member 1's old messages are gone
  store.clear();
  CHECK(store.count() == 0);
}

// Fixed inputs and the packets they give, for the Python side.
static std::vector<std::pair<const char*, ControlMsg>> controlExamples();

static void writeVectors(const char* path) {
  FILE* f = std::fopen(path, "w");
  if (!f) {
    failures++;
    return;
  }
  uint8_t aSecret[KEY_SIZE], bSecret[KEY_SIZE], aPublic[KEY_SIZE], bPublic[KEY_SIZE], dm[KEY_SIZE];
  fill(aSecret, KEY_SIZE, 0x10);
  fill(bSecret, KEY_SIZE, 0x60);
  publicKey(aSecret, aPublic);
  publicKey(bSecret, bPublic);
  dmKey(aSecret, aPublic, bPublic, dm);
  uint8_t senderKey[KEY_SIZE];
  fill(senderKey, KEY_SIZE, 1);

  std::fprintf(f, "{\n  \"note\": \"Written by test/mesh/run.sh from the C++ code. Do not edit.\",\n");
  std::fprintf(f, "  \"senderKey\": \"%s\",\n", hex(senderKey, KEY_SIZE).c_str());
  std::fprintf(f, "  \"memberA\": {\"secret\": \"%s\", \"public\": \"%s\"},\n", hex(aSecret, KEY_SIZE).c_str(),
               hex(aPublic, KEY_SIZE).c_str());
  std::fprintf(f, "  \"memberB\": {\"secret\": \"%s\", \"public\": \"%s\"},\n", hex(bSecret, KEY_SIZE).c_str(),
               hex(bPublic, KEY_SIZE).c_str());
  std::fprintf(f, "  \"dmKey\": \"%s\",\n", hex(dm, KEY_SIZE).c_str());
  // Setup handover: member A's unit and a tool with a fixed one-time key.
  uint8_t toolSecret[KEY_SIZE], toolPublic[KEY_SIZE], setupBoxKey[KEY_SIZE];
  fill(toolSecret, KEY_SIZE, 77);
  publicKey(toolSecret, toolPublic);
  boxKey(aSecret, aPublic, toolPublic, setupBoxKey);
  uint8_t box[KEY_SIZE + TAG_SIZE];
  sealBox(setupBoxKey, 1, senderKey, KEY_SIZE, box);
  uint8_t toolNonce[PAIR_NONCE_SIZE], unitNonce[PAIR_NONCE_SIZE], commit[32];
  fill(toolNonce, sizeof(toolNonce), 20);
  fill(unitNonce, sizeof(unitNonce), 40);
  pairCommit(toolPublic, toolNonce, commit);
  char code[8];
  pairCode(toolPublic, aPublic, toolNonce, unitNonce, code);
  std::fprintf(f,
               "  \"setup\": {\"toolSecret\": \"%s\", \"key\": \"%s\", \"senderKeyBox\": \"%s\", \"toolNonce\": \"%s\", "
               "\"unitNonce\": \"%s\", \"commit\": \"%s\", \"code\": \"%s\"},\n",
               hex(toolSecret, KEY_SIZE).c_str(), hex(setupBoxKey, KEY_SIZE).c_str(), hex(box, sizeof(box)).c_str(),
               hex(toolNonce, sizeof(toolNonce)).c_str(), hex(unitNonce, sizeof(unitNonce)).c_str(),
               hex(commit, sizeof(commit)).c_str(), code);

  uint8_t groupKey[KEY_SIZE];
  fill(groupKey, KEY_SIZE, 3);
  std::fprintf(f, "  \"groupKey\": \"%s\",\n", hex(groupKey, KEY_SIZE).c_str());
  // Control DMs (the text inside a sealed DM).
  std::fprintf(f, "  \"control\": [\n");
  const auto examples = controlExamples();
  for (size_t i = 0; i < examples.size(); i++) {
    const std::string text = writeControl(examples[i].second);
    std::fprintf(f, "    {\"name\": \"%s\", \"text\": \"%s\"}%s\n", examples[i].first,
                 hex(reinterpret_cast<const uint8_t*>(text.data()), text.size()).c_str(),
                 i + 1 < examples.size() ? "," : "");
  }
  std::fprintf(f, "  ],\n");

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

  Header gh = msgHeader(1, EVERYONE, 10);
  Header dh = msgHeader(1, 2, 11);
  Header ph = msgHeader(1, EVERYONE, 12);
  ph.type = Type::Pos;
  // A group made by member 1 (its first), under the group's key number 2.
  Header sh = msgHeader(1, TO_GROUP, 13);
  sh.groupId = groupId(1, 0);
  sh.key = 2;
  const Case cases[] = {{"group message", gh, senderKey, groupPlain},
                        {"direct message in 3 pieces", dh, dm, dmPlain},
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

  uint8_t ab[ACK_SIZE], sb[SYNC_REQ_MAX], hb[HEADER_SIZE];
  writeAck(Ack{1, 10, AckState::Stored}, ab);
  Header ackH;
  ackH.type = Type::Ack;
  ackH.network = 0x42;
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
  s.rosterVersion = 3;
  const size_t sn = writeSyncReq(s, sb);
  Header syncH;
  syncH.type = Type::SyncReq;
  syncH.network = 0x42;
  syncH.sender = 2;
  syncH.recipient = SERVER;
  syncH.counter = 4;
  writeHeader(syncH, hb);
  std::fprintf(f, "  \"syncReq\": \"%s%s\",\n", hex(hb, HEADER_SIZE).c_str(), hex(sb, sn).c_str());
  uint8_t fb[FETCH_SIZE];
  writeFetch(Fetch{1, 10, 13}, fb);
  Header fetchH = syncH;
  fetchH.type = Type::Fetch;
  fetchH.counter = 5;
  writeHeader(fetchH, hb);
  std::fprintf(f, "  \"fetch\": \"%s%s\",\n", hex(hb, HEADER_SIZE).c_str(), hex(fb, FETCH_SIZE).c_str());

  // A roster signed by an admin (seed 0x60...), and a join key.
  uint8_t adminSeed[32], adminSign[SIGN_KEY_SIZE];
  fill(adminSeed, 32, 0x60);
  signPublicKey(adminSeed, adminSign);
  Roster roster;
  roster.network = 0x42;
  roster.version = 3;
  RosterEntry a, b;
  a.number = 1;
  a.admin = true;
  a.name = "Arne";
  memcpy(a.publicKey, aPublic, KEY_SIZE);
  fill(a.tokenHash, TOKEN_HASH_SIZE, 0xA0);
  b.number = 7;
  b.name = "Kåre";
  memcpy(b.publicKey, bPublic, KEY_SIZE);
  fill(b.tokenHash, TOKEN_HASH_SIZE, 0xB0);
  roster.members = {a, b};
  const auto signedRoster = writeRoster(roster, adminSeed);
  uint8_t password[PASSWORD_SIZE], adminBoxSecret[KEY_SIZE], adminBox[KEY_SIZE], jk[KEY_SIZE];
  fill(password, PASSWORD_SIZE, 0x11);
  fill(adminBoxSecret, KEY_SIZE, 0x70);
  publicKey(adminBoxSecret, adminBox);
  joinKey(bSecret, bPublic, adminBox, password, jk);
  char jc[8];
  joinCode(adminBox, bPublic, jc);
  std::fprintf(f, "  \"roster\": {\"adminSeed\": \"%s\", \"adminSign\": \"%s\", \"signed\": \"%s\"},\n",
               hex(adminSeed, 32).c_str(), hex(adminSign, SIGN_KEY_SIZE).c_str(),
               hex(signedRoster.data(), signedRoster.size()).c_str());
  std::fprintf(f, "  \"join\": {\"password\": \"%s\", \"adminBoxSecret\": \"%s\", \"key\": \"%s\", \"code\": \"%s\"}\n}\n",
               hex(password, PASSWORD_SIZE).c_str(), hex(adminBoxSecret, KEY_SIZE).c_str(), hex(jk, KEY_SIZE).c_str(), jc);
  std::fclose(f);
}

// One of each control DM, as protocol/vectors.json "control" has them.
static std::vector<std::pair<const char*, ControlMsg>> controlExamples() {
  ControlMsg s, g, l, r, k;
  s.kind = Control::SenderKey;
  s.keyNumber = 3;
  fill(s.key, KEY_SIZE, 1);
  g.kind = Control::Invite;
  g.groupId = groupId(2, 5);
  g.keyNumber = 1;
  g.fresh = true;
  g.members = memberBit(2) | memberBit(9) | memberBit(32);
  fill(g.key, KEY_SIZE, 3);
  g.name = "Hytta";
  l.kind = Control::Leave;
  l.groupId = groupId(2, 5);
  r.kind = Control::Remove;
  r.groupId = groupId(2, 5);
  r.member = 9;
  k.kind = Control::AskKey;
  k.groupId = 0;
  k.keyNumber = 3;
  return {{"sender key", s}, {"invitation", g}, {"leave", l}, {"remove", r}, {"ask for a key", k}};
}

static void testControl() {
  for (const auto& [name, c] : controlExamples()) {
    const std::string text = writeControl(c);
    ControlMsg back;
    CHECK(isControl(text) && readControl(text, back));
    CHECK(back.kind == c.kind && back.groupId == c.groupId && back.keyNumber == c.keyNumber &&
          back.member == c.member && back.fresh == c.fresh && back.members == c.members && back.name == c.name &&
          memcmp(back.key, c.key, KEY_SIZE) == 0);
    // Cut short: refused.
    CHECK(!readControl(text.substr(0, 4), back));
    (void)name;
  }
  ControlMsg back;
  CHECK(!readControl(std::string("\0X\0\0\0", 5), back));  // unknown kind
  CHECK(!readControl("Hei", back));                            // a message, not a control DM
}

int main(int argc, char** argv) {
  testHeader();
  testBodies();
  testGroups();
  testSetupBox();
  testRoster();
  testSealOpen();
  testPairKey();
  testRadioRules();
  testCivilTime();
  testGatewayStore();
  testControl();
  if (argc > 1) writeVectors(argv[1]);
  std::printf(failures ? "%d FAILED\n" : "all mesh tests passed\n", failures);
  return failures ? 1 : 0;
}
