// The reverse-engineered original rules (with addresses). Where the reverse engineering was not complete,
// the solution gets an "APPROXIMATION" note.
#include "sim.h"
#include "aibridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace sim {

double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(D3 a) { return std::sqrt(dot(a, a)); }
D3 norm(D3 a) { double l = len(a); return l > 1e-12 ? a * (1 / l) : D3{0, 0, 1}; }
double wrapd(double d) { return d - WORLD * std::floor((d + WORLD / 2) / WORLD); }
D3 wdelta(D3 a, D3 b) { return {wrapd(a.x - b.x), a.y - b.y, wrapd(a.z - b.z)}; }
D3 dirOf(double h, double p) { return {std::sin(h) * std::cos(p), std::sin(p), std::cos(h) * std::cos(p)}; }
static double wrapA(double a) { return a - 2 * PI * std::floor((a + PI) / (2 * PI)); }
void basis(double h, double p, double r, D3& f, D3& up, D3& right) {
  f = dirOf(h, p);
  D3 u0 = dirOf(h, p + PI / 2);
  double c = std::cos(-r), s = std::sin(-r);
  up = u0 * c + cross(f, u0) * s;
  right = cross(up, f);
}
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static D3 rot(D3 v, D3 k, double a) {
  double c = std::cos(a), s = std::sin(a);
  return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}
static void wrapPos(D3& p) {
  p.x -= WORLD * std::floor(p.x / WORLD);
  p.z -= WORLD * std::floor(p.z / WORLD);
}
static const double DEG = PI / 180;

// ------------------------------------------------------------------ Exe
bool Exe::load(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  d.assign(std::istreambuf_iterator<char>(f), {});
  return d.size() > 0x8a000 && std::memcmp(&d[0x50f18], "bin01.bin", 9) == 0;
}

// F22.EXE = MZ stub + FlashTek X-386 extender; the 32-bit program image starts at file offset 0xC8ED and is
// loaded at address 0. Its start-up stub (0x2514) decrypts 0x24C4-0x8A9A6 in two passes before jumping to the
// game (0x8A7B1 -> 0x5BE2C). This repeats those passes, so the result is the same 2 MiB flat image that
// tools/f22unpack.py dumps (identical byte for byte, including the stub's self-patches and its return address).
bool Exe::loadGameExe(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), {});
  const size_t IMG = 0xC8ED, END = 0x8A9A6, SIZE = 0x200000;
  if (raw.size() < IMG + END || raw[0] != 'M' || raw[1] != 'Z' || raw.size() - IMG > SIZE) return false;
  std::vector<uint8_t> m(SIZE, 0);
  std::memcpy(m.data(), raw.data() + IMG, raw.size() - IMG);
  auto rd = [&](uint32_t a) { uint32_t v; std::memcpy(&v, &m[a], 4); return v; };
  auto wr = [&](uint32_t a, uint32_t v) { std::memcpy(&m[a], &v, 4); };
  // pass 1 (0x2568): the first 0x40 bytes of the second stub, fixed key pair
  for (uint32_t a = 0x24C4; a < 0x24C4 + 0x40; a += 8) {
    wr(a, rd(a) ^ 0x9324ABE1u);
    wr(a + 4, rd(a + 4) ^ 0x439A9EFAu);
  }
  // the stub patches its own loop (0x279D / 0x27A3) before running it
  wr(0x258B, 0x1ABB09BDu);   // mov ebp, 0x001ABB09
  wr(0x259F, 0x5E310631u);   // xor [esi], eax / xor [esi+4], ebx
  // pass 2 (0x259F): rolling key over 0x27B0-0x8A9A6
  uint32_t eax = 0x2349EB12u, ebx = 0x439A9EFAu;
  const uint32_t ebp = 0x001ABB09u;
  for (uint32_t ecx = uint32_t(END - 0x27B0) >> 3, esi = 0x27B0; ecx; ecx--, esi += 8) {
    wr(esi, rd(esi) ^ eax);
    wr(esi + 4, rd(esi + 4) ^ ebx);
    ebx = (ebx >> 1) | (ebx << 31);                 // ror ebx, 1
    eax = (eax << 9) | (eax >> 23);                 // rol eax, 9
    if (eax & 0x80000000u) eax += ebp;              // cdq / and edx, ebp / add eax, edx
    ebx ^= ecx;
  }
  // the stub's own check of the result (0x25B9 / 0x25CC)
  if (rd(0x27B0) != 0xA24BC68Du || rd(0x27B4) != 0xB35CD79Fu) return false;
  wr(0x21BC, 0x0008A7B6u);   // return address left on the stub's stack by "call 0x5BE2C"
  d.swap(m);
  return std::memcmp(&d[0x50f18], "bin01.bin", 9) == 0;
}
int32_t Exe::i32(uint32_t a) const { int32_t v; std::memcpy(&v, &d[a], 4); return v; }
uint16_t Exe::u16(uint32_t a) const { return uint16_t(d[a] | d[a + 1] << 8); }

// ------------------------------------------------------------------ loading
static std::string cstr(const uint8_t* p, int n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  return s.substr(0, s.find('\0'));
}
static std::string stripCodes(const std::string& s) {   // "~1", "~2" color codes
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '~' && i + 1 < s.size()) { i++; continue; }
    o += s[i];
  }
  return o;
}

void World::parseOrf(const Blob& b) {
  theater = (b.p[0] & 15) + 1;
  tod = std::clamp(b.p[0] >> 4, 1, 5);
  for (int k = 0; k < 3; k++) startAmmo_[k + 1] = b.p[1 + k];   // 0x2a983: AIM-9 / AIM-120 / JDAM
  fuelByte_ = b.p[0x7D];                                          // 0x2aa03: (byte+1) x 5000 lb
  uint32_t n = b.u32(0x284);
  std::vector<std::string> models;
  for (uint32_t i = 0; i < n; i++) {
    std::string s = upper(cstr(b.p + 0x288 + i * 16, 16));
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".PAK") == 0) s.resize(s.size() - 4);
    models.push_back(s);
  }
  size_t p = 0x288 + size_t(n) * 16;
  uint32_t cnt = b.u32(p); p += 4;
  for (uint32_t k = 0; k < cnt; k++, p += 40) {
    const uint8_t* r = b.p + p;
    Unit u;
    int32_t x = b.i32(p), y = b.i32(p + 4), z = b.i32(p + 8);
    u.side = r[0x0D]; u.activeFlag = r[0x0E];
    u.hdg = r[0x0F] * 2 * PI / 256;                 // only byte +0x0F is the heading (256 = 360°)
    uint16_t mi = uint16_t(r[0x10] | r[0x11] << 8);
    u.cls = r[0x12]; u.sub = r[0x13]; u.group = r[0x14]; u.wing = r[0x15];
    u.taxi = r[0x16]; u.route = r[0x17]; u.skill = std::min<int>(r[0x18], 3); u.dormant = r[0x19];
    u.airStart = r[0x1B]; u.noAuto = r[0x1C]; u.tmask = r[0x1D] ? r[0x1D] : 1; u.escort = r[0x1E];
    u.formation = r[0x1F]; u.slot = r[0x26]; u.id = r[0x27];
    // +0x20 = 1: only if [0x22c4d] is set (the previous campaign mission's score reached the threshold,
    // 0xfd30); +0x20 = 2: +0x21 is the required player count (0x2fdfa)
    bool mpOnly = (r[0x20] == 1 && !extraUnits) || (r[0x20] == 2 && r[0x21] > 1);
    if (mi >= models.size()) continue;
    u.model = models[mi];
    u.mpMode = r[0x20];
    u.pos = {double(x), y >= 0 ? y / 16.0 : -1.0, double(z)};      // y: 1/16 wu (half a foot); <0: on the terrain
    wrapPos(u.pos);
    if (mpOnly) { if (u.mpMode == 2) pool_.push_back(u); continue; }
    units.push_back(u);
  }
  auto section = [&](int sz, auto&& emit) {
    uint32_t k = b.u32(p);
    size_t base = p + 4;
    size_t total = 0;
    for (uint32_t j = 0; j < k; j++) {
      uint32_t c = b.u32(base + j * 8), off = b.u32(base + j * 8 + 4);
      total += c;
      emit(j, c, base + off);
    }
    p = base + 8 * size_t(k) + total * sz;
  };
  section(12, [&](uint32_t, uint32_t c, size_t at) {
    std::vector<TaxiNode> v;
    for (uint32_t q = 0; q < c; q++) {
      size_t a = at + q * 12;
      TaxiNode t{b.i32(a) / 65536.0, b.i32(a + 4) / 65536.0, b.u16(a + 8), b.u16(a + 10)};
      t.x -= WORLD * std::floor(t.x / WORLD); t.z -= WORLD * std::floor(t.z / WORLD);
      v.push_back(t);
    }
    taxis_.push_back(v);
  });
  section(16, [&](uint32_t, uint32_t c, size_t at) {
    std::vector<Waypoint> v;
    for (uint32_t q = 0; q < c; q++) {
      size_t a = at + q * 16;
      Waypoint w{{b.i32(a) / 65536.0, b.i32(a + 4) / 65536.0, b.i32(a + 8) / 65536.0}, b.u16(a + 12), b.u16(a + 14)};
      wrapPos(w.pos);
      v.push_back(w);
    }
    routes_.push_back(v);
  });
  uint32_t nt = b.u32(p); p += 4;
  for (uint32_t k = 0; k < nt; k++, p += 12) {
    const uint8_t* t = b.p + p;
    triggers_.push_back({t[0], t[1], int(t[2] | t[3] << 8), t[4], t[5], t[6]});
    if (t[4] == 3) goalTotal[std::min<int>(t[5], 3)]++;
  }
  uint16_t nr = b.u16(p); p += 2;
  for (uint32_t k = 0; k < nr && p + 0x90 <= b.n; k++, p += 0x90)
    radio.push_back({stripCodes(cstr(b.p + p, 0x80)), cstr(b.p + p + 0x80, 16)});
}

bool World::init(const Res& res, const Exe& exe, const std::string& name, std::string& err, const CreateSpec* spec) {
  exe_ = &exe;
  mission = upper(name);
  Blob b = res.get(mission + ".ORF");
  if (!b.ok()) { err = "No such mission: " + mission; return false; }
  // tables from the original program
  for (int i = 0; i < 102; i++) TH_[i] = exe.u16(0x2c7f0 + i * 2);       // thrust per %
  for (int i = 0; i < 257; i++) { CLD_[i] = exe.i32(0x2c0d0 + i * 4); CLL_[i] = exe.i32(0x2bcd0 + i * 4); }
  for (int i = 0; i < 200; i++) AUTH_[i] = exe.i32(0x2c4d0 + i * 4);    // control effectiveness
  wdef.clear();
  for (int i = 0; i < 6; i++) {
    uint32_t a = 0x39034 + i * 0x40;
    WeaponDef w;
    uint32_t np = uint32_t(exe.i32(a));
    w.name = np < exe.d.size() ? cstr(&exe.d[np], 32) : "?";
    w.dmg = exe.i32(a + 8); w.life = exe.i32(a + 0xC); w.range = exe.i32(a + 0x10);
    w.maxTurn = exe.i32(a + 0x14); w.turnBuild = exe.i32(a + 0x18); w.gimbal = exe.i32(a + 0x1C);
    w.accel = exe.i32(a + 0x20); w.vmax = exe.i32(a + 0x24);
    w.mask = exe.u8(a + 0x38); w.seeker = exe.u8(a + 0x39);
    wdef.push_back(w);
  }
  perf.clear();
  for (int i = 0; i < 10; i++) {
    uint32_t a = 0x24618 + i * 0x50;
    perf.push_back({exe.i32(a), exe.i32(a + 4), exe.i32(a + 8), exe.i32(a + 0x10), exe.i32(a + 0x14), exe.i32(a + 0x18),
                    exe.i32(a + 0x20), exe.i32(a + 0x34), exe.u16(a + 0x24), exe.u16(a + 0x26), exe.i32(a + 0x30), {}});
    for (int k = 0; k < 12; k++) perf.back().evMask[k] = exe.u16(a + 0x38 + k * 2);
  }
  units.clear(); routes_.clear(); taxis_.clear(); triggers_.clear(); radio.clear(); pool_.clear();
  std::memset(goalTotal, 0, sizeof goalTotal);
  parseOrf(b);
  if (spec) applyCreate(*spec);
  units0_ = units;
  return true;
}

