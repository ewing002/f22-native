#include "aibridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "aiunit.h"
#include "sim.h"

namespace sim {

namespace {
constexpr uint32_t ROUTE_BASE = 0x1000000, TAXI_BASE = 0x2000000;
constexpr int32_t NONE = int32_t(0x80000000);
int32_t rawOf(double wu) { return int32_t(uint32_t(int64_t(std::llround(wu * 65536.0)))); }   // 16.16, the world wraps around (2^32 = 65536 wu)
double wrapRad(double a) { const double P = 3.14159265358979323846; return a - 2 * P * std::floor((a + P) / (2 * P)); }
}  // namespace

struct AiBridge::Impl {
  const std::vector<uint8_t>& img;
  AiVm vm;
  OWorld ow;
  OHost host;
  OGroups groups;
  std::vector<OUnit> recs;
  std::vector<uint32_t> ptr;
  std::vector<uint8_t> driven, died;
  std::vector<uint8_t> routeMem, taxiMem;
  std::vector<uint32_t> routeOff, taxiOff;
  World* W = nullptr;
  size_t cur = 0;
  explicit Impl(const std::vector<uint8_t>& i) : img(i), vm(i) {}

  static void put32(std::vector<uint8_t>& m, size_t o, int32_t v) { std::memcpy(&m[o], &v, 4); }
  static void put16(std::vector<uint8_t>& m, size_t o, int v) { uint16_t x = uint16_t(v); std::memcpy(&m[o], &x, 2); }
  int32_t i32(uint32_t a) const { int32_t v = 0; if (a + 4 <= img.size()) std::memcpy(&v, &img[a], 4); return v; }
  size_t idxOf(const Unit* u) const { return size_t(u - W->units.data()); }
  uint32_t ptrOf(const Unit* u) const { return u ? ptr[idxOf(u)] : 0; }
  Unit* unitAt(uint32_t p) const {
    if (p < OWorld::BASE) return nullptr;
    size_t i = (p - OWorld::BASE) / OUnit::SIZE;
    return i < W->units.size() ? &W->units[i] : nullptr;
  }

  void buildMemory() {
    routeMem.assign(16, 0); taxiMem.assign(12, 0);
    routeOff.clear(); taxiOff.clear();
    for (auto& r : W->routes_) {
      routeOff.push_back(uint32_t(routeMem.size()));
      size_t o = routeMem.size();
      routeMem.resize(o + r.size() * 16);
      for (size_t k = 0; k < r.size(); k++) {
        put32(routeMem, o + 16 * k, rawOf(r[k].pos.x)); put32(routeMem, o + 16 * k + 4, rawOf(r[k].pos.y)); put32(routeMem, o + 16 * k + 8, rawOf(r[k].pos.z));
        put16(routeMem, o + 16 * k + 12, r[k].action); put16(routeMem, o + 16 * k + 14, r[k].param);
      }
    }
    for (auto& t : W->taxis_) {
      taxiOff.push_back(uint32_t(taxiMem.size()));
      size_t o = taxiMem.size();
      taxiMem.resize(o + t.size() * 12);
      for (size_t k = 0; k < t.size(); k++) {
        put32(taxiMem, o + 12 * k, rawOf(t[k].x)); put32(taxiMem, o + 12 * k + 4, rawOf(t[k].z));
        put16(taxiMem, o + 12 * k + 8, t[k].action); put16(taxiMem, o + 12 * k + 10, t[k].param);
      }
    }
  }

