// Game simulation following the rules of the original F22.EXE (reverse-engineered, see native/README.md).
// All values are in the game's own units: 1 world unit (wu) = 16 ft = 4.8768 m,
// simulation step = 1/40 s, speed in wu/step. The world wraps around every 65536 wu.
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "res.h"

namespace sim {

constexpr double WU_M = 4.8768;          // meters / world unit
constexpr double WORLD = 65536.0;        // world size in wu (wraps around)
constexpr double STEP_S = 1.0 / 40;      // simulation step (s)
constexpr double PI = 3.14159265358979323846;
constexpr double ANG = 2 * PI / 33554432.0;   // original angle unit (2^25 = 360°) -> radians

struct D3 {
  double x = 0, y = 0, z = 0;
  D3() = default;
  D3(double a, double b, double c) : x(a), y(b), z(c) {}
  D3 operator+(D3 o) const { return {x + o.x, y + o.y, z + o.z}; }
  D3 operator-(D3 o) const { return {x - o.x, y - o.y, z - o.z}; }
  D3 operator*(double s) const { return {x * s, y * s, z * s}; }
  D3& operator+=(D3 o) { x += o.x; y += o.y; z += o.z; return *this; }
};
double dot(D3 a, D3 b);
D3 cross(D3 a, D3 b);
double len(D3 a);
D3 norm(D3 a);
double wrapd(double d);            // wrap-around difference [-32768, 32768)
D3 wdelta(D3 a, D3 b);             // a - b with wrapping x/z
D3 dirOf(double hdg, double pitch);   // direction: hdg from +Z toward +X, pitch positive upward
// body axes from Euler angles (continuous for any pitch); right = cross(up, f) = original +x
void basis(double hdg, double pitch, double roll, D3& f, D3& up, D3& right);

// Reverse-engineered view of the original program (tables, constants)
struct Exe {
  std::vector<uint8_t> d;
  bool load(const std::string& path);
  int32_t i32(uint32_t a) const;
  uint16_t u16(uint32_t a) const;
  uint8_t u8(uint32_t a) const { return d[a]; }
};

struct WeaponDef {                 // table: 0x39034, 6 x 0x40 bytes
  std::string name;
  int dmg = 0, life = 0, mask = 0, seeker = 0;   // seeker: 1 IR, 2 radar
  double range = 0;                // wu
  int32_t maxTurn = 0, turnBuild = 0, gimbal = 0;   // original angle unit / step
  int32_t accel = 0, vmax = 0;     // 16.16 wu/step
};
struct Perf {                      // table: 0x24618, 10 x 0x50 bytes
  int32_t vmax, vrot, cruise, yawR, pitchR, rollR, accel, bleed;
  int hp, weapon, extend;
  uint16_t evMask[12];             // +0x38: bitmask of evasive maneuvers [altitude band*4 + skill]
};

enum Cls { C_AIR = 0, C_GROUND = 1, C_STRUCT = 2, C_RUNWAY = 3 };
enum AiState { S_PARKED, S_TAXI, S_TAKEOFF, S_ROUTE, S_FORM, S_PURSUE, S_TRACK, S_EXTEND, S_EVADE, S_PULLUP };

struct Unit {
  int id = 0;                      // record index (+0x27)
  std::string model;
  int cls = 0, sub = 0, side = 0, group = 0, wing = 0, taxi = -1, route = -1, skill = 0;
  int dormant = 0, airStart = 0, noAuto = 0, tmask = 1, escort = 0, formation = 0, slot = 0, activeFlag = 0;
  D3 pos;                          // wu; y = altitude
  double hdg = 0, pitch = 0, roll = 0;   // radians
  double speed = 0;                // wu/step
  int hp = 100, maxhp = 100;
  bool dead = false, onGround = true, isPlayer = false, active = false;
  double radius = 2;               // hit radius (wu), from the model
  // MI
  AiState st = S_PARKED;
  int timer = 0, wp = 0, node = 0, fireCd = 0, evadeCd = 0, flareCd = 0, extendLeft = 0, trackT = 0;
  // evasive maneuver (0x258b1, bytecode: table 0x36c32) - operation sequence and target angles
  std::string mv;                  // operations, see sim.cpp: stepManeuver
  int mvI = 0, mvWait = 0;
  double tgtH = NAN, tgtP = NAN, tgtR = NAN;   // target heading / pitch / roll (NAN = none)
  int order = 0;                   // escort command (0x27702-0x27780): 0 default, 1 patrol route, 2 formation, 3 attack, 4 the leader's target, 5 cover
  int mpMode = 0;                  // ORF +0x20: nature of the multiplayer template (Create Mission)
  AiState saved = S_ROUTE;         // state to resume (+0xFA)
  double slotErr = 0;              // formation: distance from the assigned slot (wu, statistics)
  Unit* target = nullptr;
  Unit* leader = nullptr;
  int lastLaunch = -100000;        // SAM launcher: step of the last launch
  std::vector<Unit*> launchers;    // radar: linked launchers
  bool incomingIR = false, incomingRadar = false, reached = false, wasAirborne = false;
  D3 fwd() const { return dirOf(hdg, pitch); }
};

struct Missile {
  int w = 0;
  Unit* owner = nullptr;
  Unit* target = nullptr;
  D3 pos, launchVel;
  double hdg = 0, pitch = 0;
  int32_t speed = 0, accel = 0, turn = 0;   // 16.16 / angle unit
  int life = 0, drop = 0;
  bool armed = false, dead = false;
  float born = 0;
};
struct Bullet { D3 pos, vel; int life; Unit* owner; int dmg; bool aaa; };
struct Decoy { D3 pos; int life; int kind; float born; };   // kind: 0 flare (IR), 1 chaff (radar)
struct Blast { D3 pos; float t0; float scale; };
struct Waypoint { D3 pos; int action, param; };
struct TaxiNode { double x, z; int action, param; };
struct Trigger { int kind, cond, subject, resp, a, b; bool fired = false; };
struct Radio { std::string text, wav; };
// sound events with the original identifiers (table 0x65c60 + 4*id)
enum SndId { SND_FLYBY = 1, SND_EXPLODE = 2, SND_YEEHAW = 4, SND_MISSILE = 6, SND_ENGINE = 7, SND_GEAR_WARN = 8, SND_GEAR = 9,
             SND_SCREECH = 10, SND_GUNS = 11, SND_BEEP = 14, SND_TONE = 15, SND_LAUNCH_WARN = 16, SND_AAA = 22, SND_FLARE = 23,
             SND_BRAKE = 24, SND_FLAPS = 25, SND_BAY = 26, SND_RADIO = 38 };
struct Sfx { int id; D3 pos; int vol; double pitch; bool positional; std::string wav; };

struct Controls {                  // the player's input for one step (original ranges)
  int stickX = 0, stickY = 0, rudder = 0;   // -127..127; stickY > 0 = pull
  bool fire = false, brake = false;
};

// "Create Mission" (enemy.pcx / friendly.pcx): settings of a mission assembled from the QMC0x template
struct CreateSpec {
  struct Wing { int type = 0, skill = 1, num = 2; };   // type: 0 none; enemy 1..4 = EF2000, MIG-27, MIG-29, SU-27; ally 1..3 = F-14, F-15, F-16
  Wing enemy[4], friendly[2];
  int ground = 0;                                      // 0 none, 1 chemical plant, 2 nuclear power plant
  int samSkill = 1, samNum = 0, aaaSkill = 1, aaaNum = 0;
  int region = 0, tod = 1;                             // terrain (island, snow, desert, jungle), time of day 1..5
  int aim9 = 2, amraam = 4, jdam = 0;
  bool valid() const {                                 // there is something to destroy
    for (auto& w : enemy) if (w.type) return true;
    return ground || samNum || aaaNum;
  }
};

class AiBridge;
D3 dirOf(double h, double p);

class World {
  friend class AiBridge;
 public:
  // the original AI scripts (aibridge.h): experimental, behind a switch
  void enableAiVm(const std::vector<uint8_t>* exeImage);
  bool aiVm() const { return bool(ai_); }
  void aiReseed(Unit& u);
  int nearestTaxiNode(const Unit& u) const;   // the nearest point of the taxi route (an aircraft already on the runway should not taxi back to the start)
  bool waitsForPlayer(const Unit& u) const { return u.leader && u.leader->isPlayer; }
  void triggerEvent(int kind, int cond, int subject, int param = 0);
  int evSeen = 0, evFired = 0;      // statistics: the VM's events and the triggers they satisfied   // 0x23cc0: event dispatcher (from the VM)
  std::shared_ptr<AiBridge> ai_;