// Create Mission: the template's base and player remain; the enemies and allies are copied according to the settings
// from the template's unit samples (pool_); every enemy unit and target is a primary goal.
void World::applyCreate(const CreateSpec& cs) {
  tod = std::clamp(cs.tod, 1, 5);
  startAmmo_[1] = cs.aim9; startAmmo_[2] = cs.amraam; startAmmo_[3] = cs.jdam;
  triggers_.clear(); std::memset(goalTotal, 0, sizeof goalTotal);
  for (size_t i = units.size(); i-- > 0;)                                   // the configured ones instead of the template's own escort
    if (units[i].cls == C_AIR && units[i].side == 8) units.erase(units.begin() + long(i));
  int nextId = 20;
  auto poolFind = [&](const char* model, int cls, int grp) -> Unit* {
    for (auto& u : pool_) if (u.model == model && (cls < 0 || u.cls == cls) && (grp < 0 || u.group == grp)) return &u;
    return nullptr;
  };
  auto addGoal = [&](int id) { triggers_.push_back({1, 0, id, 3, 0, 0}); goalTotal[0]++; };   // kind 1: unit identifier
  auto spawnWing = [&](const char* model, int n, int side, int grp, int route, int skill, double spread, bool goal) {
    Unit* t = poolFind(model, C_AIR, -1);
    if (!t || route >= int(routes_.size()) || routes_[route].empty() || n <= 0) return;
    for (int k = 0; k < n && nextId < 255; k++) {
      Unit u = *t;
      u.side = side; u.group = grp; u.wing = (k || grp == 7) ? 1 : 0; u.slot = k + (grp == 7); u.id = nextId++;   // the leader of group 7 is the player
      u.route = route; u.airStart = 1; u.formation = 3; u.skill = std::clamp(skill, 0, 3); u.mpMode = 0;
      D3 w = routes_[route][0].pos;
      u.pos = {w.x + 80.0 * k, w.y, w.z + 60.0 * k};
      wrapPos(u.pos);
      u.dormant = 0; u.activeFlag = 0;
      units.push_back(u);
      if (goal) addGoal(u.id);
    }
  };
  static const char* EN[4] = {"EF2000", "MIG27", "MIG29", "SU27"};
  static const char* FR[3] = {"F14", "F15", "F16"};
  for (int k = 0; k < 4; k++)
    if (cs.enemy[k].type >= 1 && cs.enemy[k].type <= 4)
      spawnWing(EN[cs.enemy[k].type - 1], cs.enemy[k].num, 4, 12 + k, (k & 1) ? 15 : 5, cs.enemy[k].skill, 80, true);
  for (int k = 0; k < 2; k++)
    if (cs.friendly[k].type >= 1 && cs.friendly[k].type <= 3)
      spawnWing(FR[cs.friendly[k].type - 1], cs.friendly[k].num, 8, k ? 11 : 7, 7, cs.friendly[k].skill, 80, false);
  auto copyGround = [&](const char* model, int grp, int n, int skill, bool goal) {
    int c = 0;
    for (auto& p : pool_) {
      if (c >= n || p.model != model || (grp >= 0 && p.group != grp) || nextId >= 255) continue;
      Unit u = p; u.id = nextId++; u.skill = std::clamp(skill, 0, 3); u.mpMode = 0; u.dormant = 0;
      units.push_back(u); c++;
      if (goal) addGoal(u.id);
    }
  };
  if (cs.ground == 1) copyGround("GSP_11H", -1, 4, 1, true);              // "chemical plant": four objects
  else if (cs.ground == 2) copyGround("GSP_08", -1, 4, 1, true);          // "nuclear power plant"
  if (cs.samNum > 0) {
    copyGround("RDAR_03", 5, 3, cs.samSkill, false);                      // the missile batteries' radars
    copyGround("SA2", 5, cs.samNum, cs.samSkill, true);
  }
  if (cs.aaaNum > 0) copyGround("ARTEMIS", 6, cs.aaaNum, cs.aaaSkill, true);
}

void World::enableAiVm(const std::vector<uint8_t>* img) { ai_ = std::make_shared<AiBridge>(img); }
void World::aiReseed(Unit& u) { if (ai_) ai_->reseed(*this, u); }
int World::nearestTaxiNode(const Unit& u) const {
  if (u.taxi < 0 || u.taxi >= int(taxis_.size())) return 0;
  const auto& t = taxis_[u.taxi];
  int best = 0; double bd = 1e30;
  for (int k = 0; k < int(t.size()); k++) {
    D3 d = wdelta(D3{t[k].x, u.pos.y, t[k].z}, u.pos);
    double r = std::hypot(d.x, d.z);
    if (r < bd) { bd = r; best = k; }
  }
  return best;
}

void World::restart() {
  if (ai_) ai_->dirty = true;
  units = units0_;
  missiles.clear(); bullets.clear(); decoys.clear(); blasts.clear(); msgs.clear(); runways_.clear();
  chute = Chute{}; ejected = 0; tally = Tally{};
  now = 0; stepNo = 0; kills = 0; crashed = 0; playerAirKills = playerGroundKills = 0; apMode = 0; playerWp = 0;
  std::memset(goalDone, 0, sizeof goalDone); std::memset(firedBy, 0, sizeof firedBy);
  std::memset(globalEv_, 0, sizeof globalEv_);
  globalEv_[0] = true;                                     // mission start (0x54e3a: 3, 0)
  for (auto& t : triggers_) t.fired = false;
  player = nullptr;
  for (auto& u : units) {
    u.dead = false; u.target = nullptr; u.leader = nullptr; u.order = 0; u.launchers.clear(); u.reached = false;
    u.wasAirborne = false; u.timer = u.wp = u.node = u.fireCd = 0; u.lastLaunch = -100000;
    u.isPlayer = false; u.pitch = u.roll = 0;
    if (u.pos.y < 0 || (u.cls != C_AIR)) u.onGround = true;
    if (u.cls == C_AIR && u.sub < int(perf.size())) { u.hp = u.maxhp = perf[u.sub].hp; }
    else if (u.cls == C_STRUCT) { static const int HP[5] = {128, 256, 512, 1024, 16384}; u.hp = u.maxhp = u.sub < 5 ? HP[u.sub] : 100; }  // 0x25630
    else if (u.cls == C_RUNWAY) u.hp = u.maxhp = 1 << 30;
    else u.hp = u.maxhp = 100;                                                                       // 0x3f7bd
    if (u.cls == C_RUNWAY) runways_.push_back(&u);
  }
  // the player: the first own (side 2) F-22 leader
  for (auto& u : units) if (u.cls == C_AIR && u.side == 2 && u.model == "F22" && u.wing == 0) { player = &u; break; }
  if (!player) for (auto& u : units) if (u.cls == C_AIR && u.side == 2) { player = &u; break; }
  if (!player) for (auto& u : units) if (u.model == "F22") { player = &u; break; }
  if (!player) { units.push_back(Unit{}); player = &units.back(); player->model = "F22"; player->cls = C_AIR; player->side = 2; player->pos = {32768, -1, 32768}; }
  Unit& P = *player;
  P.isPlayer = true; P.hp = P.maxhp = 512;                   // 0x238c2
  for (auto& u : units) if (u.pos.y < 0) u.pos.y = groundWU(u.pos.x, u.pos.z);
  for (auto& u : units) u.onGround = u.cls != C_AIR || u.pos.y <= groundWU(u.pos.x, u.pos.z) + 1;
  // player flight state
  for (int k = 0; k < 4; k++) ammo[k] = startAmmo_[k];
  ammo[0] = 480; chaff = flares = 100;
  fuel = (fuelByte_ + 1) * 5000.0;
  f = dirOf(P.hdg, 0); up = {0, 1, 0};
  yawR = pitchR = rollR = 0; noz = 0; lift = 81; gDisp = 1;
  gearPos = 0; gearWantUp = false; flapPos = 64; flapsWant = true; brakePos = 0; bayPos = 0; bayTimer = -1; refire = 99;
  weapon = ammo[2] > 0 ? 2 : ammo[1] > 0 ? 1 : 0;
  lockTgt = nullptr; lockState = 0; shootList.clear();
  if (P.onGround) {
    P.pos.y = groundInterpWU(P.pos.x, P.pos.z) + 0xBC00 / 65536.0;
    v = {0, 0, 0}; thrCmd = 0; thrAct = 0;
  } else {
    v = f * (0x18000 / 65536.0);                 // 0x2aad3: airborne start at ~569 knots
    thrCmd = 40; thrAct = 40 << 8; gearPos = 64; gearWantUp = true; flapPos = 0; flapsWant = false;
  }
  // linking radars and launchers (0x26879: within 1900 wu)
  for (auto& r : units)
    if (r.cls == C_GROUND && r.sub == 2)
      for (auto& l : units)
        if (&l != &r && l.cls == C_GROUND && l.sub == 1 && l.side == r.side && len(wdelta(l.pos, r.pos)) < 1900)
          r.launchers.push_back(&l);
  // initial state of the AI aircraft
  for (auto& u : units) {
    if (&u == player) continue;
    u.active = !u.dormant;
    if (u.cls != C_AIR) continue;
    if (!u.activeFlag) { u.st = S_PARKED; continue; }
    // leader: the first non-wingman of the group
    if (u.wing)
      // the player gets its own side code (2), its escort is friendly (8): it can be a leader too (0x26714: per-group chain)
      for (auto& l : units) if (&l != &u && l.cls == C_AIR && (l.side == u.side || (l.isPlayer && !hostile(u, l))) && l.group == u.group && !l.wing) { u.leader = &l; break; }
    const Perf& pf = perf[std::min<int>(u.sub, 9)];
    if (!u.onGround || u.airStart) {
      u.onGround = false;
      if (u.pos.y < groundWU(u.pos.x, u.pos.z) + 50) u.pos.y = groundWU(u.pos.x, u.pos.z) + 300;
      u.speed = pf.cruise / 65536.0;
      u.st = u.leader ? S_FORM : S_ROUTE;
      u.wasAirborne = true;
    } else {
      // APPROXIMATION: the player's escort waits for the player to take off (I did not find the original's launch), and taxiing starts from the nearest point
      u.st = (u.active && !u.leader && u.taxi < int(taxis_.size())) ? S_TAXI : S_PARKED;
      u.node = nearestTaxiNode(u);
      u.speed = 0;
    }
  }
  spawnGround_ = P.onGround;
  if (spawnGround_) say("Takeoff: - (minus) = afterburner, pull at about 190 knots (down arrow), then G = gear up", 8);
}

// ------------------------------------------------------------------ terep
double World::groundWU(double x, double z) const {
  if (!depth_ || depth_->empty()) return 0;
  // 0x5271a: (x>>23, z>>23) grid point and its 3 neighbors, maximum, x4 wu
  int ix = int(std::floor(x / 128)) & 511, iz = int(std::floor(z / 128)) & 511;
  int ix1 = (ix + 1) & 511, iz1 = (iz + 1) & 511;
  const uint8_t* D = depth_->data();
  int h = std::max(std::max(D[iz * 512 + ix], D[iz * 512 + ix1]), std::max(D[iz1 * 512 + ix1], D[iz1 * 512 + ix]));
  return h * 4.0;
}
// 0x52796: the 128 wu cell split into two triangles along the (fx <= fz) / (fx > fz) diagonal, linear interpolation (with the original's integer arithmetic)
double World::groundInterpWU(double x, double z) const {
  if (!depth_ || depth_->empty()) return 0;
  int64_t xi = int64_t(std::floor(x)), zi = int64_t(std::floor(z));
  int ix = int((xi >> 7) & 511), iz = int((zi >> 7) & 511);
  int fx = int(xi & 0x7f), fz = int(zi & 0x7f);
  const uint8_t* D = depth_->data();
  auto H = [&](int i, int j) { return int(D[(j & 511) * 512 + (i & 511)]) * 4; };
  int h;
  if (fx <= fz) {
    int h00 = H(ix, iz), h01 = H(ix, iz + 1), h11 = H(ix + 1, iz + 1);
    h = h00 + (((h01 - h00) * fz) >> 7) + (((h11 - h01) * fx) >> 7);
  } else {
    int gx = 128 - fx, gz = 128 - fz;
    int a = H(ix + 1, iz + 1), b = H(ix + 1, iz), c = H(ix, iz);
    h = a + (((b - a) * gz) >> 7) + (((c - b) * gx) >> 7);
  }
  return double(h);
}

bool World::los(D3 a, D3 b) const {   // 0x30c05: line of sight stepping along the terrain
  D3 d = wdelta(b, a);
  double L = len(d);
  int n = std::max(1, int(L / 32));
  for (int i = 1; i < n; i++) {
    D3 p = a + d * (double(i) / n);
    if (groundWU(p.x, p.z) > p.y) return false;
  }
  return true;
}