  void setupHost() {
    host = OHost();
    host.img = &img;
    host.world = &ow;
    host.groundH = [this](int32_t x, int32_t z) { return rawOf(W->groundWU(double(uint32_t(x)) / 65536.0, double(uint32_t(z)) / 65536.0)); };
    host.groundI = [this](int32_t x, int32_t z) { return rawOf(W->groundInterpWU(double(uint32_t(x)) / 65536.0, double(uint32_t(z)) / 65536.0)); };
    host.wpAt = [this](uint32_t p, int32_t& x, int32_t& y, int32_t& z) {
      x = y = z = 0;
      if (p >= TAXI_BASE) { size_t o = p - TAXI_BASE; if (o + 8 <= taxiMem.size()) { std::memcpy(&x, &taxiMem[o], 4); std::memcpy(&z, &taxiMem[o + 4], 4); } }
      else if (p >= ROUTE_BASE) { size_t o = p - ROUTE_BASE; if (o + 12 <= routeMem.size()) { std::memcpy(&x, &routeMem[o], 4); std::memcpy(&y, &routeMem[o + 4], 4); std::memcpy(&z, &routeMem[o + 8], 4); } }
    };
    host.wpWord = [this](uint32_t p, uint32_t off) {
      uint16_t v = 0;
      if (p >= TAXI_BASE) { size_t o = p - TAXI_BASE + off; if (o + 2 <= taxiMem.size()) std::memcpy(&v, &taxiMem[o], 2); }
      else if (p >= ROUTE_BASE) { size_t o = p - ROUTE_BASE + off; if (o + 2 <= routeMem.size()) std::memcpy(&v, &routeMem[o], 2); }
      return int32_t(v);
    };
    host.perfOf = [this](const OUnit& u) { return &img[0x24618 + 0x50 * size_t(std::min<int>(u.get8(of::ROLE), 9))]; };
    host.slotAt = [this](const OUnit& u, int32_t& a, int32_t& b) { uint32_t p = uint32_t(u.get(of::SLOT_REC)); a = p ? i32(p + 0xc) : 0; b = p ? i32(p + 0x10) : 0; };
    host.slotVec = [this](const OUnit& u, int32_t v[3]) { uint32_t p = uint32_t(u.get(of::SLOT_REC)); for (int k = 0; k < 3; k++) v[k] = p ? i32(p + 4 * k) : 0; };
    host.groupLeader = [this](int idx) { return idx >= 0 && idx < OGroups::N ? groups.leader[idx] : 0u; };
    host.spawnShot = [this](bool flare) {                      // 0x3a5da / 0x3a722: decoy
      Unit& u = W->units[cur];
      W->decoys.push_back({u.pos, 200, flare ? 0 : 1, W->now});
      W->snd(SND_FLARE, u.pos, 0xff);
    };
    host.radio = [this](int n) {                               // 0x37169: only if the aircraft's leader is the player
      OUnit& r = recs[cur];
      if (!W->player || uint32_t(r.get(of::LEADER)) != ptrOf(W->player) || n < 0 || n > 4) return;
      uint32_t tp = uint32_t(i32(0x36bc8 + 4 * uint32_t(n)));
      std::string text;
      for (uint32_t a = tp; a < img.size() && img[a] && text.size() < 80; a++) text += char(img[a]);
      uint16_t snd = 0; std::memcpy(&snd, &img[0x36bf8 + 2 * size_t(n)], 2);
      if (!text.empty()) W->say(text, 4);
      W->snd(int(snd), W->player->pos, 0xC8, 1, false);
    };
    host.onEvent = [this](int kind, int cond) {               // 0x23cc0: kind 1 = unit (id), 0 = group
      OUnit& r = recs[cur];
      W->triggerEvent(kind, cond, kind == 1 ? (r.get(of::ID) & 0xffff) : r.get8(of::GROUP_IDX), 0);
    };
    host.wpSetWord = [this](uint32_t p, uint32_t off, int v) {
      if (p >= ROUTE_BASE && p < TAXI_BASE) { size_t o = p - ROUTE_BASE + off; if (o + 2 <= routeMem.size()) put16(routeMem, o, v); }
    };
    host.missionRadio = [this](int idx) {                      // 0x27366 -> 0x32575 (text) + 0x239f8 (WAV)
      if (idx < 0 || idx >= int(W->radio.size())) return;
      W->say(W->radio[idx].text, 6);
      if (!W->radio[idx].wav.empty()) W->snd(SND_RADIO, W->player->pos, 0xFF, 1, false, W->radio[idx].wav);
    };
    host.launchSam = [this](OUnit& l, uint32_t lp) {           // 0x39b95: SAM from the launcher (63.3° climb, 0 initial speed - per the native fire)
      Unit* lu = unitAt(lp);
      Unit* T = unitAt(uint32_t(l.get(of::TARGET)));
      if (!lu || !T || lu->dead) return;
      D3 d = wdelta(T->pos, lu->pos);
      lu->lastLaunch = W->stepNo;
      W->fire(lu, 4, T, std::atan2(d.x, d.z), 0x5A0000 * ANG, 0, false);
    };
    host.aaaBurst = [this](int32_t h, int32_t p) {             // 0x3a345: anti-aircraft burst
      Unit& u = W->units[cur];
      W->fireGun(&u, dirOf(double(uint32_t(h) & 0x1ffffff) * ANG, double(uint32_t(p) & 0x1ffffff) * ANG), {0, 0, 0}, 3 << 16, true);
    };
    host.weapon = [this](int kind) {                           // 0x3a32d cannon, 0x39b6f missile, 0x3a0e4 bomb
      Unit& u = W->units[cur];
      Unit* T = unitAt(uint32_t(recs[cur].get(of::TARGET)));
      const Perf& pf = W->perf[std::min<int>(u.sub, 9)];
      if (kind == 0) W->fireGun(&u, dirOf(u.hdg, u.pitch), u.fwd() * u.speed, W->wdef[0].vmax * 2, false);
      else W->fire(&u, kind == 2 ? 3 : pf.weapon, T, u.hdg, u.pitch, int32_t(u.speed * 65536), true);
    };
  }

