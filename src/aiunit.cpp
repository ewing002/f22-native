#include "aiunit.h"

namespace sim {

namespace {
void setZS(AiCtx& c, uint32_t v) { c.flags = (c.flags & ~(F_ZF | F_SF)) | (v == 0 ? F_ZF : 0) | (int32_t(v) < 0 ? F_SF : 0); }
}  // namespace

// ---- group handling ----
void OGroups::addLeader(int i, uint32_t p) { leader[i] = p; alive[i]++; }

void OGroups::addWing(int i, int slot, uint32_t p, uint32_t rec) {
  wings[i][slot - 1] = p;
  if (OUnit* u = w->at(p)) { u->set(of::SLOT_REC, int32_t(rec)); u->set(of::DEATH, int32_t(H_WING)); }
}

void OGroups::buildChains() {
  for (int i = 0; i < N; i++) {
    OUnit* l = w->at(leader[i]);
    if (!l) continue;
    OUnit* prev = l;
    for (int k = 0; k < 10; k++) {
      uint32_t p = wings[i][k];
      prev->set(of::WING_NEXT, int32_t(p));
      OUnit* x = w->at(p);
      if (!x) break;
      x->set(of::LEADER, int32_t(leader[i]));
      prev = x;
    }
  }
}

void OGroups::promote(OUnit* n) {
  if (!n) return;
  n->set8(of::F1, 0);
  n->set(of::DEATH, int32_t(H_LEADER));
  if (n->get8(of::STATE) == 4) n->set(of::PENDING, 0x37510);
  else if (n->get8(of::STATE) == 7) n->set8(of::PREV_STATE, 2);
}

void OGroups::groundedGone(uint32_t p) {
  OUnit* u = w->at(p);
  if (!u) return;
  uint8_t st = u->get8(of::STATE);
  if (st != 0 && st != 1 && st != 3) return;
  int idx = u->get8(of::GROUP_IDX);
  if (--alive[idx] <= 0 && onEvent) onEvent(0, 1, idx);
}

void OGroups::leaderDied(uint32_t p) {
  OUnit* d = w->at(p);
  if (!d) return;
  groundedGone(p);
  uint32_t np = uint32_t(d->get(of::WING_NEXT));
  leader[d->get8(of::GROUP_IDX)] = np;
  OUnit* n = w->at(np);
  if (!n) return;
  n->set8(of::F1, 0);
  n->set(of::WP_PTR, d->get(of::WP_PTR));
  n->set(of::DEATH, int32_t(H_LEADER));
  for (uint32_t q = uint32_t(n->get(of::WING_NEXT)); q;) {
    OUnit* x = w->at(q);
    if (!x) break;
    x->set(of::LEADER, int32_t(np));
    q = uint32_t(x->get(of::WING_NEXT));
  }
  if (n->get8(of::STATE) == 4) { n->set(of::PENDING, 0x37510); return; }
  if (d->get8(of::STATE) == 1) { n->set(of::PENDING, 0x374c8); n->set8(of::STATE, 1); }
}

void OGroups::wingDied(uint32_t p) {
  OUnit* d = w->at(p);
  if (!d) return;
  groundedGone(p);
  uint32_t q = leader[d->get8(of::GROUP_IDX)];
  OUnit* x = nullptr;
  while (q) {
    x = w->at(q);
    if (!x) return;
    if (uint32_t(x->get(of::WING_NEXT)) == p) break;
    q = uint32_t(x->get(of::WING_NEXT));
  }
  if (!q) return;
  uint32_t nx = uint32_t(d->get(of::WING_NEXT));
  x->set(of::WING_NEXT, int32_t(nx));
  OUnit* n = w->at(nx);
  if (n && d->get8(of::STATE) == 1 && n->get8(of::STATE) != 1) { n->set(of::PENDING, 0x374c8); n->set8(of::STATE, 1); }
}

void OGroups::join(uint32_t lp, const std::vector<uint32_t>& joiners) {
  OUnit* l = w->at(lp);
  if (!l) return;
  for (uint32_t jp : joiners) {
    OUnit* j = w->at(jp);
    if (!j || j->get8(of::ROLE) != 1) continue;
    int32_t dx = j->get(of::X) - l->get(of::X), dy = j->get(of::Y) - l->get(of::Y), dz = j->get(of::Z) - l->get(of::Z);
    int32_t a = std::abs(dx), b = std::abs(dy), c = std::abs(dz);
    if (a < b) std::swap(a, b);
    if (a < c) std::swap(a, c);
    int32_t d = a + int32_t((int64_t(b + c) * 0x5000) >> 16);
    if (d > 0x76c0000) continue;
    uint32_t first = uint32_t(l->get(of::WING_NEXT));
    j->set(of::WING_NEXT, int32_t(first));
    l->set(of::WING_NEXT, int32_t(jp));
    j->set(of::LEADER, int32_t(lp));
    if (OUnit* f = w->at(first)) f->set(of::LEADER, int32_t(jp));
  }
}

void OGroups::activateGroup(int idx) {
  OUnit* l = w->at(leader[idx]);
  if (l && l->get8(of::STATE) == 0) { l->set(of::SCRIPT, 0x374c8); l->set8(of::STATE, 1); }
}

void OGroups::activateUnit(uint32_t p) {
  OUnit* u = w->at(p);
  if (!u) return;
  uint32_t e; std::memcpy(&e, &(*img)[0x36ba0 + 4 * size_t(u->get8(of::ROLE))], 4);
  u->set(of::SCRIPT, int32_t(e));
  u->set8(of::STATE, 1);
}

// ---- common math (from the tables in the EXE image) ----
void OHost::sinCos(uint32_t ang, int32_t& s, int32_t& co) const {
  uint32_t a = ang & 0x1ffffff, idx = a >> 16, fr = a & 0xffff;
  auto t = [&](uint32_t i) { int32_t v; std::memcpy(&v, &(*img)[0x56840 + 4 * i], 4); return v; };
  auto lerp = [&](uint32_t i) { int32_t v0 = t(i), v1 = t(i + 1); return v0 + int32_t((int64_t(v1 - v0) * int64_t(fr)) >> 16); };
  s = lerp(idx);
  co = lerp(idx + 128);
}

int32_t OHost::atan2o(int32_t x, int32_t z) const {
  if (x == 0 && z == 0) return 0;
  auto T = [&](int32_t n, int32_t d) -> int32_t {                      // table[(n << 12) / d]
    int64_t q = (int64_t(n) << 12) / d;
    if (q < 0) q = 0;
    if (q > 4096) q = 4096;
    uint16_t v; std::memcpy(&v, &(*img)[0x3aa7c + 2 * size_t(q)], 2); return v;
  };
  auto R = [](int32_t k) { return int32_t(uint32_t(k) << 8); };
  if (x <= 0) {
    if (z > 0) {
      int32_t zz = -z;
      if (zz >= x) return R(0x8000 + T(zz, x));
      return R(0x10000 - T(x, zz));
    }
    if (z >= x) return R(0x8000 - T(z, x));
    return R(T(x, z));
  }
  if (z < 0) {
    int32_t zz = -z;
    if (zz >= x) { int32_t r = T(x, zz); return r == 0 ? 0 : R(0x20000 - r); }
    return R(0x18000 + T(zz, x));
  }
  if (z >= x) return R(0x10000 + T(x, z));
  return R(0x18000 - T(z, x));
}

void OHost::rebuildDir() {
  int32_t sA, cA, sB, cB, sC, cC;
  sinCos(uint32_t(u->get(of::ANG_A)), sA, cA);
  sinCos(uint32_t(u->get(of::ANG_B)), sB, cB);
  sinCos(uint32_t(u->get(of::ANG_C)), sC, cC);
  const double k = 1.0 / 65536.0;
  double dsA = sA * k, dcA = cA * k, dsB = sB * k, dcB = cB * k, dsC = sC * k, dcC = cC * k;
  auto rnd = [](double v) { return int32_t(std::nearbyint(v)); };
  int32_t x = 0, y = 0, z = 65536, nx, ny, nz;
  nz = z; nx = rnd(x * dcC - y * dsC); ny = rnd(y * dcC + x * dsC); x = nx; y = ny; z = nz;     // 0x5785e
  nx = x; nz = rnd(z * dcB - y * dsB); ny = rnd(y * dcB + z * dsB); x = nx; y = ny; z = nz;     // 0x578a0
  ny = y; nz = rnd(z * dcA - x * dsA); nx = rnd(x * dcA + z * dsA); x = nx; y = ny; z = nz;     // 0x5781c
  u->set(of::DIRX, x); u->set(of::DIRY, y); u->set(of::DIRZ, z);
}

void OHost::rotate(int32_t v[3], uint32_t a, uint32_t b, uint32_t cc) const {
  int32_t sA, cA, sB, cB, sC, cC;
  sinCos(a, sA, cA); sinCos(b, sB, cB); sinCos(cc, sC, cC);
  const double k = 1.0 / 65536.0;
  double dsA = sA * k, dcA = cA * k, dsB = sB * k, dcB = cB * k, dsC = sC * k, dcC = cC * k;
  auto rnd = [](double q) { return int32_t(std::nearbyint(q)); };
  int32_t x = v[0], y = v[1], z = v[2], nx, ny, nz;
  nz = z; nx = rnd(x * dcC - y * dsC); ny = rnd(y * dcC + x * dsC); x = nx; y = ny; z = nz;
  nx = x; nz = rnd(z * dcB - y * dsB); ny = rnd(y * dcB + z * dsB); x = nx; y = ny; z = nz;
  ny = y; nz = rnd(z * dcA - x * dsA); nx = rnd(x * dcA + z * dsA); x = nx; y = ny; z = nz;
  v[0] = x; v[1] = y; v[2] = z;
}

// 0x2583e: every four steps looks 0x17c units ahead; if the ground + 0xa0000 there is not lower than the aircraft, evade (state 7, script 0x37550)
bool OHost::groundAhead() {
  if ((u->get(of::TIMER) & 3) != 0) return false;
  const int64_t k = 0x17c;
  int32_t px = int32_t(uint32_t(int64_t(u->get(of::DIRX)) * k) + uint32_t(u->get(of::X)));
  int32_t py = int32_t(uint32_t(int64_t(u->get(of::DIRY)) * k) + uint32_t(u->get(of::Y)));
  int32_t pz = int32_t(uint32_t(int64_t(u->get(of::DIRZ)) * k) + uint32_t(u->get(of::Z)));
  int32_t g = groundH ? groundH(px, pz) : 0;
  if (g + 0xa0000 < py) return false;
  uint8_t st = u->get8(of::STATE);
  if (st == 7) return false;
  u->set8(of::PREV_STATE, st);
  u->set8(of::STATE, 7);
  u->set(of::PENDING, 0x37550);
  return true;
}

namespace {
int32_t clampTo(int32_t v, int32_t lim) { if (v > lim) v = lim; if (v < -lim) v = -lim; return v; }
int32_t sd25(uint32_t a, uint32_t b) { return int32_t((a << 7) - (b << 7)) >> 7; }   // a - b signed, 25-bit
}  // namespace

// 0x25d48: pitch (+0x28) and roll (+0x2c) toward zero, at most the turn limit (+0xcb / +0xcf) per step
bool OHost::levelOut() {
  uint32_t b = uint32_t(u->get(of::ANG_B)), cc = uint32_t(u->get(of::ANG_C));
  int32_t e = clampTo(int32_t(0u - (b << 7)) >> 7, u->get(of::TURN_A));
  u->set(of::ANG_B, int32_t((b + uint32_t(e)) & 0x1ffffff));
  e = clampTo(int32_t(0u - (cc << 7)) >> 7, u->get(of::TURN_B));
  u->set(of::ANG_C, int32_t((cc + uint32_t(e)) & 0x1ffffff));
  rebuildDir();
  return (u->get(of::ANG_C) | u->get(of::ANG_B)) == 0;
}

// 0x25be6: turns toward the ground waypoint ([ebp+0xc3]) (heading error at most 0x60c0 / step); mode 1: speed depends on the heading error
bool OHost::steerTaxi(int mode) {
  int32_t wx = 0, wy = 0, wz = 0;
  if (wpAt) wpAt(uint32_t(u->get(of::TAXI)), wx, wy, wz);
  int32_t ex = int32_t(uint32_t(wx) >> 1) - int32_t(uint32_t(u->get(of::X)) >> 1);
  int32_t ez = int32_t(uint32_t(wz) >> 1) - int32_t(uint32_t(u->get(of::Z)) >> 1);
  int32_t ang = atan2o(-ex, -ez);
  int32_t e = clampTo(sd25(uint32_t(ang), uint32_t(u->get(of::ANG_A))), 0x60c0);
  u->set(of::ANG_A, int32_t(uint32_t(u->get(of::ANG_A)) + uint32_t(e)));
  rebuildDir();
  if (e == 0) return true;
  if (mode == 1) u->set(of::SPEED, int32_t(((uint32_t(e) & 0xffffff) ^ 0xffffff) >> 13));
  return false;
}

// 0x25b9f: taxi to the waypoint - heading hold, accelerate up to 0x2000; brakes near the waypoint (0x40000), stops below 0x100
bool OHost::taxiTo() {
  steerTaxi(0);
  int32_t wx = 0, wy = 0, wz = 0;
  if (wpAt) wpAt(uint32_t(u->get(of::TAXI)), wx, wy, wz);
  bool near = std::abs(int64_t(wx) - u->get(of::X)) <= 0x40000 && std::abs(int64_t(wz) - u->get(of::Z)) <= 0x40000;
  int32_t sp = u->get(of::SPEED);
  if (!near) { sp += 0x40; if (sp > 0x2000) sp = 0x2000; u->set(of::SPEED, sp); return false; }
  sp -= sp >> 5;
  if (sp > 0x100) { u->set(of::SPEED, sp); return false; }
  u->set(of::SPEED, 0);
  return true;
}

// 0x25c5a: turns to the commanded heading (+0xd3), at most 0x3060 / step; done when reached (command: 0x80000000 = none)
bool OHost::turnToCmd() {
  int32_t e = clampTo(sd25(uint32_t(u->get(of::CMD_ANG)), uint32_t(u->get(of::ANG_A))), 0x3060);
  u->set(of::ANG_A, int32_t(uint32_t(u->get(of::ANG_A)) + uint32_t(e)));
  rebuildDir();
  if (e != 0) return false;
  u->set(of::CMD_ANG, int32_t(0x80000000));
  return true;
}

int32_t OHost::dist3(int32_t dx, int32_t dy, int32_t dz) const {
  int32_t a = std::abs(dx), b = std::abs(dy), c = std::abs(dz);
  if (a < b) std::swap(a, b);
  if (a < c) std::swap(a, c);
  int32_t r = a + int32_t((int64_t(b + c) * 0x5000) >> 16);
  return r == 0 ? 1 : r;
}

// 0x25780: following the target (speed tgtSpeed, distance dist): within range 0x252c8 it matches the target speed (at least 28000),
// beyond that it accelerates to at least the performance record +8 speed; at most accel per step
void OHost::matchSpeed(int32_t tgtSpeed, int32_t dist, int32_t range, int32_t perfMin, int32_t accel) {
  int32_t sp = u->get(of::SPEED);
  if (dist <= range) {
    sp += clampTo(tgtSpeed - sp, accel);
    if (sp < 28000) sp = 28000;
  } else if (sp < perfMin) {
    sp += clampTo(perfMin - sp, accel);
  }
  u->set(of::SPEED, sp);
}

int32_t OHost::dist3to(const OUnit& o) const {
  return dist3(o.get(of::X) - u->get(of::X), o.get(of::Y) - u->get(of::Y), o.get(of::Z) - u->get(of::Z));
}

uint32_t OHost::coverUnit() {
  uint32_t best = 0; int32_t bestD = 0x7fffffff;
  for (uint32_t p = world ? world->head : 0; p;) {
    OUnit* o = world->at(p);
    if (!o) break;
    if (p != self && uint32_t(o->get(of::TARGET)) == self) {
      int32_t d = dist3to(*o);
      if (d < bestD) { bestD = d; best = p; }
    }
    p = uint32_t(o->get(of::NEXT));
  }
  return best;
}

// 0x25a36: the aircraft aims at the point (tx, ty, tz) - heading: the error is limited by the turn factor (table 0x2c4d0 by speed), a large
// turn brakes (0x2611d), roll follows the turn; pitch: climb angle computed from the horizontal distance approximation (max + min/2)
void OHost::aimAt(int32_t tx, int32_t ty, int32_t tz, int32_t* yawErr, int32_t* pitchErr) {
  uint32_t sp = uint32_t(u->get(of::SPEED)) >> 10;
  if (sp > 0xc6) sp = 0xc6;
  int32_t fac; std::memcpy(&fac, &(*img)[0x2c4d0 + 4 * sp], 4);
  auto brake = [&](int32_t e) {                               // 0x2611d
    uint32_t b = uint32_t(e < 0 ? -e : e) >> 10;
    if (u->get(of::SPEED) > perf[0x34 / 4]) u->set(of::SPEED, int32_t(uint32_t(u->get(of::SPEED)) - b));
  };
  auto lim = [&](int32_t rate) { return int32_t((int64_t(rate) * fac) >> 16); };
  int32_t dx = int32_t(uint32_t(tx) >> 1) - int32_t(uint32_t(u->get(of::X)) >> 1);
  int32_t dz = int32_t(uint32_t(tz) >> 1) - int32_t(uint32_t(u->get(of::Z)) >> 1);
  int32_t e = sd25(uint32_t(atan2o(-dx, -dz)), uint32_t(u->get(of::ANG_A)));
  if (yawErr) *yawErr = e;
  e = clampTo(e, lim(u->get(of::TURN_RATE)));
  brake(e);
  u->set(of::ANG_A, int32_t(uint32_t(u->get(of::ANG_A)) + uint32_t(e)));
  int32_t roll = int32_t((uint32_t(0 - e) << 6 << 7) - (uint32_t(u->get(of::ANG_C)) << 7)) >> 11;
  u->set(of::ANG_C, int32_t(uint32_t(u->get(of::ANG_C)) + uint32_t(roll)));
  int32_t ax = std::abs(dx), az = std::abs(dz);
  if (ax < az) ax >>= 1; else az >>= 1;
  int32_t h = az + ax;
  int32_t dy = int32_t(uint32_t(ty) >> 1) - int32_t(uint32_t(u->get(of::Y)) >> 1);
  int32_t pe = sd25(uint32_t(atan2o(-dy, -h)), uint32_t(u->get(of::ANG_B)));
  if (pitchErr) *pitchErr = pe;
  pe = clampTo(pe, lim(u->get(of::TURN_A)));
  brake(pe);
  u->set(of::ANG_B, int32_t(uint32_t(u->get(of::ANG_B)) + uint32_t(pe)));
  rebuildDir();
}

bool OHost::near3(int32_t x, int32_t y, int32_t z, int32_t range) const {
  return std::abs(int64_t(x) - u->get(of::X)) <= range && std::abs(int64_t(y) - u->get(of::Y)) <= range && std::abs(int64_t(z) - u->get(of::Z)) <= range;
}

// 0x258b1 (ebp: the attacker = the aircraft, esi: v). force (al = 1): without the conditions. The evasion script is chosen by the word at v's performance record +0x38 + skill * 2
// (+8 / +0x10 by height above ground), from the random bit start point to the next set bit (table 0x36c32).
bool OHost::provoke(OUnit& v, bool force) {
  if (v.get8(of::SIDE_FLAGS) & 2) return false;
  if (!(v.get8(of::TYPE) & 1)) return false;
  if (v.get8(of::STATE) == 6) return false;
  if (!force) {
    if (u->get(of::TIMER) & 0x3f) return false;
    if (dist3to(v) > 0x5f00000) return false;
    int32_t ex = int32_t(uint32_t(u->get(of::X)) >> 1) - int32_t(uint32_t(v.get(of::X)) >> 1);
    int32_t ez = int32_t(uint32_t(u->get(of::Z)) >> 1) - int32_t(uint32_t(v.get(of::Z)) >> 1);
    int32_t ang = atan2o(-ex, -ez);
    uint32_t e = uint32_t(sd25(uint32_t(ang), uint32_t(v.get(of::ANG_A)))) & 0x1ffffff;   // is the attacker behind the defender?
    if (e > 0x1400000 || e < 0xc00000) return false;
    int32_t dh = sd25(uint32_t(v.get(of::ANG_A)), uint32_t(u->get(of::ANG_A)));
    if (std::abs(dh) > 0x300000) return false;
  }
  v.set8(of::PREV_STATE, v.get8(of::STATE));
  v.set8(of::STATE, 6);
  const uint8_t* pf = perfOf ? perfOf(v) : reinterpret_cast<const uint8_t*>(perf);
  uint32_t off = uint32_t(v.get8(of::SKILL)) * 2;
  int32_t h = v.get(of::Y) - (groundH ? groundH(v.get(of::X), v.get(of::Z)) : 0);
  if (h > 0x1770000 && h < 0x7080000) off += 8;
  else if (h >= 0x7080000) off += 0x10;
  uint16_t mask; std::memcpy(&mask, pf + 0x38 + off, 2);
  if ((mask & 0xff) == 0) return false;                       // the original would search forever here
  uint32_t r = rng.next() & 7;
  while (!(mask & (1u << r))) r = (r + 1) & 7;
  uint32_t entry; std::memcpy(&entry, &(*img)[0x36c32 + 4 * r], 4);
  v.set(of::PENDING, int32_t(entry));
  return true;
}

// 0x257e9: if the aircraft may fire (flag), its skill is > 1 and the counter is divisible by 128: fires (missile / cannon), and if not in the defensive state, evades
bool OHost::fireCheck() {
  if (!u->get8(of::INCOMING)) return false;
  if (u->get8(of::SKILL) <= 1) return false;
  if (u->get(of::TIMER) & 0x7f) return false;
  if (spawnShot) spawnShot((u->get8(of::INCOMING) & 1) != 0);
  if (u->get8(of::STATE) == 6) return false;
  if (u->get8(of::SKILL) <= 1) return false;
  provoke(*u, true);
  return true;
}

uint32_t OHost::findTarget(uint8_t sideMask, uint8_t typeMask, uint32_t range, uint32_t minDist, int flags, uint32_t* bestDist) {
  uint32_t best = 0, bestD = range;
  for (uint32_t p = world ? world->head : 0; p;) {
    OUnit* o = world->at(p);
    if (!o) break;
    uint32_t nx = uint32_t(o->get(of::NEXT));
    if (p != self && (o->get8(of::SIDE_FLAGS) & sideMask) && (o->get8(of::TYPE) & typeMask)) {
      uint32_t d = uint32_t(dist3to(*o));
      bool ok = d < bestD;
      if (ok && (flags & TS_MIN) && d <= minDist) ok = false;
      if (ok && (flags & TS_ME) && uint32_t(o->get(of::TARGET)) != self) ok = false;
      if (ok && (flags & TS_STEALTH) && (o->get8(of::SIDE_FLAGS) & 0x22)) {
        uint32_t lim = uint32_t((int64_t(int32_t(range)) * o->get(of::STEALTH)) >> 16);
        if (d >= lim) ok = false;
      }
      if (ok && (flags & TS_LOS) && !(los ? los(*o) : visible(*o))) ok = false;
      if (ok) { bestD = d; best = p; }
    }
    p = nx;
  }
  if (bestDist) *bestDist = bestD;
  return best;
}

// 0x27166: the leader assigns targets to its (state 4, not bound) wingmen: each next wingman gets a target farther than the previous one (the search measures from the leader);
// they get script 0x375a8, the commanded heading / pitch from the slot record. If there are no more targets, the assignment stops. (At the end of the chain the original also examines a byte at address 0
// - this is not imitated.)
void OHost::assignWing() {
  bool leader = u->get8(of::F9) != 0;
  uint32_t range = leader ? 0x16440000u : 0xbe00000u;
  int flags = leader ? (TS_MIN | TS_STEALTH | TS_LOS | TS_ME) : (TS_MIN | TS_STEALTH | TS_LOS);
  uint32_t acc = 0;
  for (uint32_t p = uint32_t(u->get(of::WING_NEXT)); p;) {
    OUnit* w = world->at(p);
    if (!w) return;
    if (w->get8(of::STATE) == 4 && w->get8(of::F6) == 0) {
      uint32_t d = 0;
      uint32_t t = findTarget(w->get8(of::GROUP_ID), u->get8(of::F7), range, acc, flags, &d);
      acc = d;
      if (!t) return;
      w->set(of::TARGET, int32_t(t));
      w->set(of::PENDING, 0x375a8);
      w->set8(of::F8, 0);
      int32_t a = 0, b = 0;
      if (slotAt) slotAt(*w, a, b);
      w->set(of::CMD_ANG, int32_t(uint32_t(a + w->get(of::ANG_A)) & 0x1ffffff));
      w->set(of::CMD_PITCH, int32_t(uint32_t(b + w->get(of::ANG_B)) & 0x1ffffff));
    }
    p = uint32_t(w->get(of::WING_NEXT));
  }
  if (!leader && u->get8(of::F6) == 0) {
    uint32_t t = findTarget(u->get8(of::GROUP_ID), u->get8(of::F7), range, acc, flags);
    if (t) { u->set(of::TARGET, int32_t(t)); u->set(of::PENDING, 0x375a8); u->set8(of::F8, 0); }
  }
}

bool OHost::maneuver() {
  const int32_t NONE = int32_t(0x80000000);
  uint32_t sp = uint32_t(u->get(of::SPEED)) >> 10;
  if (sp > 0xc6) sp = 0xc6;
  int32_t fac; std::memcpy(&fac, &(*img)[0x2c4d0 + 4 * sp], 4);
  auto brake = [&](int32_t e) {
    uint32_t b = uint32_t(e < 0 ? -e : e) >> 10;
    if (u->get(of::SPEED) > perf[0x34 / 4]) u->set(of::SPEED, int32_t(uint32_t(u->get(of::SPEED)) - b));
  };
  auto lim = [&](int32_t rate) { return int32_t((int64_t(rate) * fac) >> 16); };
  auto add = [&](uint32_t f, int32_t e) { u->set(f, int32_t((uint32_t(u->get(f)) + uint32_t(e)) & 0x1ffffff)); };
  auto fail = [&]() { rebuildDir(); return false; };
  if (u->get8(of::STATE) == 6 && fireCheck()) return fail();
  if (u->get(of::CMD_PITCH) != NONE) {
    int32_t e = clampTo(sd25(uint32_t(u->get(of::CMD_PITCH)), uint32_t(u->get(of::ANG_B))), lim(u->get(of::TURN_A)));
    brake(e * 2);
    add(of::ANG_B, e);
  }
  if (u->get(of::CMD_ANG) != NONE) {
    int32_t e = clampTo(sd25(uint32_t(u->get(of::CMD_ANG)), uint32_t(u->get(of::ANG_A))), lim(u->get(of::TURN_RATE)));
    brake(e);
    add(of::ANG_A, e);
    int32_t r = int32_t((uint32_t(0 - e) << 6 << 7) - (uint32_t(u->get(of::ANG_C)) << 7)) >> 11;
    add(of::ANG_C, r);
  } else if (u->get(of::CMD_ROLL) != NONE) {
    add(of::ANG_C, clampTo(sd25(uint32_t(u->get(of::CMD_ROLL)), uint32_t(u->get(of::ANG_C))), u->get(of::TURN_B)));
  }
  if (u->get(of::CMD_PITCH) != NONE) {
    if (u->get(of::CMD_PITCH) != u->get(of::ANG_B)) return fail();
    u->set(of::CMD_PITCH, NONE);
  }
  if (u->get(of::CMD_ANG) != NONE) {
    if (u->get(of::CMD_ANG) != u->get(of::ANG_A)) return fail();
    u->set(of::CMD_ROLL, 0);
    u->set(of::CMD_ANG, NONE);
    return fail();
  }
  if (u->get(of::CMD_ROLL) != NONE && u->get(of::CMD_ROLL) != u->get(of::ANG_C)) return fail();
  u->set(of::CMD_ANG, NONE); u->set(of::CMD_PITCH, NONE); u->set(of::CMD_ROLL, NONE);
  rebuildDir();
  return true;
}

void OHost::formationStep() {
  if (groundAhead() || fireCheck()) return;
  OUnit* L = world ? world->at(uint32_t(u->get(of::LEADER))) : nullptr;
  if (!L) return;
  if (L->get8(of::TYPE) & 2) { u->set(of::PENDING, 0x37970); return; }
  int32_t v[3] = {0, 0, 0};
  if (slotVec) slotVec(*u, v);
  int32_t sA, cA; sinCos(uint32_t(L->get(of::ANG_A)), sA, cA);
  const double k = 1.0 / 65536.0;
  int32_t rz = int32_t(std::nearbyint(v[2] * (cA * k) - v[0] * (sA * k)));
  int32_t rx = int32_t(std::nearbyint(v[0] * (cA * k) + v[2] * (sA * k)));
  int32_t px = v[0] = rx + L->get(of::X), py = v[1] + L->get(of::Y), pz = rz + L->get(of::Z);
  int32_t maxSp = perf[0];
  auto speedBy = [&](int32_t e) { u->set(of::SPEED, u->get(of::SPEED) + clampTo(e, maxSp)); };
  if (near3(px, py, pz, 0xa0000)) {
    uint32_t c = uint32_t(u->get(of::ANG_C));                  // 0x25d2d: roll slowly to zero
    u->set(of::ANG_C, int32_t((c + uint32_t(int32_t(0u - (c << 7)) >> 11)) & 0x1ffffff));
    rebuildDir();
    speedBy((L->get(of::SPEED) - u->get(of::SPEED)) >> 2);
  } else {
    int32_t ye = 0, pe = 0;
    aimAt(px, py, pz, &ye, &pe);
    if (std::abs(ye) < 0x40000 || std::abs(pe) < 0x40000) {
      int32_t d = dist3to(*L);
      speedBy(((L->get(of::SPEED) - 0x500 + int32_t(uint32_t(d) >> 10)) - u->get(of::SPEED)) >> 7);
    } else {
      speedBy((L->get(of::SPEED) - u->get(of::SPEED)) >> 2);
    }
  }
  if (u->get(of::SPEED) > maxSp) u->set(of::SPEED, maxSp);
}

void OHost::notifyAttacked(uint32_t vp, bool force) {
  OUnit* v = world ? world->at(vp) : nullptr;
  if (!v || !(v->get8(of::TYPE) & 1) || (v->get8(of::SIDE_FLAGS) & 2)) return;
  if (!force && (u->get(of::TIMER) & 0x3f)) return;           // al = 0: only every 64th step
  uint32_t gp = groupLeader ? groupLeader(v->get8(of::GROUP_IDX)) : 0;
  OUnit* g = world->at(gp);
  if (!g || (g->get8(of::SIDE_FLAGS) & 2) || g->get8(of::F9) == 0) return;
  for (uint32_t p = uint32_t(g->get(of::WING_NEXT)); p;) {     // 0x270ed: is someone in the group already targeting me?
    OUnit* w = world->at(p);
    if (!w) return;
    if (uint32_t(w->get(of::TARGET)) == self) return;
    p = uint32_t(w->get(of::WING_SIB));
  }
  for (uint32_t p = uint32_t(g->get(of::WING_NEXT)); p;) {     // otherwise the first state-4 wingman comes after me
    OUnit* w = world->at(p);
    if (!w) return;
    if (w->get8(of::STATE) == 4) {
      w->set(of::TARGET, int32_t(self));
      w->set(of::PENDING, 0x375a8);
      w->set8(of::F8, 0);
      int32_t a = 0, b = 0;
      if (slotAt) slotAt(*w, a, b);
      w->set(of::CMD_ANG, int32_t(uint32_t(a + w->get(of::ANG_A)) & 0x1ffffff));
      w->set(of::CMD_PITCH, int32_t(uint32_t(b + w->get(of::ANG_B)) & 0x1ffffff));
      return;
    }
    p = uint32_t(w->get(of::WING_NEXT));
  }
}

bool OHost::fireAtTarget() {
  OUnit* t = world ? world->at(uint32_t(u->get(of::TARGET))) : nullptr;
  if (!t) { u->set(of::PENDING, 0x376b0); return true; }
  uint16_t kind; std::memcpy(&kind, reinterpret_cast<const uint8_t*>(perf) + 0x26, 2);
  if (kind == 3) { if (weapon) weapon(2); return true; }
  if (dist3to(*t) <= 0x17c0000) { if (weapon) weapon(0); return false; }
  if (weapon) weapon(1);
  return true;
}

bool OHost::pursuitStep() {
  if (groundAhead() || fireCheck()) return false;
  uint16_t kind; std::memcpy(&kind, reinterpret_cast<const uint8_t*>(perf) + 0x26, 2);
  if (kind == 3) return groundAttack();
  if (leaderFar()) { u->set(of::PENDING, 0x376b0); return false; }   // the original immediately overwrites the 0x3759c entry
  uint32_t tp = uint32_t(u->get(of::TARGET));
  OUnit* t = world ? world->at(tp) : nullptr;
  if (!t) { u->set(of::PENDING, 0x376b0); return false; }
  notifyAttacked(tp);
  aimAt(t->get(of::X), t->get(of::Y), t->get(of::Z));
  int32_t d = dist3to(*t);
  matchSpeed(t->get(of::SPEED), d, 0x5f00000, perf[8 / 4], perf[0x20 / 4]);
  return d <= u->get(of::MIN_RANGE);
}

void OHost::takeoffStep(AiCtx& c) {
  u->set8(of::STATE, 3);
  u->set(of::SPEED, u->get(of::SPEED) + (uint32_t(perf[0x20 / 4]) >> 1));
  if (u->get(of::SPEED) < perf[4 / 4]) { c.flags &= ~1u; c.pc += 4; return; }
  if (onEvent) onEvent(1, 1);
  if (onLiftoff) onLiftoff();
  u->set(of::CMD_PITCH, 0x140000);
  u->set8(of::STATE, 2);
  u->set8(of::TYPE, uint8_t((u->get8(of::TYPE) & 0xfd) | 1));
  c.flags |= 1u; c.pc += 4;
}

void OHost::taxiWaypoint(AiCtx& c) {
  uint32_t wp = uint32_t(u->get(of::TAXI));
  int32_t kind = wpWord ? wpWord(wp, 8) & 0xffff : 0;
  if (kind == 1) {
    c.pc = 0x374e8;                                           // the script PC directly (+0xbf)
    u->set(of::CMD_ANG, int32_t(uint32_t((wpWord ? wpWord(wp, 0xa) : 0) & 0xffff) << 16));
    return;
  }
  if (kind != 0) {
    OUnit* w = world ? world->at(uint32_t(u->get(of::WING_NEXT))) : nullptr;
    if (w) { w->set(of::SCRIPT, 0x374c8); w->set8(of::STATE, 1); }
  }
  u->set(of::TAXI, u->get(of::TAXI) + 0xc);
  c.pc += 4;
}

void OHost::formOp(AiCtx& c) {
  if (u->get8(of::F1) == 1) {
    OUnit* L = world ? world->at(uint32_t(u->get(of::LEADER))) : nullptr;
    if (!L || L->get8(of::STATE) != 2) {
      u->set(of::TARGET, int32_t(findTarget(u->get8(of::GROUP_ID), u->get8(of::F7), 0x17c00000u, 0, TS_STEALTH | TS_LOS)));
      if (u->get(of::TARGET)) { c.pc = 0x375a8; u->set8(of::STATE, 5); return; }
    }
    c.pc = 0x3759c; u->set8(of::STATE, 4);
    return;
  }
  u->set(of::CMD_SPEED, perf[8 / 4]);
  u->set8(of::STATE, 2);
  c.pc += 4;
}

uint32_t OHost::coverFor(uint32_t lp) {
  uint32_t sv = self; self = lp;
  OUnit* L = world->at(lp); OUnit* su = u; u = L;             // the distance is measured from the leader
  uint32_t r = coverUnit();
  u = su; self = sv;
  return r;
}

void OHost::actOp(int which) {
  OUnit* L = world ? world->at(uint32_t(u->get(of::LEADER))) : nullptr;
  if (which == 1 && L) {
    uint32_t t = uint32_t(L->get(of::TARGET));
    u->set(of::TARGET, int32_t(t == self ? 0 : t));
  } else if (which == 3) {
    u->set(of::TARGET, L ? int32_t(coverFor(uint32_t(u->get(of::LEADER)))) : 0);
  } else if (which == 0 || which == 1) {
    u->set(of::TARGET, int32_t(findTarget(u->get8(of::GROUP_ID), u->get8(of::F7), 0x17c00000u, 0, TS_STEALTH | TS_LOS)));
  }
}

void OHost::assignToUnit(OUnit& w, uint32_t tp, uint8_t f8) {
  w.set(of::TARGET, int32_t(tp));
  w.set(of::PENDING, 0x375a8);
  w.set8(of::F8, f8);
  int32_t a = 0, b = 0;
  if (slotAt) slotAt(w, a, b);
  w.set(of::CMD_ANG, int32_t(uint32_t(a + w.get(of::ANG_A)) & 0x1ffffff));
  w.set(of::CMD_PITCH, int32_t(uint32_t(b + w.get(of::ANG_B)) & 0x1ffffff));
}

void OHost::classOp(int cls) {
  auto adv = [&] { u->set(of::WP_PTR, u->get(of::WP_PTR) + 0x10); };
  uint32_t wp = uint32_t(u->get(of::WP_PTR));
  if (cls == 0) { adv(); return; }
  if (cls == 1) { u->set(of::WP_PTR, (wpWord ? (wpWord(wp, 0xe) & 0xffff) : 0) * 16 + u->get(of::WP_BASE)); return; }
  if (cls == 2) {                                             // 0x26eb3: each wingman (and finally the leader) gets an ever farther target, up to 0x2c880000
    uint32_t acc = 0;
    auto search = [&](uint8_t grp) {
      uint32_t d = 0;
      uint32_t t = findTarget(grp, u->get8(of::F7), 0x2c880000u, acc, TS_MIN, &d);
      acc = d;
      if (!t) { t = findTarget(grp, u->get8(of::F7), 0x2c880000u, 0, 0, &d); acc = d; }
      return t;
    };
    for (uint32_t p = uint32_t(u->get(of::WING_NEXT)); p;) {
      OUnit* w = world->at(p);
      if (!w) return;
      if (w->get8(of::STATE) == 4) {
        uint32_t t = search(w->get8(of::GROUP_ID));
        if (!t) { adv(); return; }
        assignToUnit(*w, t, 1);
      }
      p = uint32_t(w->get(of::WING_NEXT));
    }
    uint32_t t = search(u->get8(of::GROUP_ID));
    if (!t) { adv(); return; }
    u->set(of::TARGET, int32_t(t)); u->set(of::PENDING, 0x375a8); u->set8(of::F8, 1);
    return;
  }
  if (cls == 3) {                                             // 0x26fdd: a named unit (id) is the target; the first wingman (or the leader) matching the type mask goes after it
    int32_t id = wpWord ? (wpWord(wp, 0xe) & 0xffff) : 0;
    uint32_t tp = 0; OUnit* T = nullptr;
    for (uint32_t p = world ? world->head : 0; p;) {
      OUnit* o = world->at(p);
      if (!o) break;
      if ((uint32_t(o->get(of::ID)) & 0xffff) == uint32_t(id)) { tp = p; T = o; break; }
      p = uint32_t(o->get(of::NEXT));
    }
    if (!T) { adv(); return; }
    uint8_t tm = T->get8(of::TYPE);
    for (uint32_t p = uint32_t(u->get(of::WING_NEXT)); p;) {
      OUnit* w = world->at(p);
      if (!w) break;
      if (w->get8(of::STATE) == 4 && (w->get8(of::F7) & tm)) { assignToUnit(*w, tp, 0); adv(); return; }
      p = uint32_t(w->get(of::WING_NEXT));
    }
    if (u->get8(of::F7) & tm) { u->set(of::TARGET, int32_t(tp)); u->set(of::PENDING, 0x375a8); u->set8(of::F8, 0); }
    adv();
    return;
  }
  if (cls == 4) { assignFromLeader(); return; }
  if (cls == 5) {                                             // 0x27366: one-time radio message - the class is zeroed, then the next waypoint
    if (wpSetWord) wpSetWord(wp, 0xc, 0);
    if (missionRadio) missionRadio(wpWord ? (wpWord(wp, 0xe) & 0xffff) : 0);
    u->set(of::WP_PTR, u->get(of::WP_PTR) + 0x10);
    return;
  }
  if (cls == 6) {                                             // 0x2739e: two events (id / group), then the next waypoint
    if (onEvent) { onEvent(1, 2); onEvent(0, 2); }
    adv();
  }
}

// 0x30c05: between the aircraft and the target (with a step size computed by dividing the distance / 64, over distance >> 22 steps - the original walks this many) the
// ground height (maximum of four neighbors) stays below the ray, or the interpolated ground does not reach the ray. With a zero step count the original
// would repeat 2^32 times; here at most 4096 steps.
bool OHost::visible(const OUnit& t) const {
  int32_t d = dist3to(t);
  int32_t div = int32_t(uint32_t(d) >> 6);
  if (div == 0) return true;
  auto stepOf = [&](int32_t a, int32_t b) {
    int32_t diff = int32_t(uint32_t(a) >> 1) - int32_t(uint32_t(b) >> 1);
    return int32_t((int64_t(diff) << 16) / div);
  };
  int32_t sx = stepOf(t.get(of::X), u->get(of::X)), sy = stepOf(t.get(of::Y), u->get(of::Y)), sz = stepOf(t.get(of::Z), u->get(of::Z));
  int32_t x = u->get(of::X), y = u->get(of::Y), z = u->get(of::Z);
  uint32_t n = uint32_t(div) >> 16;
  if (n == 0 || n > 4096) n = n == 0 ? 4096 : 4096;
  for (; n; n--) {
    int32_t g = groundH ? groundH(x, z) : 0;
    if (g >= y) {
      int32_t gi = groundI ? groundI(x, z) : g;
      if (gi >= y) return false;
    }
    x += sx; y += sy; z += sz;
  }
  return true;
}

void OHost::move() {
  int64_t sp = u->get(of::SPEED);
  if (sp == 0) return;
  u->set(of::X, int32_t(uint32_t(u->get(of::X)) + uint32_t((int64_t(u->get(of::DIRX)) * sp) >> 16)));
  u->set(of::Y, int32_t(uint32_t(u->get(of::Y)) + uint32_t((int64_t(u->get(of::DIRY)) * sp) >> 16)));
  u->set(of::Z, int32_t(uint32_t(u->get(of::Z)) + uint32_t((int64_t(u->get(of::DIRZ)) * sp) >> 16)));
}

uint32_t OHost::pickLauncher(const OUnit& t) {
  int32_t ex = int32_t(uint32_t(t.get(of::X)) >> 1) - int32_t(uint32_t(u->get(of::X)) >> 1);
  int32_t ez = int32_t(uint32_t(t.get(of::Z)) >> 1) - int32_t(uint32_t(u->get(of::Z)) >> 1);
  int32_t bearing = atan2o(-ex, -ez);
  int32_t best = 0x2000000; uint32_t bp = 0;
  for (uint32_t p = uint32_t(u->get(of::WING_NEXT)); p;) {
    OUnit* l = world ? world->at(p) : nullptr;
    if (!l) break;
    int32_t e = std::abs(sd25(uint32_t(bearing), uint32_t(l->get(of::ANG_A))));
    if (e < best) { best = e; bp = p; }
    p = uint32_t(l->get(of::WING_NEXT));
  }
  OUnit* l = bp ? world->at(bp) : nullptr;
  if (!l || now - l->get(of::LAST_LAUNCH) <= 0x12c0) return 0;
  return bp;
}

void OHost::radarStep() {
  if (u->get(of::TIMER) & 0x1f) return;
  int32_t y0 = u->get(of::Y);
  u->set(of::Y, y0 + 0x10000);                                // the search starts 1 wu above the radar
  uint32_t tp = findTarget(u->get8(of::GROUP_ID), 1, 0xed80000u, 0, TS_STEALTH | TS_LOS);
  if (OUnit* t = tp ? world->at(tp) : nullptr) {
    if (onEvent) onEvent(1, 4);                               // "detected" (1, 4, id)
    uint32_t lp = pickLauncher(*t);
    if (OUnit* l = lp ? world->at(lp) : nullptr) {
      l->set(of::TARGET, int32_t(tp));
      if (launchSam) launchSam(*l, lp);
      l->set(of::LAST_LAUNCH, now);
    }
  }
  u->set(of::Y, y0);
}

void OHost::aaaStep() {
  if (u->get(of::TIMER) & 0x1f) return;
  uint32_t tp = findTarget(u->get8(of::GROUP_ID), 1, 0x4740000u, 0, 0);
  u->set(of::TARGET, int32_t(tp));
  OUnit* t = tp ? world->at(tp) : nullptr;
  if (!t) return;
  int32_t d = dist3to(*t);
  int32_t v[3] = {0, 0, int32_t(uint32_t(d / 0x30000) * uint32_t(t->get(of::SPEED)))};   // lead: (distance / 0x30000) * the target speed
  rotate(v, uint32_t(t->get(of::ANG_A)), uint32_t(t->get(of::ANG_B)), uint32_t(t->get(of::ANG_C)));
  int32_t ax = int32_t(uint32_t(v[0]) + uint32_t(t->get(of::X))), ay = int32_t(uint32_t(v[1]) + uint32_t(t->get(of::Y))), az = int32_t(uint32_t(v[2]) + uint32_t(t->get(of::Z)));
  int32_t hx = int32_t(uint32_t(u->get(of::X)) >> 1) - int32_t(uint32_t(ax) >> 1);
  int32_t hz = int32_t(uint32_t(u->get(of::Z)) >> 1) - int32_t(uint32_t(az) >> 1);
  int32_t bearing = atan2o(-hx, -hz);                         // 0x30683 (esi = the target, edi = the aircraft): the original "backward" direction - 0x3a345 receives the same vector
  int32_t e = clampTo(sd25(uint32_t(bearing), uint32_t(u->get(of::ANG_A))), 0x400000);
  u->set(of::ANG_A, int32_t(uint32_t(u->get(of::ANG_A)) + uint32_t(e)));
  int32_t dx = std::abs(int32_t(uint32_t(u->get(of::X)) - uint32_t(ax))), dz = std::abs(int32_t(uint32_t(u->get(of::Z)) - uint32_t(az)));
  if (dx < dz) dx >>= 1; else dz >>= 1;
  int32_t elev = atan2o(int32_t(uint32_t(u->get(of::Y)) - uint32_t(ay)), dz + dx);   // 0x306a3
  if (aaaBurst) aaaBurst(u->get(of::ANG_A), elev);
}

void OHost::assignFromLeader() {
  uint32_t acc = 0;
  auto adv = [&] { u->set(of::WP_PTR, u->get(of::WP_PTR) + 0x10); };
  for (uint32_t p = uint32_t(u->get(of::WING_NEXT)); p;) {
    OUnit* w = world->at(p);
    if (!w) break;
    if (w->get8(of::STATE) == 4) {
      uint32_t d = 0;
      uint32_t t = findTarget(w->get8(of::GROUP_ID), u->get8(of::F7), 0x16440000u, acc, TS_MIN, &d);
      acc = d;
      if (!t) { adv(); return; }
      assignToUnit(*w, t, 0);
    }
    p = uint32_t(w->get(of::WING_NEXT));
  }
  uint32_t t = findTarget(u->get8(of::GROUP_ID), u->get8(of::F7), 0x16440000u, acc, TS_MIN);
  if (!t) { adv(); return; }
  u->set(of::TARGET, int32_t(t)); u->set(of::PENDING, 0x375a8); u->set8(of::F8, 0);
}

bool OHost::leaderFar() {
  OUnit* l = world ? world->at(uint32_t(u->get(of::LEADER))) : nullptr;
  if (!l || (l->get8(of::SIDE_FLAGS) & 2) || l->get8(of::F9) == 0) return false;
  if (dist3to(*l) <= 0x16440000) return false;
  u->set(of::PENDING, 0x3759c);
  u->set8(of::STATE, 4);
  return true;
}

bool OHost::groundAttack() {
  OUnit* t = world ? world->at(uint32_t(u->get(of::TARGET))) : nullptr;
  if (!t) { u->set(of::PENDING, 0x376b0); return false; }
  int32_t tx = t->get(of::X), tz = t->get(of::Z);
  int32_t ty = (groundH ? groundH(tx, tz) : 0) + 0x57e0000;
  int32_t ye = 0, pe = 0;
  aimAt(tx, ty, tz, &ye, &pe);
  if (!near3(tx, ty, tz, 0x5dc0000)) return false;
  return std::abs(ye) <= 0x200000 && std::abs(pe) <= 0x200000;
}

bool OHost::attackStep() {
  if (groundAhead() || fireCheck()) return true;
  uint16_t kind; std::memcpy(&kind, reinterpret_cast<const uint8_t*>(perf) + 0x26, 2);
  if (kind == 3) return groundAttack();
  OUnit* t = world ? world->at(uint32_t(u->get(of::TARGET))) : nullptr;
  if (!t) { u->set(of::PENDING, 0x376b0); return false; }
  if (t->get8(of::SIDE_FLAGS) & 2) t->set8(0xb0, 1);
  int32_t ye = 0, pe = 0;
  aimAt(t->get(of::X), t->get(of::Y), t->get(of::Z), &ye, &pe);
  int32_t d = dist3to(*t);
  matchSpeed(t->get(of::SPEED), d, 0x17c0000, perf[8 / 4], perf[0x20 / 4]);
  if (d > u->get(of::MIN_RANGE)) return false;
  if (std::abs(ye) > 0x200000 || std::abs(pe) > 0x200000) return false;
  provoke(*t, false);
  return true;
}

bool OHost::patrolStep() {
  if (groundAhead() || fireCheck()) return true;
  u->set(of::F10B, u->get(of::F10B) + 0x14000);
  if ((u->get(of::TIMER) & 0x3f) == 0 && u->get8(of::F9) == 0) assignWing();
  int32_t x = 0, y = 0, z = 0;
  if (wpAt) wpAt(uint32_t(u->get(of::WP_PTR)), x, y, z);
  if (near3(x, y, z, 0xc80000)) return true;
  aimAt(x, y, z);
  int32_t minSp = perf[8 / 4], sp = u->get(of::SPEED);
  if (sp < minSp) u->set(of::SPEED, sp + clampTo(minSp - sp, perf[0x20 / 4]));
  return false;
}

// 0x36f9c: speed toward the commanded (+0x103) with the acceleration (perf +0x20); done when reached, or when ground evasion has started
bool OHost::speedTo() {
  int32_t acc = perf[0x20 / 4];
  int32_t e = clampTo(u->get(of::CMD_SPEED) - u->get(of::SPEED), acc);
  u->set(of::SPEED, u->get(of::SPEED) + e);
  if (u->get(of::SPEED) == u->get(of::CMD_SPEED)) return true;
  return groundAhead();
}

bool OHost::op(int code, const uint8_t* ins, AiCtx& c) {
  int a = ins[1], b = ins[2], d = ins[3];
  auto fin = [&](bool carry) {                                // the helper's carry -> bit 1 of the flags (1 = done), the instruction continues
    c.flags = carry ? (c.flags | 1u) : (c.flags & ~1u);
    c.pc += 4;
    return true;
  };
  switch (code) {
    case 0x0b: return fin(patrolStep());
    case 0x18: return fin(attackStep());
    case 0x14: return fin(maneuver());
    case 0x26: radarStep(); c.pc += 4; return true;   // the handler (0x37408) continues; the script stops at END
    case 0x27: aaaStep(); c.pc += 4; return true;
    case 0x1a: actOp(a); c.pc += 4; return true;
    case 0x1b: if (radio) radio(a); c.pc += 4; return true;
    case 0x24: {                                              // CLS: the class is in the waypoint's +0xc word
      int32_t cls = wpWord ? (wpWord(uint32_t(u->get(of::WP_PTR)), 0xc) & 0xffff) : 0;
      classOp(cls); c.pc += 4; return true;
    }
    case 0x0e: {                                              // TGT (0x36f26): if it has no target and may search, it searches; flag: 1 = no target
      if (!u->get(of::TARGET) && u->get8(of::F8) != 0)
        u->set(of::TARGET, int32_t(findTarget(u->get8(of::GROUP_ID), u->get8(of::F7), 0xbe00000u, 0, TS_STEALTH | TS_LOS)));
      return fin(u->get(of::TARGET) == 0);
    }
    case 0x20: taxiWaypoint(c); return true;
    case 0x22: takeoffStep(c); return true;
    case 0x25: formOp(c); return true;
    case 0x29: {                                              // VMAX: the commanded speed from the performance record, with the skill factor (0x36c02)
      int32_t v = a == 0 ? perf[0] : (a == 3 ? int32_t(uint32_t(perf[8 / 4]) >> 3) : perf[8 / 4]);
      int32_t k; std::memcpy(&k, &(*img)[0x36c02 + 4 * size_t(u->get8(of::SKILL))], 4);
      u->set(of::CMD_SPEED, int32_t((int64_t(v) * k) >> 16));
      c.pc += 4; return true;
    }
    case 0x2a: {                                              // VTGT: r0 = perf[+0x30] * 0x36c22[skill]
      int32_t k; std::memcpy(&k, &(*img)[0x36c22 + 4 * size_t(u->get8(of::SKILL))], 4);
      c.r[0] = int32_t((int64_t(perf[0x30 / 4]) * k) >> 16);
      c.pc += 4; return true;
    }
    case 0x0c: return fin(fireAtTarget());
    case 0x0f: {                                              // the wrapper (0x36f76): not done -> the step stops here
      bool done = pursuitStep();
      c.flags = done ? (c.flags | 1u) : (c.flags & ~1u);
      c.pc += 4;
      return done;
    }
    case 0x19: formationStep(); c.pc += 4; return true;
    case 0x10: return fin(speedTo());
    case 0x11: return fin(levelOut());
    case 0x1f: return fin(taxiTo());
    case 0x21: return fin(steerTaxi(1));
    case 0x23: return fin(turnToCmd());
    case 0x0a: {                                              // STF: field <- r[a]; offset = signed b << 8 | c
      int32_t off = int32_t(int8_t(b)) * 256 + d;
      u->set(uint32_t(off), c.r[a & 3]);
      c.pc += 4;
      return true;
    }
    case 0x15: {                                              // LDF: r[a] <- field; offset = b << 8 | c
      c.r[a & 3] = u->get(uint32_t(b << 8 | d));
      c.pc += 4;
      return true;
    }
    case 0x13: {                                              // RND: r[a] = random & (b << 8 | c); flags from the result
      uint32_t v = rng.next() & uint32_t(b << 8 | d);
      c.r[a & 3] = int32_t(v);
      setZS(c, v);
      c.flags &= ~F_CF;
      c.pc += 4;
      return true;
    }
    case 0x1e: {                                              // AGL: r[a] = altitude - ground height
      int32_t g = groundH ? groundH(u->get(of::X), u->get(of::Z)) : 0;
      c.r[a & 3] = u->get(of::Y) - g;
      c.pc += 4;
      return true;
    }
    case 0x0d:                                                // WPSET: waypoint pointer = base + a * 16
      u->set(of::WP_PTR, a * 16 + u->get(of::WP_BASE));
      c.pc += 4;
      return true;
    case 0x28:                                                // STATE
      u->set8(of::STATE, uint8_t(a));
      c.pc += 4;
      return true;
    default:
      return other(code, ins, c);
  }
}

}  // namespace sim