// ------------------------------------------------------------------ helpers
double World::auth(int32_t vraw) const { return AUTH_[std::clamp(vraw >> 10, 0, 199)] / 65536.0; }

bool World::hostile(const Unit& a, const Unit& b) const {
  bool aEnemy = a.side == 4, bEnemy = b.side == 4;
  if (a.side == 1 || b.side == 1 || a.side == 0x10 || b.side == 0x10) return false;
  return aEnemy != bEnemy;
}

double World::stealthOf(const Unit& t) const {
  if (t.isPlayer) return (radarOn ? 0.75 : 0.4) + 0.25 * bayPos / 32.0;   // 0x2b6d8
  if (t.side == 2) return 0.4;                                             // 0x268a5
  return 1.0;
}

// 0xe310: only in the air, once; EJECT.WAV (0x1d), the pilot starts with the aircraft's velocity plus a
// 0.5 wu/step kick in the aircraft's "up" direction (0xe3c7). The aircraft then continues flying uncontrolled.
void World::eject() {
  if (!player || player->onGround || ejected || crashed) return;
  ejected = 1;
  apMode = 0;
  snd(29, player->pos, 0xC8, 1, false);
  chute.on = true; chute.pos = player->pos; chute.vel = v + up * 0.5; chute.hdg = player->hdg; chute.t0 = now;
  // the aircraft's handler becomes that of the falling wreck (+0x53 = 0x2bbc2), 2..33 debris pieces (0x317b4); the impact is not a crash (0x2b23b)
  player->speed = len(v);
  killEv.push_back({int(player - units.data()), true, 2 + rnd(32)});
  player->dead = true;
  say("EJECT", 3);
}

// 0x105c0: air kill 250, ground 100 (the original adds it twice: 200), missile hit 150, shell hit 25,
// landing 1000, completed BONUS goal 1000 (0x10540, 0x23c32), integrity 500 · hp / maxhp, +1000 in a campaign
int World::missionScore(bool campaign) const {
  int s = 250 * playerAirKills + 2 * 100 * playerGroundKills + 150 * tally.missilesHit + 25 * tally.bulletsHit;
  if (tally.landed) s += 1000;
  s += 1000 * goalDone[2];
  if (player && player->maxhp > 0) s += std::max(0, player->hp) * 500 / player->maxhp;
  if (campaign) s += 1000;
  return s;
}

void World::killPlayer(const std::string& why) {
  if (crashed) return;
  crashed = 1; crashWhy = why;
  player->dead = true;
  blasts.push_back({player->pos, now, 3.5f});
  say(why + ". Enter: restart", 600);
}

void World::kill(Unit& t, D3 at, Unit* by) {
  if (t.dead) return;
  t.dead = true;
  blasts.push_back({at, now, t.cls == C_AIR ? 3.2f : 3.0f});
  killEv.push_back({int(&t - units.data()), t.cls == C_AIR && !t.onGround});
  if (t.isPlayer) { killPlayer("You were shot down"); return; }
  snd(SND_EXPLODE, at, 255);                              // 0x312d0
  if (t.side == 4) { kills++; say("Hit: " + t.model + " destroyed", 2); }
  if (by && by->isPlayer && t.side == 4 && t.cls == C_AIR) snd(SND_YEEHAW, at, 0xD2, 0.875, false);   // 0x2bb33
  if (by && by->isPlayer && t.side == 4) (t.cls == C_AIR ? playerAirKills : playerGroundKills)++;
}

void World::damage(Unit& t, int dmg, D3 at, Unit* by) {
  if (t.dead) return;
  int32_t f = 0x8001 + (rnd(0x8000));                 // 0x3901c: random multiplier 0.5-1.0
  int d = int((int64_t(dmg) * f) >> 16);
  t.hp -= d;
  if (ai_ && by && t.hp > 0) ai_->onHit(*this, t, by);
  if (t.hp <= 0) kill(t, at, by);
  else if (t.isPlayer) say("You were hit! Airframe " + std::to_string(std::max(1, t.hp * 100 / t.maxhp)) + "%", 2);
}

// ------------------------------------------------------------------ player flight model (0x2dc22)
static int64_t tdiv(int64_t a, int64_t b) { return a / b; }   // C++: truncating division, like the original
static int64_t chain(int64_t x) { x = tdiv(x, 0xAF00000); x = x * 0x347EF; x >>= 3; return tdiv(x, 0x640); }
static int64_t dens(int64_t alt16) { return (std::max<int64_t>(0, 0x17700000LL - alt16) * 14) >> 16; }   // 0x2d6fb
static int64_t q0(int64_t v) { int64_t a = ((v << 1) * 0x4E06) >> 16; return (a * a) >> 16; }
static int64_t qp(int64_t v, int64_t alt) { return ((q0(v) * dens(alt)) >> 16) >> 1; }
static int64_t interp(const int32_t* tab, int64_t x, int n) {
  int64_t i = std::min<int64_t>(x >> 16, n - 2);
  return tab[i] + (((int64_t(tab[i + 1]) - tab[i]) * (x & 0xFFFF)) >> 16);
}

int32_t World::thrustRaw(double altWU) {                 // 0x2cbaa
  int pct = std::min(thrAct >> 8, 101);
  int64_t t = (int64_t(TH_[pct]) * 0x222E) >> 9;
  int a = int(altWU);
  if (a > 0xEA6) return 0;
  if (a > 0x8CA) { int64_t fa = (int64_t(0xEA6 - a) << 16) / 0x5DC; t = (fa * t) >> 16; }
  return int32_t(((t * 0x2B * 0x28) >> 16) >> 1);
}

void World::throttleKey(int pct) {                       // 0x27470: changing the throttle switches off the AUTOPILOT
  int prev = thrCmd;
  thrCmd = pct;
  if (apMode == 2) apMode = 0;
  if (pct && !prev) snd(SND_RADIO, player->pos, 0xFF, 1, false, "OCV032.WAV");        // 0x2749d: throttle from zero upward
  else if (!pct && prev) snd(SND_RADIO, player->pos, 0xFF, 1, false, "OCV033.WAV");   // 0x274b7: throttle pulled down to zero
}
void World::throttleStep(int dir) {
  static const int L[] = {0, 60, 70, 80, 90, 100, 110};
  int i = 0;
  for (int k = 0; k < 7; k++) if (L[k] <= thrCmd) i = k;
  i = std::clamp(i + dir, 0, 6);
  thrCmd = L[i];
}
void World::toggleGear() {                               // 0x2db28: does not move above 350 knots / on the ground
  if (player->onGround) { say("The gear cannot be retracted on the ground", 1.5f); return; }
  if (len(v) * 65536 > 0xEC71 && !gearWantUp) return;
  if (len(v) * 65536 > 0xEC71 && gearWantUp) {
    say("The gear cannot be lowered above 350 knots", 1.5f);
    snd(SND_GEAR_WARN, player->pos, 0xFF, 1, false);      // 0x27833
    return;
  }
  gearWantUp = !gearWantUp;
  snd(SND_GEAR, player->pos, 0x46);                       // 0x27856 (0x2d7ec)
  say(gearWantUp ? "Gear UP" : "Gear DOWN", 1.5f);
}
void World::toggleFlaps() {
  if (len(v) * 65536 > 0x10E38 && !flapsWant) { say("The flaps cannot be lowered above 400 knots", 1.5f); return; }
  flapsWant = !flapsWant;
  snd(SND_FLAPS, player->pos, 0x46);                      // 0x278e1
  say(flapsWant ? "Flaps DOWN" : "Flaps UP", 1.5f);
}

void World::stepPlayer(const Controls& c) {
  Unit& P = *player;
  D3 right = cross(up, f);
  // 0x40d2a: applying the angular rates about the body axes
  f = rot(f, up, yawR * ANG); right = cross(up, f);
  f = rot(f, right, -pitchR * ANG); up = rot(up, right, -pitchR * ANG);
  up = rot(up, f, -rollR * ANG);
  f = norm(f); up = norm(up - f * dot(up, f)); right = cross(up, f);

  // gear / brake / flaps (0x2d7b7, 0x2d718, 0x2d74d): 1 per step
  gearPos += gearWantUp ? (gearPos < 64) : -(gearPos > 0);
  flapPos += flapsWant ? (flapPos < 64) : -(flapPos > 0);
  if (c.brake && brakePos == 0) snd(SND_BRAKE, P.pos, 0x3C);   // 0x27897
  brakePos += c.brake ? (brakePos < 32) : -(brakePos > 0);

  int64_t alt16 = int64_t(P.pos.y * 65536);
  int64_t rho = dens(alt16);
  // thrust
  if (fuel <= 0) { thrCmd = 0; thrAct = 0; }
  {
    int d = (thrCmd << 8) - thrAct;
    thrAct += std::clamp(d, -0xC4, 0xC4);
  }
  int64_t T = thrustRaw(P.pos.y);
  // angle of attack from the body-frame velocity (0x2d277)
  D3 vb{dot(v, right), dot(v, up), dot(v, f)};
  double aoa = std::atan2(-vb.y, std::hypot(vb.x, vb.z));
  int64_t aoaRawI = int64_t(aoa / ANG);
  aoaRaw = double(aoaRawI);
  int64_t vraw = int64_t(len(v) * 65536);
  bool flaps = flapPos >= 32, gearDown = gearPos < 64, brk = brakePos > 0;
  int64_t aidx = std::llabs(aoaRawI) >> 16;
  stallWarn = P.isPlayer && !P.onGround ? (aidx >= 86 ? 2 : aidx >= 70 ? 1 : 0) : 0;
  // lift (0x2d395)
  int64_t L;
  if (thrCmd && aidx <= 1 && vraw >= 0x871C) L = 0x51;              // assist level flight
  else {
    int64_t s = aoaRawI + (flaps ? 0x50000 : 0);
    bool neg = s < 0;
    int64_t cl = interp(CLL_, std::llabs(s), 257);
    if (neg) cl = -cl;
    int64_t e = (cl * 3) >> 3;
    e = (e * 0x200000) >> 16;
    L = chain(e * qp(vraw, alt16)) >> 1;
  }
  lift = int(L);
  // drag (0x2c970) + induced (0x2cb2f)
  int64_t cd = 0x3333 + (thrCmd == 0 ? 0xC000 : 0) + (flaps ? 0x4000 : 0) + (brk ? 0x9C40 : 0);
  int64_t e0 = (q0(vraw) * cd) >> 17;
  int64_t c2 = 0x20000 + (brk ? 0x4000 : 0) + (gearDown ? 0x2000 : 0);
  e0 = (e0 * c2) >> 16;
  e0 = chain(e0 * rho);
  if (P.onGround && gearDown) { e0 += 0x40; if (brk) e0 += 0x80; }
  if (e0 <= 0) e0 = 2;
  int64_t d0 = e0 >> 1;
  int64_t ei = interp(CLD_, std::llabs(aoaRawI + (flaps ? 0xA0000 : 0)), 257) >> 3;
  ei = (ei * 0x200000) >> 16;
  int64_t d1 = chain(ei * qp(vraw, alt16)) >> 1;
  int64_t D = d0 + d1;
  // sideslip (0x2cdb0): beta = atan(-vx, |vy,vz|) from the body-frame velocity;
  // side force perpendicular to the velocity, and additional drag |..|/2 (0x2c93c, 0x2c938)
  int64_t betaI = int64_t(std::atan2(-vb.x, std::hypot(vb.y, vb.z)) / ANG);
  int64_t qpv = qp(vraw, alt16);
  int64_t Sside = chain((((betaI * 0xDB6) >> 16) * 0x100000 >> 16) * qpv);
  D += std::llabs(chain(((((betaI * 0x202) >> 16) * qpv) >> 16) * 0x100000)) >> 1;
  gDisp = P.onGround ? 1.0 : 1 + (L / 81.0 - 1) * 0.6866;           // 0x2d449

  // steering (0x2d0ab)
  double a = auth(int32_t(vraw));
  int sx = c.stickX, sy = c.stickY;
  bool gearUp = gearPos == 64;
  if (gearUp) noz = -(sy > 0 ? 1 : sy < 0 ? -1 : 0) * double(std::abs(sy) * std::abs(sy >> 1) * 0x6D60 >> 16);
  else noz -= std::trunc(noz / 4);
  if (L != 0) {
    int s = sy < 0 ? sy / 2 : sy;
    int64_t e = int64_t(s) * std::abs(s >> 1);
    pitchR += double((int64_t(a * 65536 * 90000) >> 16) * e >> 16);
  } else if (!P.onGround) {                                           // stall: auto-rotation, nose down
    double rollAng = std::atan2(-right.y, up.y);
    rollR += rollAng >= 0 ? 65536 : -65536;
    double pitchAng = std::asin(clampd(f.y, -1, 1));
    double dp = clampd(-PI / 2 - pitchAng, -0.2575 * DEG, 0.2575 * DEG);
    D3 hr = norm(cross({0, 1, 0}, f));
    f = rot(f, hr, -dp); up = rot(up, hr, -dp);
  }
  rollR += (sx >= 0 ? 1 : -1) * (4.5 * sx) * (4.5 * sx);
  if (!P.onGround) {                                                  // nose drop in a bank (0x2cfe2)
    double pitchAng = std::asin(clampd(f.y, -1, 1)), rollAng = std::atan2(-right.y, up.y);
    double dp = clampd(-PI / 2 - pitchAng, -0.118 * DEG, 0.118 * DEG) + std::max(0.0, 0.118 * DEG * std::cos(rollAng));
    D3 hr = norm(cross({0, 1, 0}, f));
    if (std::fabs(dp) > 1e-12) { f = rot(f, hr, -dp); up = rot(up, hr, -dp); }
    double m = -(noz * double(T)) / 128;                              // thrust-vector moment
    pitchR += a * m * 90000 / 65536;
  }
  // integration (0x2d4eb)
  right = cross(up, f);
  v += f * (double(T) / 65536);
  double s = len(v);
  if (s > 1e-9) v = v * std::max(0.0, (s - double(D) / 65536) / s);
  D3 vh = norm(v);
  D3 ld = len(v) > 1e-6 ? norm(up - vh * dot(up, vh)) : up;
  v += ld * (double(L) / 65536);
  v.y -= 81.0 / 65536;                                                // gravity: 81 = 0.98 g
  {                                                                   // side force (0x2d65d)
    D3 sd = len(v) > 1e-6 ? norm(right - vh * dot(right, vh)) : right;
    v += sd * (double(Sside) / 65536);
  }
  P.pos += v;
  wrapPos(P.pos);
  // fuel (0x2d84c)
  int pct = std::min(thrAct >> 8, 101);
  double burn = TH_[pct] * 64.0 / 65536;
  if (TH_[pct] != 512) burn /= 2;
  fuel = std::max(0.0, fuel - burn);
  // automatic retraction (0x2db28)
  int64_t sp = int64_t(len(v) * 65536);
  if (sp > 0xEC71 && !gearWantUp && !P.onGround) { gearWantUp = true; say("Gear retracted automatically", 2); snd(SND_RADIO, player->pos, 0xFF, 1, false, "OCV047.WAV"); }   // 0x2db64
  if (sp > 0x10E38 && flapsWant) flapsWant = false;
  // rudder (0x2d48d) and damping (0x2cf85)
  yawR += c.rudder * 32;
  pitchR = pitchR * 7 / 8; yawR = yawR * 7 / 8; rollR = rollR / 2;

  // state into the unit
  P.speed = len(v);
  P.hdg = std::atan2(f.x, f.z); P.pitch = std::asin(clampd(f.y, -1, 1)); P.roll = std::atan2(-right.y, up.y);   // + = right wing down

  // ground (0x2b555)
  // 0x2b555: pre-filter with the maximum of the four corners (0x5271a), contact with the interpolated ground (0x52796)
  double gh = groundWU(P.pos.x, P.pos.z);
  if (P.pos.y <= gh + 0xBC00 / 65536.0 && P.pos.y <= (gh = groundInterpWU(P.pos.x, P.pos.z)) + 0xBC00 / 65536.0) {
    if (!P.onGround) touchdown(gh);
    if (!crashed) playerGround(gh);
  } else {
    if (P.onGround) { P.wasAirborne = true; say("Airborne! G: retract the gear", 3); }
    P.onGround = false;
  }
}