  void initUnit(size_t i) {
    Unit& u = W->units[i];
    OUnit& r = recs[i];
    r = OUnit();
    r.set(of::X, rawOf(u.pos.x)); r.set(of::Y, rawOf(u.pos.y)); r.set(of::Z, rawOf(u.pos.z));
    r.set(of::ANG_A, int32_t(uint32_t(std::llround(u.hdg / ANG)) & 0x1ffffff));
    r.set(of::ANG_B, int32_t(uint32_t(std::llround(u.pitch / ANG)) & 0x1ffffff));
    r.set8(of::SIDE_FLAGS, uint8_t(u.side));
    r.set(of::ID, u.id);
    r.set8(of::GROUP_IDX, uint8_t(u.group));
    r.set8(of::ROLE, uint8_t(u.sub));
    r.set8(of::SKILL, uint8_t(u.skill));
    r.set8(of::F1, u.wing ? 1 : 0);
    r.set8(of::F6, uint8_t(u.noAuto));
    r.set8(of::F7, uint8_t(u.tmask));
    r.set8(of::F9, uint8_t(u.escort));
    r.set8(of::GROUP_ID, u.side == 4 ? 0x0a : (u.side == 1 || u.side == 0x10) ? 0 : 0x04);   // whom it hunts (side mask)
    r.set(of::CMD_ANG, NONE); r.set(of::CMD_PITCH, NONE); r.set(of::CMD_ROLL, NONE);
    r.set8(of::STATE, 0);
    r.set(of::LAST_LAUNCH, W->stepNo - 0x2580);               // 0x26b56: the launcher may fire immediately
    r.set(of::TIMER, u.id & 31);                               // 0x26b83: phase (mod 32) for ground units
    if (u.cls != C_AIR || u.isPlayer) {
      r.set8(of::TYPE, u.cls == C_AIR ? 1 : 2);
      if (u.isPlayer) r.set8(of::STATE, 2);
      if (driven[i] && u.active) startGround(i);
      return;
    }
    // initial values of the air AI aircraft (0x26a3a): turn limits from skill level, weapon range, routes
    const uint8_t* pf = &img[0x24618 + 0x50 * size_t(std::min(u.sub, 9))];
    int32_t pv[0x50 / 4]; std::memcpy(pv, pf, 0x50);
    int32_t k = i32(0x36c12 + 4 * uint32_t(u.skill & 3));
    auto mul = [&](int32_t a) { return int32_t((int64_t(a) * k) >> 16); };
    r.set(of::TURN_A, mul(pv[0x14 / 4])); r.set(of::TURN_RATE, mul(pv[0x10 / 4])); r.set(of::TURN_B, mul(pv[0x18 / 4]));
    uint16_t wpn; std::memcpy(&wpn, pf + 0x26, 2);
    r.set(of::MIN_RANGE, int32_t(uint32_t(i32(0x39034 + 0x40 * uint32_t(wpn) + 0x10)) << 16));
    r.set8(of::TYPE, u.onGround ? 2 : 1);
    if (u.route >= 0 && u.route < int(routeOff.size())) { r.set(of::WP_BASE, int32_t(ROUTE_BASE + routeOff[u.route])); r.set(of::WP_PTR, int32_t(ROUTE_BASE + routeOff[u.route])); r.set8(0xab, uint8_t(std::min<size_t>(W->routes_[u.route].size(), 255))); }
    if (u.taxi >= 0 && u.taxi < int(taxiOff.size())) r.set(of::TAXI, int32_t(TAXI_BASE + taxiOff[u.taxi] + 12 * uint32_t(W->nearestTaxiNode(u))));
    // initial state: per the native restart() decision (from the original starting rules)
    r.set(of::SPEED, rawOf(u.speed));
    switch (u.st) {
      case S_TAXI: r.set(of::PENDING, 0x374c8); r.set8(of::STATE, 1); break;
      case S_TAKEOFF: r.set(of::PENDING, 0x374e8); r.set8(of::STATE, 3); break;
      case S_PARKED: break;
      default: r.set(of::PENDING, 0x37510); r.set8(of::STATE, 2); break;
    }
  }