  // loads the mission; afterwards the terrain must be set (setTerrain), then restart()
  bool init(const Res& res, const Exe& exe, const std::string& mission, std::string& err, const CreateSpec* spec = nullptr);
  void setTerrain(const std::vector<uint8_t>* depth) { depth_ = depth; }
  void setRadius(Unit& u, double r) { u.radius = r; }
  void step(const Controls& c);     // one 40 Hz step

  // player commands
  void throttleKey(int pct);        // 0, 60, 70, 80, 90, 100, 110
  void throttleStep(int dir);
  void toggleGear();
  void toggleFlaps();
  void cycleWeapon();               // W (0x3a536, 0xff): steps backward
  void selectWeapon(int w);         // keys 1..4: AMRAAM, SIDEWINDER, cannon, JDAM
  void cycleTargets(bool tab, int dir);   // Tab / Shift+Tab (0x30edd / 0x30f4d), ] / [ in the shoot list (0x30dcc / 0x30dfa)
  void nearestTarget(bool bore);    // ' (0x30fba: nearest to the crosshair), Enter: shoot list (0x30d2e)
  void toggleRadar() { radarOn = !radarOn; say(radarOn ? "RADAR ON" : "RADAR OFF", 1.5f); }   // R (0x2b019, 0x221a4)
  // autopilot (0x2db9b): 0 off, 1 AUTOLEVEL (L), 2 AUTOPILOT (A); a stick movement switches it off
  int apMode = 0;
  void toggleAutopilot(int mode) { apMode = apMode == mode ? 0 : mode; }
  int playerWp = 0;                 // the player's current waypoint (+0xAC)
  void nextWaypoint();              // N (0x30ce0)
  // ILS (not from the original): localizer and glide slope of the nearest runway for landing
  struct Ils { bool active = false; double nm = 0, loc = 0, gs = 0, hdg = 0; };   // loc: course deviation (° , + = the runway is to the right), gs: glide-slope deviation (°, + = the aircraft is above the path)
  Ils ils() const;
  bool gunLead(D3& dir) const;      // 0x31bc0: direction of the cannon's intercept point to the locked target (from the aircraft's position)
  void homeWaypoint();              // H (0x30d0a): the second-to-last (home) waypoint + AUTOPILOT
  const Waypoint* playerWaypoint() const;
  void countermeasures();
  // escort commands (Ctrl+P / F / E / M / C): 1 patrol route, 2 formation, 3 attack, 4 the player's target, 5 cover
  bool wingCommand(int cmd);
  int wingmen() const;
  void restart();
  void startEvade(Unit& u);
  void testKill(Unit& u) { kill(u, u.pos, player); }   // self-test         // start an evasive maneuver (0x258b1); the self-test calls it too