void World::touchdown(double gh) {                        // 0x2b3e8: any fault = crash
  Unit& P = *player;
  double bank = P.roll / DEG, pitch = P.pitch / DEG, kt = len(v) * 378.95;
  bool nearBase = false;
  for (Unit* r : runways_) if (len(wdelta(r->pos, P.pos)) <= 512) nearBase = true;   // 0x2000000 (16.16)
  if (gearPos != 0) killPlayer("Belly landing - the gear was not down");
  else if (bank < -7.0 || bank > 22.5) killPlayer("Too much bank at touchdown");
  else if (pitch < 0 || pitch > 33.75) killPlayer("Bad pitch angle at touchdown");
  else if (len(v) * 65536 > 0x10400) killPlayer("Touchdown too fast (" + std::to_string(int(kt)) + " knots)");
  else if (!nearBase) killPlayer("Landing outside an airfield");
  else { P.onGround = true; tally.landed = true; say("You have landed - B: brake", 3); }   // 0x2b4e8: +0x10 landing
  snd(SND_SCREECH, P.pos, 255);                           // 0x2b4d8
  (void)gh;
}

void World::playerGround(double gh) {
  Unit& P = *player;
  P.onGround = true;
  P.pos.y = gh + 0xBC00 / 65536.0;
  v.y = 0;
  // wings level, pitch damps out, no negative pitch
  D3 hf = norm(D3{f.x, 0, f.z});
  double pitch = std::max(0.0, std::asin(clampd(f.y, -1, 1)));
  pitch -= pitch / 32;
  f = norm(hf * std::cos(pitch) + D3{0, std::sin(pitch), 0});
  D3 right = norm(cross({0, 1, 0}, f));
  up = cross(f, right);
  rollR = 0;
  if (len(v) * 378.95 < 6) {                              // stopped: refueling, rearming (0x2a983)
    for (int k = 1; k < 4; k++) ammo[k] = startAmmo_[k];
    ammo[0] = 480; chaff = flares = 100; fuel = (fuelByte_ + 1) * 5000.0;
  }
}

// ------------------------------------------------------------------ fegyverek
void World::cycleWeapon() {                               // 0x3a536
  for (int k = 0; k < 4; k++) {
    weapon = (weapon + 3) & 3;
    if (weapon == 0 || ammo[weapon] > 0) break;
  }
  lockState = 0;
  say(std::string("Fegyver: ") + wdef[weapon].name, 1.5f);
}

void World::selectWeapon(int w) {                        // 0x3a536: class 0 cannon, 1 SIDEWINDER, 2 AMRAAM, 3 JDAM
  weapon = w & 3;
  lockState = 0;
  say(std::string("Fegyver: ") + wdef[weapon].name, 1.5f);
}

// ---------------------------------------------------------------- target selection (the original's 0x30974..0x31022)
// Candidates come in the unit list's order (+0x34 / +0x30 chain = the load order). Common conditions:
// distance < 15200 wu (0x3b60), and the terrain does not block it (0x30c05, in 64 wu steps).
static const double TGT_RANGE = 15200;                    // 0x3b600000 / 0x1db00000 with half-distance
int World::typeMask(const Unit& u) const {
  int m = (u.cls == C_AIR && !u.onGround) ? 1 : 2;        // 0x2b6a0 / 0x2b5c4: in the air / on the ground
  // 0x245c5 / 0x2453c: subject of a goal trigger (response 3, category != 3): kind 1 unit identifier,
  // kind 0 by airborne, kind 2 by grounded group
  for (auto& t : triggers_) {
    if (t.resp != 3 || t.a == 3) continue;
    if ((t.kind == 1 && u.id == t.subject) || (t.kind == 0 && (m & 1) && u.group == t.subject) ||
        (t.kind == 2 && (m & 2) && u.group == t.subject)) { m |= 0x40; break; }
  }
  return m;
}
bool World::lineOfSight(const Unit& u) const {
  D3 d = wdelta(u.pos, player->pos);
  int n = int(len(d) / 64);
  if (n < 1) return true;
  D3 s = d * (1.0 / n), p = player->pos;
  for (int k = 0; k < n; k++) {
    if (p.y < groundWU(p.x, p.z)) return false;
    p += s;
  }
  return true;
}
double World::boreCos(const Unit& u) const {
  D3 d = wdelta(u.pos, player->pos);
  double r = len(d);
  return r > 1e-9 ? dot(d, f) / r : 1.0;
}
// Tab / Shift+Tab (0x30edd / 0x30f4d): the first unit after / before the current target that matches the weapon's type mask
// (0x27c16: air weapon 1 = airborne, JDAM 2 = on the ground), lies in the 30.9° cone (0x2c) ahead of the nose,
// within 15200 wu, and is visible. It does not look sideways. If it wrapped around, there is no target.
// [ / ] (0x30dfa / 0x30dcc): the previous / next element of the shoot list (wrapping); empty list: no target.
void World::cycleTargets(bool tab, int dir) {
  lockState = 0; curAz = curEl = curX = curY = 0;
  if (!tab) {
    std::vector<Unit*> v;
    for (Unit* u : shootList) if (u && !u->dead) v.push_back(u);
    if (v.empty()) { lockTgt = nullptr; return; }
    int n = int(v.size()), i = -1;
    for (int k = 0; k < n; k++) if (v[k] == lockTgt) i = k;
    i = i < 0 ? 0 : ((i + dir) % n + n) % n;               // if the target is not in the list: the first
    lockTgt = v[i];
    return;
  }
  static const int MASK[4] = {1, 1, 1, 2};
  int mask = MASK[weapon & 3];
  const double cone = std::cos(30.94 * PI / 180);
  int n = int(units.size());
  if (!n) return;
  int start = lockTgt ? int(lockTgt - units.data()) : (dir > 0 ? -1 : n);
  for (int k = 1; k <= n; k++) {
    int i = start + dir * k;
    i = ((i % n) + n) % n;
    Unit& u = units[i];
    if (lockTgt && &u == lockTgt) break;
    if (u.dead || &u == player || !(typeMask(u) & mask)) continue;
    if (len(wdelta(u.pos, player->pos)) >= TGT_RANGE || boreCos(u) < cone || !lineOfSight(u)) continue;
    lockTgt = &u;
    return;
  }
  lockTgt = nullptr;
}
// ' (0x30fba): the unit nearest to the crosshair within the 12.7° cone (0x12), within 15200 wu, visible;
// it ignores type and side.  Enter (0x30d2e): shoot list - of the enemy units matching the weapon's mask (0x27c1b: air
// weapon 1, JDAM 0x40 = mission goal) the 4 nearest, by distance; the target is the first.
void World::nearestTarget(bool bore) {
  lockState = 0; curAz = curEl = curX = curY = 0;
  if (bore) {
    const double cone = std::cos(12.66 * PI / 180);
    Unit* best = nullptr;
    double bc = cone;
    for (auto& u : units) {
      if (u.dead || &u == player) continue;
      if (len(wdelta(u.pos, player->pos)) >= TGT_RANGE) continue;
      double c = boreCos(u);
      if (c <= bc || !lineOfSight(u)) continue;
      bc = c; best = &u;
    }
    lockTgt = best;
    return;
  }
  static const int MASK[4] = {1, 1, 1, 0x40};
  int mask = MASK[weapon & 3];
  std::vector<std::pair<double, Unit*>> c;
  for (auto& u : units) {
    if (u.dead || &u == player || !hostile(*player, u) || !(typeMask(u) & mask)) continue;
    double r = len(wdelta(u.pos, player->pos));
    if (r >= TGT_RANGE || !lineOfSight(u)) continue;
    c.push_back({r, &u});
  }
  std::sort(c.begin(), c.end(), [](auto& a, auto& b) { return a.first < b.first; });
  shootList.clear();
  for (size_t k = 0; k < c.size() && k < 4; k++) shootList.push_back(c[k].second);
  lockTgt = shootList.empty() ? nullptr : shootList[0];
}

// ---------------------------------------------------------------- waypoints and autopilot
const Waypoint* World::playerWaypoint() const {
  if (!player || player->route < 0 || player->route >= int(routes_.size()) || routes_[player->route].empty()) return nullptr;
  auto& R = routes_[player->route];
  return &R[std::clamp(playerWp, 0, int(R.size()) - 1)];
}
void World::nextWaypoint() {                              // 0x30ce0: steps cyclically
  if (!playerWaypoint()) return;
  int n = int(routes_[player->route].size());
  playerWp = (playerWp + 1) % n;
}
// Cannon aiming pipper with a target point (0x31bc0): the shell travels 4 wu (0x40000) per step; the flight time is searched by
// decreasing it from t0 = distance / 4 down to 12 steps, where the distance / 4 of the target's position t steps ahead comes closest to t
// (an exact match stops immediately); the target's motion = direction vector x speed. The original does not account for the shooter's own motion.
bool World::gunLead(D3& dir) const {
  if (!lockTgt || lockTgt->dead || !player) return false;
  const Unit& T = *lockTgt;
  D3 own = player->pos, disp = T.fwd() * T.speed;
  D3 d0 = wdelta(T.pos, own);
  const double S = 4.0;
  auto f = [&](double t) { D3 q = d0 + disp * t; return len(q) / S; };
  double t = std::floor(len(d0) / S), best = f(t), bestErr = std::fabs(best - t), bt = t;
  if (bestErr >= 1.0)
    for (int k = 1; k <= 12; k++) {
      double tk = t - k, fk = f(tk), e = std::fabs(fk - tk);
      if (e < bestErr) { bestErr = e; best = fk; bt = tk; }
      if (e < 1.0) break;
    }
  (void)bt;
  dir = d0 + disp * best;
  return len(dir) > 1e-9;
}