  void startGround(size_t i) {                                // 0x23ae7: a szerep szerinti szkript (0x36ba0)
    OUnit& r = recs[i];
    uint32_t e = uint32_t(i32(0x36ba0 + 4 * uint32_t(std::min(W->units[i].sub, 3))));
    if (!e) return;
    r.set(of::PENDING, int32_t(e));
    r.set8(of::STATE, 1);
  }

  void syncIn(size_t i) {
    Unit& u = W->units[i];
    OUnit& r = recs[i];
    if (u.dead) { r.set8(of::TYPE, 0); r.set8(of::SIDE_FLAGS, 0); return; }
    r.set8(of::SIDE_FLAGS, uint8_t(u.side));
    r.set(of::STEALTH, int32_t(W->stealthOf(u) * 65536));   // +0xa3: detection factor for hidden (+2 & 0x22) targets (0x268a5 / 0x2b6d8)
    if (!driven[i]) {
      r.set(of::X, rawOf(u.pos.x)); r.set(of::Y, rawOf(u.pos.y)); r.set(of::Z, rawOf(u.pos.z));
      r.set(of::ANG_A, int32_t(uint32_t(std::llround(u.hdg / ANG)) & 0x1ffffff));
      r.set(of::SPEED, u.isPlayer ? rawOf(len(W->v)) : rawOf(u.speed));
      r.set8(of::TYPE, u.cls == C_AIR ? (u.onGround ? 2 : 1) : 2);
    } else {
      r.set8(of::TYPE, u.onGround ? 2 : 1);
    }
    r.set(of::TARGET, int32_t(ptrOf(u.isPlayer ? W->lockTgt : u.target)));   // the player's target: the locked target
  }