  double groundWU(double x, double z) const;
  double groundInterpWU(double x, double z) const;
  bool hitsGround(D3 p, double off = 0) const {   // the original's two-stage test (0x39de5, 0x4101c, 0x2b555): the maximum of the four corners AND the interpolated ground
    return p.y <= groundWU(p.x, p.z) + off && p.y <= groundInterpWU(p.x, p.z) + off;
  }   // the original 0x52796: height interpolated over the cell's two triangles (for touchdown)    // the original 0x5271a: maximum of 4 neighbors x4
  bool los(D3 a, D3 b) const;

  // state (read-only)
  std::vector<Unit> units;
  Unit* player = nullptr;
  std::vector<Missile> missiles;
  std::vector<Bullet> bullets;
  std::vector<Decoy> decoys;
  std::vector<Blast> blasts;
  struct Msg { std::string text; float until; };
  std::vector<Msg> msgs;
  std::vector<Radio> radio;
  float now = 0;                    // s
  int stepNo = 0;
  // player flight state (original fields)
  D3 v;                             // velocity vector wu/step (+0xE9..)
  D3 f{0, 0, 1}, up{0, 1, 0};       // body forward / up
  double yawR = 0, pitchR = 0, rollR = 0;   // original angle unit / step
  int stickX = 0, stickY = 0;       // the step's stick command (+0xCF / +0xD1; the autopilot's too) - for the model animation
  int thrCmd = 0, thrAct = 0;       // %, 8.8
  int gearPos = 0, flapPos = 64, brakePos = 0, bayPos = 0;   // 0..64 / 0..32
  bool gearWantUp = false, flapsWant = true;
  double fuel = 20000;              // font
  int lift = 81;                    // raw lift (81 = 1 g)
  double gDisp = 1, aoaRaw = 0, noz = 0;
  int stallWarn = 0;
  int weapon = 1;                   // 0 cannon, 1 AIM-9, 2 AIM-120, 3 JDAM
  int ammo[4] = {480, 2, 6, 0};
  int chaff = 100, flares = 100, kills = 0;
  bool radarOn = true;
  Unit* lockTgt = nullptr;
  std::vector<Unit*> shootList;
  // ejection (Ctrl+J, 0xe310): the pilot is a separate object (PAR_H00A, with open parachute PAR_H00B), the camera follows him
  struct Chute { bool on = false; D3 pos, vel; double hdg = 0; float t0 = 0; } chute;
  int ejected = 0;                  // [0x52e6e]
  // per-mission pilot counters (0x2a330 + 0x1c·i, incremented by 0x2a530)
  struct Tally { int bulletsFired = 0, bulletsHit = 0, missilesFired = 0, missilesHit = 0, jdamFired = 0, jdamKills = 0; bool landed = false; } tally;
  int missionScore(bool campaign) const;   // 0x105c0
  const std::vector<Trigger>& triggers() const { return triggers_; }
  int musShotsAtPlayer = 0, musShotsFriendly = 0;   // [0x3902c] / [0x39030]: shots fired at the player / from a friendly aircraft (missile 4, cannon 1)
  bool extraUnits = false;          // [0x22c4d]: the units with ORF +0x20 = 1 are loaded too (previous good score)
  void eject();     // Enter (0x30d2e): at most 4 targets, by distance (0x304ec)
  int lockState = 0;                // 0 none, 1 tracking, 2 locked
  double curAz = 0, curEl = 0;      // lock cursor (rad, for display)
  double curX = 0, curY = 0;        // lock cursor offset in body coordinates (wu), 0x2a7f9/0x2a7fd
  int evadeCount[8] = {};           // evasive maneuvers per type (statistics)
  int terrainKills = 0;
  // mission outcome: goals (trigger response 3: 0 primary, 1 secondary, 2 bonus, 3 failure)
  int goalTotal[4] = {}, goalDone[4] = {};
  int playerAirKills = 0, playerGroundKills = 0;
  const std::vector<std::vector<Waypoint>>& routes() const { return routes_; }
  const std::vector<std::vector<TaxiNode>>& taxis() const { return taxis_; }
  bool primaryDone() const { return goalTotal[0] > 0 && goalDone[0] >= goalTotal[0] && goalDone[3] == 0; }
  bool missionFailed() const { return goalDone[3] > 0 || crashed; }             // AI aircraft that crashed into the terrain (statistics)
  int refire = 0, bayLatch = 0, bayTimer = -1;
  int firedBy[16] = {};             // missiles fired per side (statistics)
  int crashed = 0;                  // 0 alive, 1 crashed
  std::string crashWhy;
  std::vector<WeaponDef> wdef;
  std::vector<Perf> perf;
  std::string mission;
  int theater = 1;
  int tod = 1;                      // time of day: upper 4 bits of ORF byte 0 (1 day .. 5 sunset)
  void say(const std::string& s, float sec = 3) { msgs.push_back({s, now + sec}); }
  struct KillEv { int idx; bool air; int debris = -1; };   // debris: number of debris pieces (-1: the usual)
  std::vector<KillEv> killEv;       // the step's destructions (for visual effects; cleared by the main program)
  std::vector<Sfx> sfx;             // the step's sound events (played and cleared by the main program)
  void snd(int id, D3 p, int vol, double pitch = 1, bool positional = true, const std::string& wav = "") {
    sfx.push_back({id, p, vol, pitch, positional, wav});
  }