World::Ils World::ils() const {
  Ils r;
  const Unit& P = *player;
  if (P.onGround || crashed) return r;
  const Unit* best = nullptr; double bd = 1e9;
  for (const Unit* u : runways_) { double d = len(wdelta(u->pos, P.pos)); if (d < bd) { bd = d; best = u; } }
  if (!best || bd > 380 * 12) return r;
  D3 rf = dirOf(best->hdg, 0);                              // the runway heading
  D3 d = wdelta(P.pos, best->pos);                          // from the runway to the aircraft
  if (rf.x * f.x + rf.z * f.z < 0) rf = rf * -1;        // we approach the end the aircraft is facing
  double along = d.x * rf.x + d.z * rf.z, lat = d.x * rf.z - d.z * rf.x;   // along < 0: on the approach side
  if (along > -20) return r;
  double ang = std::atan2(lat, -along);
  double gsAng = std::atan2(P.pos.y - groundWU(best->pos.x, best->pos.z), -along);
  r.active = std::fabs(ang) < 35 * DEG;
  r.nm = -along / 380; r.loc = -ang / DEG; r.gs = (gsAng - 3 * DEG) / DEG; r.hdg = std::atan2(rf.x, rf.z) / DEG;
  if (r.hdg < 0) r.hdg += 360;
  return r;
}
void World::homeWaypoint() {                              // 0x30d0a + 0x276c5
  if (playerWaypoint()) playerWp = std::max(0, int(routes_[player->route].size()) - 2);
  toggleAutopilot(2);
}
double World::safeAlt() const {                           // 0x2d8ab: 12 samples, every 100 steps ahead
  double s = 0;
  D3 p = player->pos;
  for (int i = 0; i < 12; i++) {
    s = std::max(s, groundWU(p.x, p.z) + 400);
    p += v * 100.0;
  }
  return s;
}
int World::altHold(double alt) const {                    // 0x2d956: desired pitch = dy/4 (16.16 -> angle), max 67.5°
  double want = std::clamp((alt - player->pos.y) * 65536 / 4, -6291456.0, 6291456.0) * ANG;
  return int((want - player->pitch) / ANG / 65536);       // angle unit >> 16 (1 = 0.703°)
}
void World::autopilot(Controls& c) {
  Unit& P = *player;
  if (!apMode) return;
  if (P.onGround || std::abs(c.stickX) >= 64 || std::abs(c.stickY) >= 64 || std::abs(c.rudder) >= 64) { apMode = 0; return; }
  const double DEG = PI / 180;
  // 0x2d9bc: wings and nose level (pitch 11.4 / degree, max 128; roll max cap); climb above the terrain
  auto level = [&](int cap) {
    int y = std::clamp(int(-P.pitch / DEG * 11.38), -128, 128), x = std::clamp(int(-P.roll / DEG * 11.38), -cap, cap);
    c.stickY = y; c.stickX = x;
    if (std::abs(x | y) >> 7 == 0) {
      double s = safeAlt();
      if (s > P.pos.y) c.stickY = altHold(s);
    }
  };
  if (apMode == 1) level(64);
  else {                                                  // 0x2da36 + 0x2d99b
    thrCmd = 80;
    const Waypoint* wp = playerWaypoint();
    double err = 0;
    if (wp) { D3 d = wdelta(wp->pos, P.pos); err = wrapA(std::atan2(d.x, d.z) - P.hdg); }
    double bank = std::clamp(err * 16, -49.2 * DEG, 49.2 * DEG), bErr = bank - P.roll;
    // direct heading turn: at most 0x4000 angle units per step
    double a = std::clamp(err, -0x4000 * ANG, 0x4000 * ANG), ca = std::cos(a), sa = std::sin(a);
    auto ry = [&](D3& q) { q = {q.x * ca + q.z * sa, q.y, -q.x * sa + q.z * ca}; };
    ry(f); ry(up); ry(v);
    if (std::abs(bErr) < 0x80000 * ANG) level(16);
    else c.stickX = std::clamp(int(bErr / ANG / 65536), -64, 64);
    double alt = safeAlt();
    if (wp) alt = std::max(alt, wp->pos.y);
    c.stickY = altHold(alt);
  }
  c.stickX = std::clamp(c.stickX, -127, 127); c.stickY = std::clamp(c.stickY, -127, 127); c.rudder = 0;
}

// ---------------------------------------------------------------- escort commands
// The original swaps the escort's script (+0xEB): 0x3796c patrol, 0x37998 formation, 0x379ac attack
// (0x370fc: nearest enemy within 6080 wu), 0x379c4 the leader's target (0x37117), 0x379d0 cover
// (0x3713a / 0x30825: whatever threatens the leader). Radio (0x37169): text 0x36bc8, sound 0x36bf8 (0x11..0x15).
int World::wingmen() const {
  int n = 0;
  for (auto& u : units) if (!u.dead && u.leader == player && u.cls == C_AIR) n++;
  return n;
}
bool World::wingCommand(int cmd) {
  if (ai_) return ai_->wingCommand(*this, cmd);             // with the original scripts (--aivm)
  static const char* MSG[5] = {"WING - FOLLOWING PATROL ROUTE", "WING - ENGAGING ENEMY", "WING - RETURNING TO FORMATION",
                               "WING - USING YOUR TARGET FOR ATTACK", "WING - GOT YOUR BACK"};
  static const int SND[5] = {0x11, 0x12, 0x13, 0x14, 0x15};
  int msg = -1;
  Unit* first = nullptr;
  for (auto& u : units) {
    if (u.dead || u.leader != player || u.cls != C_AIR || u.onGround) continue;   // 0x27717: only in the air
    if (!first) first = &u;
    switch (cmd) {
      case 1: u.order = 1; u.target = nullptr; u.st = S_ROUTE; msg = 0; break;
      case 2: u.order = 2; u.target = nullptr; u.st = S_FORM; msg = 2; break;   // 0x37998: formation only, without target search
      case 3: {                                           // 0x370fc: nearest enemy within 6080 wu
        Unit* t;
        if (acquire(u, 6080, false, t)) { u.order = 3; u.target = t; u.st = S_PURSUE; u.timer = 1200; msg = 1; }
        else { u.order = 0; u.st = S_FORM; }
        break;
      }
      case 4:                                             // 0x27780: only if it has a target, and that is not the escort itself
        if (!lockTgt || lockTgt->dead || lockTgt == &u) return false;
        u.order = 4; u.target = lockTgt; u.st = S_PURSUE; u.timer = 1200; msg = 3;
        break;
      case 5: {                                           // 0x30825 (called by 0x37147): the nearest unit targeting the leader (measured from the leader)
        Unit* best = nullptr;
        double bd = 1e30;
        for (auto& t : units) {
          if (t.dead || &t == player || t.target != player) continue;
          double r = len(wdelta(t.pos, player->pos));
          if (r < bd) { bd = r; best = &t; }
        }
        if (best) { u.order = 5; u.target = best; u.st = S_PURSUE; u.timer = 1200; msg = 4; }
        else { u.order = 0; u.st = S_FORM; }
        break;
      }
    }
  }
  if (!first) return false;
  if (msg >= 0) {
    say(MSG[msg], 4);
    snd(SND[msg], player->pos, 0xC8, 1, false);
  }
  return true;
}

void World::stepLock() {                                  // 0x2a79b, every 2nd step
  if (stepNo & 1) return;
  const WeaponDef& w = wdef[weapon];
  if (lockTgt && lockTgt->dead) lockTgt = nullptr;
  if (!lockTgt) { lockState = 0; curAz = curEl = curX = curY = 0; return; }   // 0x2a7e7: no lock without a target (does not search for a new one)
  Unit& t = *lockTgt;
  bool air = t.cls == C_AIR && !t.onGround;
  bool ok = ammo[weapon] > 0 && weapon != 0;
  if (weapon == 2 && !radarOn) ok = false;                // AMRAAM: needs radar
  if (!((w.mask & 1) && air) && !((w.mask & 2) && !air)) ok = false;
  D3 d = wdelta(t.pos, player->pos);
  D3 right = cross(up, f);
  double bx = dot(d, right), by = dot(d, up), bz = dot(d, f);
  if (bz < 1.0 / 16 || len(d) > w.range) ok = false;
  if (std::fabs(bx) > bz / 2 || std::fabs(by) > bz / 2) ok = false;    // 0x2a6cc: square cone ~26.6°
  if (ok && !los(player->pos, t.pos)) ok = false;
  if (!ok) { lockState = 0; curAz = curEl = curX = curY = 0; return; }
  // 0x2a886: the cursor steps toward the target's body-frame (x, y) offset; the step is the x of the (0, 0, z)
  // vector rotated by 0xA000 angle units, i.e. z*sin(0.275°) - per update
  double st = bz * std::sin(0xA000 * ANG);
  curX += clampd(bx - curX, -st, st);
  curY += clampd(by - curY, -st, st);
  lockState = (std::fabs(bx - curX) <= st && std::fabs(by - curY) <= st) ? 2 : 1;
  curAz = std::atan2(curX, bz); curEl = std::atan2(curY, bz);
}

void World::fire(Unit* from, int wi, Unit* tgt, double hdg, double pitch, int32_t speed, bool dropPhase) {
  Missile m;
  m.w = wi; m.owner = from; m.target = tgt;
  m.pos = from->pos + D3{0, -0.5, 0};
  m.hdg = hdg; m.pitch = pitch; m.speed = speed; m.accel = 0; m.turn = 0;
  m.launchVel = from->isPlayer ? v : from->fwd() * from->speed;
  m.drop = dropPhase ? 20 : 0;                            // 0x3996e: 0.5 s free fall
  m.life = wdef[wi].life;
  m.armed = !(wi == 4);                                   // the SAM arms only at terminal speed
  m.born = now;
  firedBy[from->side & 15]++;
  if (from->target == player) musShotsAtPlayer += 4; else if (from->side & 8) musShotsFriendly += 4;   // 0x399a1 / 0x399b0
  if (tgt && tgt->isPlayer) say("MISSILE LAUNCH! (G: countermeasures)", 2.5f);   // 0x39250
  snd(SND_MISSILE, from->pos, 255, from->isPlayer ? 1.0 : 1.25);           // 0x39967 / 0x3a78d
  if (tgt && tgt->isPlayer) snd(SND_LAUNCH_WARN, from->pos, 0xEB, 1, false);   // 0x39b1f
  missiles.push_back(m);
}

void World::fireGun(Unit* from, D3 dir, D3 vel, int speedRaw, bool aaa) {
  Bullet b;
  b.pos = from->pos + dir * 2.0;
  b.vel = dir * (speedRaw / 65536.0) + vel;
  b.life = 380; b.owner = from; b.dmg = wdef[0].dmg; b.aaa = aaa;
  bullets.push_back(b);
  if (from->target == player) musShotsAtPlayer += 1; else if (from->side & 8) musShotsFriendly += 1;    // 0x3a1b2 / 0x3a1c1
  if (aaa || !from->isPlayer || (stepNo & 7) == 0) snd(aaa ? SND_AAA : SND_GUNS, from->pos, 255);   // 0x3a3f7 / 0x3a19b
}

void World::countermeasures() {                           // 0x276f7: one flare and one chaff
  if (flares > 0 || chaff > 0) snd(SND_FLARE, player->pos, 0xC8);   // APPROXIMATION: the call site is not known
  if (flares > 0) { flares--; decoys.push_back({player->pos, 200, 0, now}); }
  if (chaff > 0) { chaff--; decoys.push_back({player->pos + D3{0, -1, 0}, 200, 1, now}); }
}