  void syncOut(size_t i) {
    Unit& u = W->units[i];
    OUnit& r = recs[i];
    u.pos.x = double(uint32_t(r.get(of::X))) / 65536.0;
    u.pos.z = double(uint32_t(r.get(of::Z))) / 65536.0;
    u.pos.y = r.get(of::Y) / 65536.0;
    u.hdg = wrapRad(double(uint32_t(r.get(of::ANG_A)) & 0x1ffffff) * ANG);
    u.pitch = wrapRad(double(uint32_t(r.get(of::ANG_B)) & 0x1ffffff) * ANG);
    u.roll = wrapRad(double(uint32_t(r.get(of::ANG_C)) & 0x1ffffff) * ANG);
    u.speed = r.get(of::SPEED) / 65536.0;
    static const AiState MAP[8] = {S_PARKED, S_TAXI, S_ROUTE, S_TAKEOFF, S_FORM, S_TRACK, S_EVADE, S_PULLUP};
    int st = r.get8(of::STATE) & 7;
    u.st = MAP[st];
    u.onGround = st <= 1 || st == 3;
    u.wasAirborne = !u.onGround;
    u.target = unitAt(uint32_t(r.get(of::TARGET)));
    if (u.target && u.target->dead) u.target = nullptr;
  }
};

AiBridge::AiBridge(const std::vector<uint8_t>* img) : p_(new Impl(*img)) {}
AiBridge::~AiBridge() = default;

bool AiBridge::drives(const Unit& u) const {
  if (!p_->W || p_->driven.empty()) return false;
  size_t i = size_t(&u - p_->W->units.data());
  return i < p_->driven.size() && p_->driven[i];
}

void AiBridge::rebuild(World& w) {
  Impl& I = *p_;
  I.W = &w;
  size_t n = w.units.size();
  I.recs.assign(n, OUnit());
  I.ow = OWorld();
  I.groups = OGroups();
  I.ptr.assign(n, 0);
  I.driven.assign(n, 0);
  I.died.assign(n, 0);
  I.buildMemory();
  I.setupHost();
  for (size_t i = 0; i < n; i++) I.ptr[i] = OWorld::BASE + uint32_t(i) * uint32_t(OUnit::SIZE);   // addresses first (the initial values may already refer to them)
  for (size_t i = 0; i < n; i++) {
    Unit& u = w.units[i];
    I.driven[i] = (u.cls == C_AIR && !u.isPlayer) || (u.cls == C_GROUND && (u.sub == 0 || u.sub == 2));   // anti-aircraft gun (0) and radar (2)
    I.initUnit(i);
  }
  for (size_t i = 0; i < n; i++) I.ow.add(&I.recs[i]);      // the list (+0x34) after the initial values: initUnit zeroes it
  I.groups.w = &I.ow; I.groups.img = &I.img;
  I.groups.onEvent = [&I](int kind, int cond, int id) { I.W->triggerEvent(kind, cond, id, 0); };
  for (size_t i = 0; i < n; i++) {                           // groups: leader (wing == 0) and wingmen (the slot: 0x255fc[formation] + (slot - 1) * 0x14)
    Unit& u = w.units[i];
    if (u.cls != C_AIR || u.group < 0 || u.group >= OGroups::N) continue;
    if (!u.wing) I.groups.addLeader(u.group, I.ptr[i]);
    else {
      int slot = std::clamp(u.slot, 1, 10);
      uint32_t rec = 0;
      if (u.formation < 8) { uint32_t p = uint32_t(I.i32(0x255fc + 4 * uint32_t(u.formation))) + uint32_t(slot - 1) * 0x14; if (p > 0x20000 && p + 0x14 <= I.img.size()) rec = p; }
      I.groups.addWing(u.group, slot, I.ptr[i], rec);
    }
  }
  I.groups.buildChains();
  {                                                           // 0x26879 / 0x26824: the nearby launchers (role 1) go into the chain of the radars (role 2)
    std::vector<uint32_t> joiners;
    for (size_t i = 0; i < n; i++) if (w.units[i].cls == C_GROUND && w.units[i].sub == 1) joiners.push_back(I.ptr[i]);
    for (size_t i = 0; i < n; i++) if (w.units[i].cls == C_GROUND && w.units[i].sub == 2) I.groups.join(I.ptr[i], joiners);
  }
  steps = 0;
  dirty = false;
}

void AiBridge::onActivate(World& w, Unit& u) {
  if (!dirty && drives(u) && u.cls == C_GROUND) {
    size_t i = size_t(&u - w.units.data());
    if (p_->recs[i].get8(of::STATE) == 0) p_->startGround(i);
    return;
  }
  if (dirty || !drives(u) || u.wing) return;                 // in the original only the leader starts; the wingmen are woken by the taxi waypoint (LEADER)
  OUnit& r = p_->recs[size_t(&u - w.units.data())];
  if (r.get8(of::STATE) != 0) return;
  r.set(of::PENDING, u.onGround ? 0x374c8 : 0x37510);
  r.set8(of::STATE, u.onGround ? 1 : 2);
}

void AiBridge::onHit(World& w, Unit& victim, Unit* by) {
  if (dirty || !by || !drives(victim)) return;
  Impl& I = *p_;
  size_t vi = size_t(&victim - w.units.data()), bi = size_t(by - w.units.data());
  if (vi >= I.recs.size() || bi >= I.recs.size() || !(I.recs[vi].get8(of::TYPE) & 1)) return;
  I.host.u = &I.recs[bi]; I.host.self = I.ptr[bi]; I.cur = bi;
  I.host.provoke(I.recs[vi], true);                          // 0x26468: the attacked unit evades ...
  I.host.notifyAttacked(I.ptr[vi], true);                    // ... and its group goes after the attacker (0x270a6)
}

// 0x2770d / 0x27728 / 0x2774a / 0x27765 / 0x27780: the first wingman of the player's chain (if airborne) gets a new entry; the radio message is given by the script (RADIO)
void AiBridge::reseed(World& w, Unit& u) {
  if (dirty) rebuild(w);
  Impl& I = *p_;
  size_t i = size_t(&u - w.units.data());
  if (i >= I.recs.size()) return;
  OUnit& r = I.recs[i];
  static const uint32_t KEEP[] = {of::NEXT, of::WING_NEXT, of::LEADER, of::SLOT_REC, of::DEATH};
  int32_t keep[5];
  for (int k = 0; k < 5; k++) keep[k] = r.get(KEEP[k]);
  I.initUnit(i);
  for (int k = 0; k < 5; k++) r.set(KEEP[k], keep[k]);
  I.host.u = &r; I.host.rebuildDir();
}

bool AiBridge::wingCommand(World& w, int cmd) {
  if (dirty) rebuild(w);
  Impl& I = *p_;
  if (!w.player) return false;
  size_t pi = size_t(w.player - w.units.data());
  OUnit& P = I.recs[pi];
  I.syncIn(pi);
  OUnit* f = I.ow.at(uint32_t(P.get(of::WING_NEXT)));
  if (!f || !(f->get8(of::TYPE) & 1)) return false;
  switch (cmd) {
    case 1: f->set(of::PENDING, 0x3796c); break;               // Ctrl+P: patrol route (0x2774a)
    case 2: f->set(of::PENDING, 0x37998); break;               // Ctrl+F: formation (0x27765)
    case 3: f->set(of::PENDING, 0x379ac); f->set8(of::F8, 1); break;   // Ctrl+E: attack (0x27728)
    case 4:                                                     // Ctrl+M: the player's target (0x27780)
      if (!P.get(of::TARGET) || uint32_t(P.get(of::TARGET)) == uint32_t(P.get(of::WING_NEXT))) return false;
      f->set(of::PENDING, 0x379c4); f->set8(of::F8, 0); break;
    case 5: f->set(of::PENDING, 0x379d0); break;               // Ctrl+C: cover (0x2770d)
    default: return false;
  }
  return true;
}

void AiBridge::step(World& w) {
  if (dirty) rebuild(w);
  Impl& I = *p_;
  size_t n = I.recs.size();
  if (n != w.units.size()) { dirty = true; return; }
  for (size_t i = 0; i < n; i++) if (w.units[i].dead && !I.died[i]) {     // death: the group chain (0x2668f / 0x26714)
    I.died[i] = 1;
    if (I.driven[i]) {
      if (w.units[i].group >= 0 && w.units[i].group < OGroups::N && I.groups.leader[w.units[i].group] == I.ptr[i]) I.groups.leaderDied(I.ptr[i]);
      else I.groups.wingDied(I.ptr[i]);
    }
  }
  I.host.now = w.stepNo;
  for (size_t i = 0; i < n; i++) I.syncIn(i);
  for (size_t i = 0; i < n; i++) {
    Unit& u = w.units[i];
    if (!I.driven[i] || u.dead) continue;
    OUnit& r = I.recs[i];
    if (r.get8(of::STATE) == 0 && u.cls == C_AIR && w.waitsForPlayer(u) && u.active && u.activeFlag && w.player->wasAirborne &&
        u.taxi >= 0 && u.taxi < int(I.taxiOff.size()) && !r.get(of::PENDING) && !r.get(of::SCRIPT)) {   // the player's escort taxis out after the player's takeoff
      r.set(of::TAXI, int32_t(TAXI_BASE + I.taxiOff[u.taxi] + 12 * uint32_t(w.nearestTaxiNode(u))));
      r.set(of::PENDING, 0x374c8);
      r.set8(of::STATE, 1);
    }
    I.host.u = &r; I.host.self = I.ptr[i]; I.cur = i;
    std::memcpy(I.host.perf, &I.img[0x24618 + 0x50 * size_t(std::min(u.sub, 9))], 0x50);
    r.set8(of::INCOMING, u.incomingIR ? 1 : (u.incomingRadar ? 2 : 0));
    u.incomingIR = u.incomingRadar = false;
    AiCtx cx;
    loadCtx(r, cx);
    if (cx.pc != 0 || cx.pending != 0) { I.vm.run(cx, I.host); steps++; }
    storeCtx(r, cx);
    r.set8(of::INCOMING, 0);
    r.set(of::TIMER, r.get(of::TIMER) + 1);
    I.host.move();
    I.syncOut(i);
  }
}

}  // namespace sim