 private:
  void respond(Trigger& t);
  void parseOrf(const Blob& b);
  void applyCreate(const CreateSpec& cs);
  std::vector<Unit> pool_;            // the template's multiplayer units (Create Mission samples)
  void stepPlayer(const Controls& c);
  void autopilot(Controls& c);      // 0x2db9b: controls in place of the stick
  double safeAlt() const;           // 0x2d8ab: terrain + 400 wu along the route of the next 30 s
  int altHold(double alt) const;    // 0x2d956: pitch command for the target altitude
  void playerGround(double gh);
  void touchdown(double gh);
  void stepLock();
  void fire(Unit* from, int w, Unit* tgt, double hdg, double pitch, int32_t speed, bool dropPhase);
  void fireGun(Unit* from, D3 dir, D3 vel, int speedRaw, bool aaa);
  void stepMissiles();
  void stepBullets();
  void stepGroundUnits();
  void stepAI(Unit& u);
  void steer(Unit& u, D3 to, int32_t speedTarget, bool doSpeed = true);
  bool stepManeuver(Unit& u);
  bool acquire(Unit& u, double range, bool stealth, Unit*& out);
  double stealthOf(const Unit& t) const;
  void damage(Unit& t, int dmg, D3 at, Unit* by = nullptr);
  void kill(Unit& t, D3 at, Unit* by = nullptr);
  void runTriggers();
  void activateGroup(int group, int side);
  void killPlayer(const std::string& why);
  double auth(int32_t vraw) const;
  bool hostile(const Unit& a, const Unit& b) const;
  int typeMask(const Unit& u) const;           // +0x45: 1 airborne, 2 on the ground, 0x40 target (0x245c5)
  bool lineOfSight(const Unit& u) const;       // 0x30c05: the terrain does not block it
  double boreCos(const Unit& u) const;         // cosine of the angle between the target direction and the aircraft's nose
  int32_t thrustRaw(double altWU);

  const Exe* exe_ = nullptr;
  const std::vector<uint8_t>* depth_ = nullptr;
  std::vector<std::vector<Waypoint>> routes_;
  std::vector<std::vector<TaxiNode>> taxis_;
  std::vector<Trigger> triggers_;
  std::vector<Unit> units0_;          // the loaded initial state (for restarting)
  std::vector<Unit*> runways_;
  int32_t TH_[102], CLD_[257], CLL_[257], AUTH_[200];
  int fuelByte_ = 3;
  int startAmmo_[4] = {480, 2, 6, 0};
  D3 spawnPos_;
  double spawnHdg_ = 0;
  bool spawnGround_ = true;
  std::mt19937 rng_{1996};
  int rnd(int n) { return int(rng_() % uint32_t(n)); }
  bool globalEv_[4] = {};
};

}  // namespace sim