void World::stepMissiles() {
  for (auto& m : missiles) {
    if (m.dead) continue;
    const WeaponDef& w = wdef[m.w];
    if (m.drop > 0) {                                     // separation
      m.drop--;
      m.pos += m.launchVel;
      m.pos.y -= 0x200 / 65536.0 * (20 - m.drop);
      wrapPos(m.pos);
      continue;
    }
    if (m.w == 3) {                                       // JDAM: ballistic, pursuit when guided
      m.launchVel.y -= 0x51 / 65536.0;
      if (m.target && !m.target->dead) {
        D3 d = wdelta(m.target->pos, m.pos);
        D3 hv{m.launchVel.x, 0, m.launchVel.z};
        double hs = len(hv) + 0x14 / 65536.0, want = std::atan2(d.x, d.z), cur = std::atan2(hv.x, hv.z);
        cur += clampd(wrapA(want - cur), -w.maxTurn * ANG, w.maxTurn * ANG);
        m.launchVel.x = std::sin(cur) * hs; m.launchVel.z = std::cos(cur) * hs;
      }
      m.pos += m.launchVel; wrapPos(m.pos);
      if (hitsGround(m.pos)) {                            // 0x39de5: for every ground target within 48 wu
        m.dead = true;
        blasts.push_back({m.pos, now, 4.0f});
        for (auto& u : units)
          if (!u.dead && u.cls != C_AIR && u.cls != C_RUNWAY && len(wdelta(u.pos, m.pos)) <= 48) {
            damage(u, w.dmg, u.pos, m.owner);
            if (u.dead && m.owner && m.owner->isPlayer) tally.jdamKills++;   // 0x39e49: +0xe
          }
      }
      continue;
    }
    if (--m.life <= 0) { m.dead = true; continue; }
    m.turn = std::min(m.turn + w.turnBuild, w.maxTurn);
    if (m.speed < w.vmax) { m.accel += w.accel; m.speed = std::min(m.speed + m.accel, w.vmax); }
    else {
      if (m.w == 4) m.armed = true;
      // IR seeker without a target: every 16 steps the nearest air unit within 480 wu (0x39652)
      if (w.seeker == 1 && !m.target && (stepNo & 15) == 0) {
        double best = 480;
        for (auto& u : units)
          if (!u.dead && &u != m.owner && u.cls == C_AIR && !u.onGround) {
            double r = len(wdelta(u.pos, m.pos));
            if (r < best) { best = r; m.target = &u; }
          }
      }
      if (m.target && !m.target->dead) {                  // 0x39460: pursuit with 2 steps of lead
        Unit& T = *m.target;
        D3 tv = T.isPlayer ? v : T.fwd() * T.speed;
        D3 d = wdelta(T.pos + tv * 2, m.pos);
        double ye = wrapA(std::atan2(d.x, d.z) - m.hdg);
        double pe = std::atan2(d.y, std::hypot(d.x, d.z)) - m.pitch;
        double gim = w.gimbal * ANG, tr = m.turn * ANG;
        if (std::fabs(ye) > gim || std::fabs(pe) > gim) m.target = nullptr;   // loses it for good
        else { m.hdg += clampd(ye, -tr, tr); m.pitch += clampd(pe, -tr, tr); }
      }
    }
    if (m.target) (w.seeker == 1 ? m.target->incomingIR : m.target->incomingRadar) = true;
    // countermeasures (0x395db): every 16 steps the farthest same-kind countermeasure around the target
    if (m.target && (m.life & 15) == 0 && w.seeker) {
      int kind = w.seeker == 1 ? 0 : 1;
      double gim = w.gimbal * ANG, best = -1;
      Decoy* pick = nullptr;
      for (auto& dc : decoys) {
        if (dc.kind != kind) continue;
        double r = len(wdelta(dc.pos, m.target->pos));
        if (r > 1140) continue;
        D3 dd = wdelta(dc.pos, m.pos);
        double ye = wrapA(std::atan2(dd.x, dd.z) - m.hdg), pe = std::atan2(dd.y, std::hypot(dd.x, dd.z)) - m.pitch;
        if (std::fabs(ye) > gim || std::fabs(pe) > gim) continue;
        if (r > best) { best = r; pick = &dc; }
      }
      if (pick) {
        D3 dd = wdelta(pick->pos, m.pos), dt = wdelta(m.target->pos, m.pos);
        double ya = std::fabs(wrapA(std::atan2(dd.x, dd.z) - std::atan2(dt.x, dt.z)));
        double pa = std::fabs(std::atan2(dd.y, std::hypot(dd.x, dd.z)) - std::atan2(dt.y, std::hypot(dt.x, dt.z)));
        double thr = m.target->side == 2 ? gim / 2 : gim;
        if (ya >= thr || pa >= thr) {
          if (m.target->isPlayer) say("The missile lost its target!", 2);
          m.target = nullptr;
        }
      }
    }
    m.pos += dirOf(m.hdg, m.pitch) * (m.speed / 65536.0);
    wrapPos(m.pos);
    if (hitsGround(m.pos)) { m.dead = true; blasts.push_back({m.pos, now, 2.0f}); continue; }
    if (m.armed) {                                        // 0x39754: axis-aligned box, for every unit
      for (auto& u : units) {
        if (u.dead || &u == m.owner || u.cls == C_RUNWAY) continue;
        D3 d = wdelta(u.pos, m.pos);
        double R = u.radius;
        if (std::fabs(d.x) <= R && std::fabs(d.y) <= R && std::fabs(d.z) <= R) {
          m.dead = true;
          blasts.push_back({m.pos, now, 2.5f});
          if (m.owner && m.owner->isPlayer && (u.side & 6)) tally.missilesHit++;   // 0x39793: +0xa
          damage(u, w.dmg, u.pos, m.owner);
          break;
        }
      }
    }
  }
  missiles.erase(std::remove_if(missiles.begin(), missiles.end(), [](auto& m) { return m.dead; }), missiles.end());
  for (auto& d : decoys) d.life--;
  decoys.erase(std::remove_if(decoys.begin(), decoys.end(), [](auto& d) { return d.life <= 0; }), decoys.end());
  blasts.erase(std::remove_if(blasts.begin(), blasts.end(), [&](auto& b) { return now - b.t0 > 1.1f; }), blasts.end());
}

void World::stepBullets() {                               // 0x3a118: moves twice per step, life -2
  for (auto& b : bullets) {
    for (int h = 0; h < 2 && b.life > 0; h++) {
      b.pos += b.vel * 0.5;
      wrapPos(b.pos);
      if (hitsGround(b.pos)) { b.life = 0; break; }   // 0x4101c
      for (auto& u : units) {
        if (u.dead || &u == b.owner || u.cls == C_RUNWAY) continue;
        if (b.aaa && u.cls != C_AIR) continue;            // 0x4103a
        D3 d = wdelta(u.pos, b.pos);
        if (std::fabs(d.x) <= u.radius && std::fabs(d.y) <= u.radius && std::fabs(d.z) <= u.radius) {
          if (b.owner && b.owner->isPlayer && (u.side & 6)) tally.bulletsHit++;    // 0x397a8: +6
          damage(u, b.dmg, u.pos, b.owner);
          b.life = 0;
          break;
        }
      }
    }
    b.life -= 2;
  }
  bullets.erase(std::remove_if(bullets.begin(), bullets.end(), [](auto& b) { return b.life <= 0; }), bullets.end());
}

// ------------------------------------------------------------------ ground units (0x26d32, 0x26d9a)
void World::stepGroundUnits() {
  for (auto& u : units) {
    if (u.dead || u.cls != C_GROUND || !u.active || ((stepNo + u.id) & 31)) continue;
    if (ai_ && ai_->drives(u)) continue;                  // controlled by the original script (--aivm)
    if (u.sub == 2) {                                     // radar: 3800 wu, stealth, line of sight
      Unit* best = nullptr;
      double br = 1e30;
      D3 eye = u.pos + D3{0, 1, 0};
      for (auto& t : units) {
        if (t.dead || t.cls != C_AIR || t.onGround || !hostile(u, t)) continue;
        double r = len(wdelta(t.pos, u.pos));
        if (r > 3800 * stealthOf(t) || r >= br || !los(eye, t.pos)) continue;
        br = r; best = &t;
      }
      if (!best) continue;
      D3 d = wdelta(best->pos, u.pos);
      double bearing = std::atan2(d.x, d.z);
      Unit* lnch = nullptr;
      double be = 1e30;
      for (Unit* l : u.launchers) {
        if (l->dead || stepNo - l->lastLaunch < 4800) continue;   // 120 s per launcher
        double e = std::fabs(wrapA(l->hdg - bearing));
        if (e < be) { be = e; lnch = l; }
      }
      if (lnch) {
        lnch->lastLaunch = stepNo;
        fire(lnch, 4, best, bearing, 0x5A0000 * ANG, 0, false);   // 0x39b95: 63.3°, speed 0
      }
    } else if (u.sub == 0) {                              // anti-aircraft gun: 1140 wu, lead
      Unit* best = nullptr;
      double br = 1140;
      for (auto& t : units) {
        if (t.dead || t.cls != C_AIR || t.onGround || !hostile(u, t)) continue;
        double r = len(wdelta(t.pos, u.pos));
        if (r < br) { br = r; best = &t; }
      }
      if (!best) continue;
      D3 tv = best->isPlayer ? v : best->fwd() * best->speed;
      D3 aim = wdelta(best->pos + tv * (br / 3), u.pos);
      double want = std::atan2(aim.x, aim.z);
      u.hdg += clampd(wrapA(want - u.hdg), -PI / 4, PI / 4);
      double el = std::atan2(aim.y, std::hypot(aim.x, aim.z));
      fireGun(&u, dirOf(u.hdg, el), {0, 0, 0}, 3 << 16, true);
    }
  }
}

// ------------------------------------------------------------------ AI aircraft
bool World::acquire(Unit& u, double range, bool stealth, Unit*& out) {
  out = nullptr;
  double br = 1e30;
  for (auto& t : units) {
    if (t.dead || &t == &u || !hostile(u, t)) continue;
    bool air = t.cls == C_AIR && !t.onGround;
    if (!((u.tmask & 1) && air) && !((u.tmask & 2) && (t.cls == C_GROUND || t.cls == C_STRUCT))) continue;
    double r = len(wdelta(t.pos, u.pos));
    double lim = range * (stealth ? stealthOf(t) : 1.0);
    if (r > lim || r >= br || !los(u.pos, t.pos)) continue;
    br = r; out = &t;
  }
  return out != nullptr;
}

void World::steer(Unit& u, D3 to, int32_t speedTarget, bool doSpeed) {   // 0x25b31 (fly_to_wp) steering
  const Perf& pf = perf[std::min<int>(u.sub, 9)];
  static const double SKILL[4] = {0.80, 0.85, 0.90, 1.0};  // 0x36c12
  double sk = SKILL[u.skill & 3], a = auth(int32_t(u.speed * 65536));
  D3 d = wdelta(to, u.pos);
  double err = wrapA(std::atan2(d.x, d.z) - u.hdg);
  double ymax = pf.yawR * ANG * sk * std::max(a, 0.15);
  double ys = clampd(err, -ymax, ymax);
  u.hdg = wrapA(u.hdg + ys);
  u.roll += (clampd(ys * 64, -1.4, 1.4) - u.roll) / 16;    // bank into the turn direction
  double want = clampd(std::atan2(d.y, std::hypot(d.x, d.z)), -1.0, 1.0);
  double pmax = pf.pitchR * ANG * sk * std::max(a, 0.15);
  u.pitch = wrapA(u.pitch + clampd(wrapA(want - u.pitch), -pmax, pmax));
  if (!doSpeed) return;
  double ts = speedTarget / 65536.0, acc = pf.accel / 65536.0;
  u.speed += clampd(ts - u.speed, -acc, acc);
  if (u.speed * 65536 > pf.bleed) u.speed -= (std::fabs(ys / ANG) / 1024) / 65536;   // speed loss in a turn
}

// ------------------------------------------------------------------ evasive maneuvers
// 0x258b1: picks the maneuver from the perf+0x38 bitmask (altitude band: <375, 375-1800, >=1800 wu
// AGL; skill), from a random starting bit to the first set bit. The 8 maneuvers come from the original
// bytecode (0x36c32 -> 0x377d4...): Z = level out (0x25d48), L/R = heading -/+90°, U/D = pitch
// +/-90°, V = roll +180°, P = pitch -45° (absolute), N = pitch and roll 0, heading +180°,
// E = cruise speed, then perf+0x30 + 0..63 steps straight, finally resuming the previous state.
void World::startEvade(Unit& u) {
  if (u.st == S_EVADE) return;
  const Perf& pf = perf[std::min<int>(u.sub, 9)];
  double agl = u.pos.y - groundWU(u.pos.x, u.pos.z);
  int band = agl > 375 && agl < 1800 ? 1 : agl >= 1800 ? 2 : 0;
  int mask = pf.evMask[band * 4 + (u.skill & 3)] & 0xFF;
  if (!mask) mask = 1;
  int k = rnd(8);
  while (!(mask & (1 << k))) k = (k + 1) & 7;
  int r = rnd(2);
  static const char* M[8][2] = {{"LL", "RR"}, {"UUVN", "UUVN"}, {"VPLL", "VPRR"}, {"UVDDVU", "UVDDVU"},
                                {"VDDN", "VDDN"}, {"UUUU", "UUUU"}, {"RLLR", "LRRL"}, {"RRUUVN", "LLUUVN"}};
  u.saved = (u.st == S_PULLUP || u.st == S_EXTEND) ? S_ROUTE : u.st;
  u.st = S_EVADE;
  u.mv = std::string("Z") + M[k][r] + "ZE";
  u.mvI = 0; u.mvWait = 0;
  u.tgtH = u.tgtP = u.tgtR = NAN;
  evadeCount[k]++;
}

bool World::stepManeuver(Unit& u) {
  const Perf& pf = perf[std::min<int>(u.sub, 9)];
  static const double SKILL[4] = {0.80, 0.85, 0.90, 1.0}, SKSPD[4] = {0.75, 0.8, 0.9, 1.0};
  double sk = SKILL[u.skill & 3], a = std::max(auth(int32_t(u.speed * 65536)), 0.15);
  double pr = pf.pitchR * ANG * sk, yr = pf.yawR * ANG * sk, rr = pf.rollR * ANG * sk;
  auto bleed = [&](double d) { if (u.speed * 65536 > pf.bleed) u.speed -= (std::fabs(d / ANG) / 1024) / 65536; };   // 0x2611d
  auto toward = [](double& v, double t, double m) {
    double e = wrapA(t - v);
    if (std::fabs(e) <= m) { v = t; return true; }
    v = wrapA(v + (e > 0 ? m : -m));
    return false;
  };
  if (u.mvI >= int(u.mv.size())) { u.st = u.saved; return true; }
  char op = u.mv[u.mvI];
  bool done = false;
  switch (op) {
    case 'Z': {                                            // 0x25d48
      bool a1 = toward(u.pitch, 0, pr), a2 = toward(u.roll, 0, rr);
      done = a1 && a2;
      break;
    }
    case 'N': u.pitch = 0; u.roll = 0; u.hdg = wrapA(u.hdg + PI); done = true; break;   // 0x377ac
    case 'E': {                                            // 0x376b8
      if (u.mvWait == 0) {
        double ts = pf.cruise * SKSPD[u.skill & 3] / 65536.0, acc = pf.accel / 65536.0;
        u.speed += clampd(ts - u.speed, -acc, acc);
        if (std::fabs(ts - u.speed) < 1e-9) u.mvWait = 1 + pf.extend + rnd(64);
      } else if (--u.mvWait <= 1) done = true;
      break;
    }
    default: {                                             // target-angle step (0x26142)
      if (u.mvWait == 0) {
        u.mvWait = 1;
        if (op == 'U') u.tgtP = wrapA(u.pitch + PI / 2);
        if (op == 'D') u.tgtP = wrapA(u.pitch - PI / 2);
        if (op == 'P') u.tgtP = -PI / 4;
        if (op == 'R') u.tgtH = wrapA(u.hdg + PI / 2);
        if (op == 'L') u.tgtH = wrapA(u.hdg - PI / 2);
        if (op == 'V') u.tgtR = wrapA(u.roll + PI);
      }
      if (!std::isnan(u.tgtP)) { double p0 = u.pitch; toward(u.pitch, u.tgtP, pr * a); bleed(2 * wrapA(u.pitch - p0)); }
      if (!std::isnan(u.tgtH)) {
        double h0 = u.hdg;
        toward(u.hdg, u.tgtH, yr * a);
        double dh = wrapA(u.hdg - h0);
        bleed(dh);
        u.roll += (clampd(dh * 64, -1.4, 1.4) - u.roll) / 16;   // banking (as in steer)
      } else if (!std::isnan(u.tgtR)) {
        toward(u.roll, u.tgtR, rr);
      }
      if (!std::isnan(u.tgtP)) { if (u.pitch != u.tgtP) break; u.tgtP = NAN; }
      if (!std::isnan(u.tgtH)) { if (u.hdg == u.tgtH) { u.tgtH = NAN; u.tgtR = 0; } break; }
      if (!std::isnan(u.tgtR)) { if (u.roll != u.tgtR) break; u.tgtR = NAN; }
      done = true;
      break;
    }
  }
  if (done) { u.mvI++; u.mvWait = 0; }
  return false;
}

void World::stepAI(Unit& u) {
  if (u.dead || u.cls != C_AIR || u.isPlayer) return;
  const Perf& pf = perf[std::min<int>(u.sub, 9)];
  static const double SKSPD[4] = {0.75, 0.8, 0.9, 1.0}, SKEXT[4] = {1, 0.82, 0.66, 0.5};
  double gh = groundWU(u.pos.x, u.pos.z), agl = u.pos.y - gh;
  int32_t cruise = pf.cruise;
  auto flyTo = [&](D3 p, int32_t spd) { steer(u, p, spd); };
  // terrain following: every 4 steps, 380 wu ahead (0x25b31)
  if (!u.onGround && u.st != S_PULLUP && u.st != S_EVADE && (stepNo & 3) == 0) {   // not during a maneuver (0x26142)
    D3 ahead = u.pos + u.fwd() * 380;
    if (ahead.y < groundWU(ahead.x, ahead.z) + 10) { u.timer = u.st; u.st = S_PULLUP; }
  }
  // evasion: attacker within 1520 wu in the rear ±45° (0x258b1), every 64 steps
  if (!u.onGround && u.st != S_EVADE && ((stepNo + u.id) & 63) == 0) {
    for (auto& a : units) {
      if (a.dead || a.cls != C_AIR || a.onGround || !hostile(u, a)) continue;
      D3 d = wdelta(a.pos, u.pos);
      if (len(d) > 1520) continue;
      double rel = std::fabs(wrapA(std::atan2(d.x, d.z) - u.hdg));
      if (rel > PI * 0.75 && std::fabs(wrapA(a.hdg - u.hdg)) < 33.75 * DEG) {
        startEvade(u);
        break;
      }
    }
  }
  // missile reaction (0x257e9): skill >= 2, every 128 steps countermeasure + evasion
  if ((u.incomingIR || u.incomingRadar) && u.skill >= 2 && stepNo - u.flareCd >= 128) {
    u.flareCd = stepNo;
    decoys.push_back({u.pos, 200, u.incomingIR ? 0 : 1, now});
    startEvade(u);
  }
  u.incomingIR = u.incomingRadar = false;
  const WeaponDef& w = wdef[std::min(pf.weapon, 5)];

  switch (u.st) {
    case S_PARKED:
      if (waitsForPlayer(u) && u.active && u.activeFlag && player->wasAirborne && u.taxi >= 0 && u.taxi < int(taxis_.size())) {
        u.st = S_TAXI; u.node = nearestTaxiNode(u);
      }
      return;
    case S_TAXI: {                                        // taxiing: 12 m/s, 4 wu nodes
      if (u.taxi < 0 || u.taxi >= int(taxis_.size()) || u.node >= int(taxis_[u.taxi].size())) { u.st = S_TAKEOFF; break; }
      const TaxiNode& n = taxis_[u.taxi][u.node];
      D3 to{n.x, u.pos.y, n.z};
      D3 d = wdelta(to, u.pos);
      u.hdg = wrapA(u.hdg + clampd(wrapA(std::atan2(d.x, d.z) - u.hdg), -27 * DEG / 40, 27 * DEG / 40));
      u.speed = 12 / WU_M / 40;
      if (len(D3{d.x, 0, d.z}) < 4) {
        if (n.action == 1) { u.hdg = (int64_t(n.param) << 16) * ANG; u.st = S_TAKEOFF; }
        else if (n.action == 2)                           // the group's next aircraft may start
          for (auto& o : units) if (o.cls == C_AIR && o.side == u.side && o.group == u.group && o.st == S_PARKED && o.activeFlag) {
            o.st = S_TAXI; break;
          }
        u.node++;
      }
      break;
    }
    case S_TAKEOFF: {                                     // takeoff roll, rotation at the perf+4 speed, 9.8°
      u.speed += pf.accel / 65536.0 / 2;
      if (u.speed * 65536 >= pf.vrot) { u.pitch = 9.8 * DEG; u.onGround = false; }
      if (!u.onGround && agl > 50) { u.wasAirborne = true; u.st = u.leader ? S_FORM : S_ROUTE; }
      break;
    }
    case S_ROUTE: {
      if (u.order == 1 && (!u.leader || u.leader->dead)) u.order = 0;
      if (u.order != 1 && u.leader && !u.leader->dead && u.leader->st != S_PURSUE && u.leader->st != S_TRACK) { u.order = 0; u.st = S_FORM; break; }
      if (((stepNo + u.id) & 63) == 0) {                  // target search 3040 / 5700 wu (escort)
        Unit* t;
        if (acquire(u, u.escort ? 5700 : 3040, true, t)) { u.target = t; u.st = S_PURSUE; u.timer = 1200; break; }
      }
      if (u.route >= 0 && u.route < int(routes_.size()) && !routes_[u.route].empty()) {
        auto& R = routes_[u.route];
        if (u.wp >= int(R.size())) u.wp = 0;
        const Waypoint& wp = R[u.wp];
        flyTo(wp.pos, cruise);
        D3 d = wdelta(wp.pos, u.pos);
        if (std::fabs(d.x) <= 200 && std::fabs(d.y) <= 200 && std::fabs(d.z) <= 200) {   // arrival
          switch (wp.action) {
            case 1: u.wp = wp.param; break;
            case 2: case 4: {
              Unit* t;
              if (acquire(u, wp.action == 2 ? 11400 : 5700, false, t)) { u.target = t; u.st = S_PURSUE; u.timer = 1200; }
              u.wp++;
              break;
            }
            case 3:
              for (auto& t : units) if (t.id == wp.param && !t.dead && hostile(u, t)) { u.target = &t; u.st = S_PURSUE; u.timer = 1200; }
              u.wp++;
              break;
            case 5: if (wp.param < radio.size()) say(radio[wp.param].text, 5); u.wp++; break;
            case 6: u.reached = true; u.wp++; break;
            default: u.wp++; break;
          }
        }
      } else {
        flyTo(u.pos + dirOf(u.hdg, 0) * 1000 + D3{0, 0, 0}, cruise);
      }
      break;
    }
    case S_FORM: {                                        // formation flying (0x25e4b)
      Unit* L = u.leader;
      if (!L || L->dead) { u.leader = nullptr; u.st = S_ROUTE; break; }
      if ((L->st == S_PURSUE || L->st == S_TRACK || L->isPlayer) && u.order != 2) {
        Unit* t;
        if (((stepNo + u.id) & 63) == 0 && acquire(u, 5700, true, t)) { u.target = t; u.st = S_PURSUE; u.timer = 1200; break; }
      }
      // offset: 0x255fc[formation] + (slot-1)*0x14, 16.16 wu (x right, y up, z forward),
      // rotated with the leader's full orientation matrix (0x267d7, 0x25e7b)
      D3 off{20.0 * ((u.slot & 1) ? 1 : -1), 0, -16};
      if (u.slot >= 1 && u.formation < 8) {
        uint32_t p = uint32_t(exe_->i32(0x255fc + u.formation * 4)) + (u.slot - 1) * 0x14;
        if (p > 0x20000 && p + 12 < exe_->d.size())
          off = {exe_->i32(p) / 65536.0, exe_->i32(p + 4) / 65536.0, exe_->i32(p + 8) / 65536.0};
      }
      D3 lf, lu, lr;
      basis(L->hdg, L->pitch, L->roll, lf, lu, lr);
      D3 slotP = L->pos + lr * off.x + lu * off.y + lf * off.z;
      D3 d = wdelta(slotP, u.pos);
      u.slotErr = len(d);
      double vmaxS = pf.vmax / 65536.0, ls = L->speed, adj;
      if (std::fabs(d.x) <= 10 && std::fabs(d.y) <= 10 && std::fabs(d.z) <= 10) {   // 0x259d4: in position
        u.roll -= u.roll / 16;                            // 0x25d2d: wings level
        adj = (ls - u.speed) / 4;
      } else {
        double eh = std::fabs(wrapA(std::atan2(d.x, d.z) - u.hdg));
        double ep = std::fabs(wrapA(std::atan2(d.y, std::hypot(d.x, d.z)) - u.pitch));
        steer(u, slotP, 0, false);                        // 0x25a36
        if (eh < 0x40000 * ANG || ep < 0x40000 * ANG)     // faces the slot: distance-dependent catch-up
          adj = (ls - 0x500 / 65536.0 + len(wdelta(L->pos, u.pos)) / 1024 - u.speed) / 128;
        else adj = (ls - u.speed) / 4;
      }
      u.speed = std::min(u.speed + clampd(adj, -vmaxS, vmaxS), vmaxS);
      break;
    }
    case S_PURSUE: {                                      // 0x25dab: pure pursuit
      if (!u.target || u.target->dead || --u.timer <= 0) { u.target = nullptr; u.st = S_ROUTE; break; }
      Unit& T = *u.target;
      double r = len(wdelta(T.pos, u.pos));
      int32_t spd = r < 1520 ? int32_t(std::max(T.speed * 65536, double(cruise) * 0.8)) : pf.vmax;
      flyTo(T.pos, spd);
      if (r <= w.range) { u.st = S_TRACK; u.timer = 400; u.trackT = 0; }
      break;
    }
    case S_TRACK: {                                       // 0x26028: fires in a ±22.5° cone
      if (!u.target || u.target->dead || --u.timer <= 0) { u.target = nullptr; u.st = S_ROUTE; break; }
      Unit& T = *u.target;
      flyTo(T.pos, cruise);
      D3 d = wdelta(T.pos, u.pos);
      double r = len(d);
      double az = std::fabs(wrapA(std::atan2(d.x, d.z) - u.hdg)), el = std::fabs(std::atan2(d.y, std::hypot(d.x, d.z)) - u.pitch);
      if (r > w.range * 1.2) { u.st = S_PURSUE; u.timer = 1200; break; }
      if (az <= 22.5 * DEG && el <= 22.5 * DEG && r <= w.range && --u.fireCd <= 0) {
        if (pf.weapon == 3) { fire(&u, 3, &T, u.hdg, u.pitch, int32_t(u.speed * 65536), true); u.fireCd = 200; }
        else if (r <= 380) { fireGun(&u, dirOf(u.hdg, u.pitch), u.fwd() * u.speed, wdef[0].vmax * 2, false); u.fireCd = 10; }
        else {
          fire(&u, pf.weapon, &T, u.hdg, u.pitch, int32_t(u.speed * 65536), true);
          u.st = S_EXTEND; u.timer = 0;
          u.extendLeft = int(pf.extend * SKEXT[u.skill & 3]) + rnd(64);
        }
      }
      break;
    }
    case S_EXTEND: {                                      // climb at 33.75° up to 200 wu AGL, then straight
      D3 to = u.pos + dirOf(u.hdg, agl < 200 ? 33.75 * DEG : 0) * 500;
      flyTo(to, int32_t(cruise * SKSPD[u.skill & 3]));
      if (agl >= 200 && --u.extendLeft <= 0) {
        u.hdg = wrapA(u.hdg + (rnd(91) - 45) * DEG);
        u.st = S_ROUTE;
      }
      break;
    }
    case S_EVADE: stepManeuver(u); break;
    case S_PULLUP: {                                      // 28.1° climb up to 80 wu AGL
      u.pitch += clampd(28.1 * DEG - u.pitch, -0.05, 0.05);
      u.speed = std::max(u.speed, cruise / 65536.0 * 0.7);
      if (agl >= 80) u.st = AiState(u.timer > S_PULLUP ? S_ROUTE : u.timer);
      break;
    }
  }
  // movement (0x40cfa)
  u.pos += dirOf(u.hdg, u.onGround ? 0 : u.pitch) * u.speed;
  wrapPos(u.pos);
  gh = groundInterpWU(u.pos.x, u.pos.z);
  if (u.onGround) u.pos.y = gh;
  else if (hitsGround(u.pos, 1)) {                         // 0x269d6: both the corner maximum and the interpolated ground
    if (u.st == S_TAKEOFF) u.pos.y = gh + 1;
    else { kill(u, u.pos); terrainKills++; }             // hit the terrain
  }
  if (!u.onGround && !u.wasAirborne && u.pos.y - gh > 20) u.wasAirborne = true;
}

// ------------------------------------------------------------------ triggerek (0x23cc0 / 0x23910)
void World::activateGroup(int group, int side) {
  for (auto& u : units)
    if (u.cls == C_AIR && u.group == group && (side == 0 || u.side == side) && !u.isPlayer && !u.dead) {
      u.dormant = 0; u.active = true;
      if (u.st == S_PARKED && u.activeFlag) u.st = (!u.leader && u.taxi < int(taxis_.size())) ? S_TAXI : (u.onGround ? S_PARKED : S_ROUTE);
      if (u.leader && !waitsForPlayer(u) && u.st == S_PARKED) { u.st = S_TAXI; u.node = nearestTaxiNode(u); }
      if (ai_) ai_->onActivate(*this, u);
    }
}

void World::runTriggers() {
  for (auto& t : triggers_) {
    if (t.fired) continue;
    bool ok = false;
    // the subject's units
    std::vector<Unit*> subj;
    for (auto& u : units) {
      if (t.kind == 0 && u.cls == C_AIR && u.group == t.subject && !u.isPlayer) subj.push_back(&u);
      else if (t.kind == 1 && u.id == t.subject) subj.push_back(&u);
      else if (t.kind == 2 && u.cls != C_AIR && u.group == t.subject) subj.push_back(&u);
    }
    if (t.kind == 3) ok = t.cond >= 0 && t.cond < 4 && globalEv_[t.cond];   // global: 0 start (0x54e3a), 1 goal waypoint (0x2b85c)
    else if (!subj.empty()) {
      int dead = 0, air = 0, reach = 0, hit = 0;
      for (Unit* u : subj) { dead += u->dead; air += u->wasAirborne; reach += u->reached; hit += u->hp < u->maxhp; }
      double frac = double(dead) / subj.size();
      switch (t.cond) {
        case 0: ok = t.resp == 3 && t.b > 0 && t.b < 100 ? frac * 100 >= t.b : dead == int(subj.size()); break;
        case 1: ok = air > 0; break;
        case 2: ok = reach > 0; break;
        case 3: ok = hit > 0; break;                      // "detected": the original sends it on a hit (0x26478, 0x4105b)
        default: break;
      }
    }
    if (!ok) continue;
    t.fired = true;
    respond(t);
  }
}

// the trigger's response (table 0x23910: 0 / 1 / 4 activation, 2 radio, 3 goal)
void World::respond(Trigger& t) {
  switch (t.resp) {
    case 0: case 1: case 4:
      if (t.a == 0) activateGroup(t.b, 0);
      else if (t.a == 1) { for (auto& u : units) if (u.id == t.b) { u.dormant = 0; u.active = true; if (ai_) ai_->onActivate(*this, u); } }
      else if (t.a == 2) { for (auto& u : units) if (u.id == t.b || (u.cls != C_AIR && u.group == t.b)) u.active = false; }
      break;
    case 2:
      if (t.b < int(radio.size())) {
        bool alive = false;
        for (auto& u : units) if (u.id == t.a && !u.dead) alive = true;
        if (alive || t.a == 0) {
          say(radio[t.b].text, 6);
          if (!radio[t.b].wav.empty()) snd(SND_RADIO, player->pos, 0xFF, 1, false, radio[t.b].wav);   // 0x23a84
        }
      }
      break;
    case 3: {
      goalDone[std::min(t.a, 3)]++;
      static const char* C[] = {"PRIMARY OBJECTIVE COMPLETED", "SECONDARY OBJECTIVE COMPLETED", "BONUS OBJECTIVE COMPLETED", "MISSION FAILED"};
      say(C[std::min(t.a, 3)], 6);
      break;
    }
  }
}

// 0x23cc0: the original event dispatcher - (type, condition, subject, parameter). The response of a matching, not yet fulfilled trigger runs immediately; the goal (3) response
// according to the +6 word: if 0, only parameter = 0 (complete), otherwise parameter >= +6 (in quarters); the other responses only for parameter = 0 (0x23b6c ...).
void World::triggerEvent(int kind, int cond, int subject, int param) {
  evSeen++;
  for (auto& t : triggers_) {
    if (t.fired || t.kind != kind || t.cond != cond || t.subject != subject) continue;
    bool ok = t.resp == 3 ? (t.b ? t.b <= param : param == 0) : param == 0;
    if (!ok) continue;
    t.fired = true;
    evFired++;
    respond(t);
  }
}

// ------------------------------------------------------------------ step
void World::step(const Controls& cIn) {
  stepNo++;
  Controls c = cIn;
  if (ejected) c = Controls{};                            // after ejection nobody flies the aircraft
  if (!crashed && !ejected) autopilot(c);
  if (chute.on) {                                          // the pilot: free fall, then slow descent with an open parachute
    double gh = groundWU(chute.pos.x, chute.pos.z);
    if (chute.pos.y > gh) {
      chute.vel.y -= 81.0 / 65536;                         // gravity (as for the aircraft)
      if (now - chute.t0 > 2.0f) {                         // the parachute opens after 2 s (APPROXIMATION: the original's 0xe4dd timing is not known)
        chute.vel.x *= 0.96; chute.vel.z *= 0.96;
        chute.vel.y += (-0.026 - chute.vel.y) * 0.1;      // about 5 m/s descent (APPROXIMATION)
      }
      chute.pos += chute.vel;
      wrapPos(chute.pos);
      if (chute.pos.y < gh) { chute.pos.y = gh; chute.vel = {0, 0, 0}; }
    }
  }
  stickX = c.stickX; stickY = c.stickY;
  if (const Waypoint* wp = playerWaypoint()) {            // 0x2b7c9: horizontally within 152 wu (half-distance 0x4c)
    D3 d = wdelta(wp->pos, player->pos);
    if (!player->onGround && std::hypot(d.x, d.z) <= 152) {
      if (wp->action == 6) player->reached = true;                        // "reached the goal" (unit, group: 1/0, condition 2)
      else if (wp->action == 9 && wp->param == 1) globalEv_[1] = true;    // global 1: the goal waypoint reached (3, 1)
      nextWaypoint();                                                      // 0x30ce0: steps cyclically
    }
  }
  now = float(stepNo * STEP_S);

  if (!crashed && !ejected) {
    stepPlayer(c);
    // fegyverek
    refire++;
    if (weapon == 0) {
      if (c.fire && ammo[0] > 0 && !player->onGround) { ammo[0]--; tally.bulletsFired++; fireGun(player, f, v, wdef[0].vmax * 2, false); }   // 0x3a324: +4
    } else {
      if (c.fire && bayTimer < 0 && refire >= 20 && ammo[weapon] > 0 && !player->onGround) {
        bayTimer = 0; bayLatch = lockState == 2 ? 1 : 0;  // 0x3a4d3: the lock is latched at trigger pull
        if (bayPos == 0) snd(SND_BAY, player->pos, 0x64);    // 0x3a511
      }
      if (bayTimer >= 0) {
        if (bayPos < 32) bayPos++;
        else {                                             // 0x3a49f: launch, closing the bay door
          fire(player, weapon, bayLatch ? lockTgt : nullptr, player->hdg, player->pitch, int32_t(len(v) * 65536), true);
          (weapon == 3 ? tally.jdamFired : tally.missilesFired)++;   // 0x3a090: +0xc JDAM, 0x39ddc: +8 missile
          ammo[weapon]--; refire = 0; bayTimer = -1;
          if (ammo[weapon] == 0) cycleWeapon();
        }
      } else if (bayPos > 0) bayPos--;
    }
    stepLock();
    // automatic countermeasure release (0x2b868): every 64 steps, if a missile approaches within 760 wu
    if ((stepNo & 63) == 0) {
      double best = 760;
      int kind = -1;
      for (auto& m : missiles)
        if (m.target == player) {
          double r = len(wdelta(m.pos, player->pos));
          if (r < best) { best = r; kind = wdef[m.w].seeker == 1 ? 0 : 1; }
        }
      if (kind == 0 && flares > 0) { flares--; decoys.push_back({player->pos, 200, 0, now}); }
      if (kind == 1 && chaff > 0) { chaff--; decoys.push_back({player->pos, 200, 1, now}); }
    }
  }
  player->incomingIR = player->incomingRadar = false;
  if (ai_) ai_->step(*this);
  for (auto& u : units) if (!ai_ || !ai_->drives(u)) stepAI(u);
  stepGroundUnits();
  stepMissiles();
  stepBullets();
  if ((stepNo & 7) == 0) runTriggers();
  msgs.erase(std::remove_if(msgs.begin(), msgs.end(), [&](auto& m) { return m.until < now; }), msgs.end());
}

}  // namespace sim
