// Mirror image of the original unit record (F22.EXE: the ebp-addressed record of the AI routines between 0x24000-0x41000), and the VM Host,
// which executes the field-based instructions with the original's precision. The meaning of the fields is assembled from the original code and the native simulation
// (comments in sim.cpp); fields of uncertain meaning are named only by their offset.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>

#include "aivm.h"

namespace sim {

// Offsets used by the AI (bytes in the unit record). All accesses are 32-bit unless marked otherwise.
namespace of {
enum : uint32_t {
  SIDE_FLAGS = 0x02,   // byte: side / flags (2: ground, 4 / 8 ...)
  X = 0x08, Y = 0x0c, Z = 0x10,   // position, 16.16 wu (altitude is Y)
  ANG_A = 0x24, ANG_B = 0x28, ANG_C = 0x2c,   // angles: heading, pitch, roll (0x1ffffff = 360°)
  ID = 0x41,           // word: the unit's index
  TYPE = 0x45,         // byte: 1 airborne, 2 on the ground, 0x40 target
  HANDLER = 0x53,      // the "behavior" function
  OWNER = 0x57,        // owner unit (the launcher for a missile)
  NEXT = 0x34,         // next element of the unit list (pointer)
  SLOT = 0x67,
  GROUP_ID = 0x6a,     // byte: side mask / group id used in searches (cl)
  WING_NEXT = 0x97,    // next wingman of the formation (pointer)
  STEALTH = 0xa3,      // 16.16 factor: the detection distance of hidden targets is multiplied by this (for bits 0x22 of the +2 field)
  SLOT_REC = 0xe3,
  WING_SIB = 0xe7,     // neighboring element of the formation (the notification chain)
  GROUP_IDX = 0xef,    // byte: group index in the leader table (0x24940)     // pointer to the formation slot record (+0xc / +0x10: heading and pitch offsets)
  DEATH = 0x93,        // id of the death handler (0x2668f: leader, 0x26714: wingman)
  ROLE = 0xf5,         // byte: role (indexes the script table at 0x36ba0; 1: joiner, 2: leader seeking joiners)
  F1 = 0xf1,           // byte: role (1: flying in formation)
  F6 = 0xf6, F7 = 0xf7, F8 = 0xf8, F9 = 0xf9,   // bytes: F6 "bound", F7 type mask for searching, F8 counter, F9 leader flag         // byte: index in the group
  TIMER = 0x6f,        // step counter
  DIRX = 0x73, DIRY = 0x77, DIRZ = 0x7b,   // movement direction (unit vector, 16.16)
  SPEED = 0x7f,        // speed (16.16 wu / step)
  VEL_B = 0x83,        // (meaning not verified)
  TARGET = 0x8f,       // the target unit (pointer; 0: none)
  WP_PTR = 0x9b, WP_BASE = 0x9f,   // waypoint pointer and the base of the route (16-byte waypoints)
  VM = 0xa7,           // the VM context (r0..r3 = 0xa7.., ret = +0x10, flags = +0x14, PC = +0x18 -> 0xbf)
  SCRIPT = 0xbf,       // PC of the current script
  TAXI = 0xc3,
  TURN_RATE = 0xc7,    // turn factor (for the heading error limit)         // ground route pointer
  TURN_A = 0xcb, TURN_B = 0xcf,   // turn limit (heading / pitch, angle units)
  CMD_ANG = 0xd3,      // commanded heading
  CMD_PITCH = 0xd7,
  CMD_ROLL = 0xdb,     // commanded roll (0x80000000 = none)    // commanded pitch
  LEADER = 0xdf,       // leader unit
  PENDING = 0xeb,      // new script entry (the VM takes it over in the next step)
  SKILL = 0xf0,        // byte: skill
  STATE = 0xf3,        // byte: AI state (0 parked ... 7)
  PREV_STATE = 0xfa,   // byte: previous AI state (saved by ground evasion)
  INCOMING = 0xfb,   // byte: incoming missile flags (1: infrared -> flare, other: radar -> chaff; cleared every step)
  PERF = 0xff,         // pointer to the performance record
  F10B = 0x10b,        // counter (the patrol step increases it by 0x14000)
  LAST_LAUNCH = 0x107, // time of the launcher's last launch (per counter 0x52d34)
  CMD_SPEED = 0x103,   // commanded speed
  MIN_RANGE = 0x10f,   // (meaning not verified)
};
}  // namespace of

struct OUnit {
  static constexpr size_t SIZE = 0x140;
  uint8_t b[SIZE] = {};
  int32_t get(uint32_t o) const { int32_t v; std::memcpy(&v, b + o, 4); return v; }
  void set(uint32_t o, int32_t v) { std::memcpy(b + o, &v, 4); }
  uint8_t get8(uint32_t o) const { return b[o]; }
  void set8(uint32_t o, uint8_t v) { b[o] = v; }
};

// The VM context in the unit record: r0..r3 (0xa7..0xb6), ret (0xb7), flags (0xbb), PC (0xbf) - the original ebp+0xa7 + 0x00/0x10/0x14/0x18.
inline void loadCtx(const OUnit& u, AiCtx& c) {
  for (int i = 0; i < 4; i++) c.r[i] = u.get(of::VM + 4 * i);
  c.ret = uint32_t(u.get(of::VM + 0x10)); c.flags = uint32_t(u.get(of::VM + 0x14)); c.pc = uint32_t(u.get(of::VM + 0x18));
  c.pending = uint32_t(u.get(of::PENDING));
}
inline void storeCtx(OUnit& u, const AiCtx& c) {
  for (int i = 0; i < 4; i++) u.set(of::VM + 4 * i, c.r[i]);
  u.set(of::VM + 0x10, int32_t(c.ret)); u.set(of::VM + 0x14, int32_t(c.flags)); u.set(of::VM + 0x18, int32_t(c.pc));
  u.set(of::PENDING, 0);
}

// The unit list: the original records, pointers are "addresses" (BASE + index * 0x140), the chain is in the +0x34 field; 0 is the null pointer.
struct OWorld {
  static constexpr uint32_t BASE = 0x100000;
  std::vector<OUnit*> units;
  uint32_t head = 0;                                         // 0x304b8: a lista feje
  uint32_t add(OUnit* x) {                                   // the new unit is appended to the end of the list
    units.push_back(x);
    uint32_t p = BASE + uint32_t(units.size() - 1) * uint32_t(OUnit::SIZE);
    x->set(of::NEXT, 0);
    if (!head) head = p; else { uint32_t q = head; while (at(q)->get(of::NEXT)) q = uint32_t(at(q)->get(of::NEXT)); at(q)->set(of::NEXT, int32_t(p)); }
    return p;
  }
  OUnit* at(uint32_t p) const {
    if (p < BASE) return nullptr;
    size_t i = (p - BASE) / OUnit::SIZE;
    return i < units.size() ? units[i] : nullptr;
  }
};

// Group handling (F22.EXE 0x23770 / 0x2385a / 0x267d7 / 0x268e2 / 0x2668f / 0x26714 / 0x26824): 36 groups (0x24 of them), each with one leader (table 0x24940),
// at most 10 wingmen (0x249d0 + group * 0x28), the wingmen chained (+0x97) from the leader's +0x97 field, each one's +0xdf field is the leader.
struct OGroups {
  static constexpr int N = 0x24;
  static constexpr uint32_t H_LEADER = 0x2668f, H_WING = 0x26714;
  OWorld* w = nullptr;
  const std::vector<uint8_t>* img = nullptr;
  uint32_t leader[N] = {};
  uint32_t wings[N][10] = {};
  int8_t alive[N] = {};                                      // 0x256a6: number of aircraft of the group that have not yet taken off (parked / taxiing / on the ground)
  std::function<void(int kind, int cond, int id)> onEvent;   // 0x23cc0 (cl, ch, bx)
  void addLeader(int idx, uint32_t p);                       // 0x2385a
  void addWing(int idx, int slot, uint32_t p, uint32_t slotRec);   // 0x267d7 (slot starts at 1)
  void buildChains();                                        // 0x268e2
  void leaderDied(uint32_t p);                               // 0x2668f: the next wingman becomes the leader
  void wingDied(uint32_t p);                                 // 0x26714: unlinking
  void join(uint32_t leaderPtr, const std::vector<uint32_t>& joiners);   // 0x26824: the nearby joiners are placed at the front of the leader's chain
  void groundedGone(uint32_t p);                             // 0x2691d: the group's ground counter decreases; event at zero
  void activateGroup(int idx);                               // 0x23ab7: the leader goes from parked to taxiing (0x374c8)
  void activateUnit(uint32_t p);                             // 0x23ae7: script by role (0x36ba0), taxi state
  void promote(OUnit* n);                                    // 0x26650
};

// The original random number generator (0x56682): s = rol(rol(s, 11) + s, 3)
struct AiRng {
  uint32_t s = 0x1234567;
  uint32_t next() {
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    uint32_t t = rol(s, 11) + s;
    s = rol(t, 3);
    return s;
  }
};

// Host: field-based instructions (STF 0a, WPSET 0d, RND 13, LDF 15, AGL 1e, STATE 28) with the original's precision;
// the rest is implemented by the derived class (default: does not stop, the helper is "done").
struct OHost : AiHost {
  OUnit* u = nullptr;
  AiRng rng;
  const std::vector<uint8_t>* img = nullptr;   // the EXE image: the sine (0x56840) and arctangent table (0x3aa7c)
  int32_t perf[64] = {};                       // the performance record (pointed to by [ebp+0xff]): +0x04 max speed, +0x20 acceleration
  std::function<void(uint32_t ptr, int32_t& x, int32_t& y, int32_t& z)> wpAt;   // position of the ground waypoint ([ebp+0xc3])
  std::function<int32_t(int32_t x, int32_t z)> groundH;   // 0x5271a: ground height (maximum of four neighbors x 4)
  uint32_t readWord(uint32_t) override { return 0; }
  bool op(int code, const uint8_t* ins, AiCtx& c) override;
  // the original common math
  void sinCos(uint32_t ang, int32_t& s, int32_t& c) const;   // 0x575e6: sine / cosine, 16.16, linear interpolation
  int32_t atan2o(int32_t x, int32_t z) const;                // 0x3a8f0: heading (0x1ffffff = 360°)
  void rebuildDir();
  void rotate(int32_t v[3], uint32_t a, uint32_t b, uint32_t c) const;   // 0x575e6 + 0x5785e / 0x578a0 / 0x5781c: rotate the vector (in C, B, A order)                                         // 0x40c75: movement direction from the angles (+0x73)
  bool groundAhead();                                        // 0x2583e: ground is close ahead -> evade (state 7)
  bool levelOut();                                           // 0x25d48: pitch / roll slowly to zero
  bool steerTaxi(int mode);                                  // 0x25be6: turns toward the ground waypoint (mode 1: speed too)
  bool taxiTo();                                             // 0x25b9f: taxi to the waypoint
  bool turnToCmd();                                          // 0x25c5a: turns to the commanded heading
  int32_t dist3(int32_t dx, int32_t dy, int32_t dz) const;   // 0x52aaa: distance approximation: max + (sum of the other two) * 0x5000 / 65536
  void matchSpeed(int32_t tgtSpeed, int32_t dist, int32_t range, int32_t perfMin, int32_t accel);   // 0x25780
  bool speedTo();
  OWorld* world = nullptr;
  std::function<bool(const OUnit& target)> los;               // 0x30c05: line of sight to the target (true = visible); default: always
  std::function<void(const OUnit&, int32_t& a, int32_t& b)> slotAt;   // the formation slot record (+0xc / +0x10)
  enum { TS_MIN = 1, TS_STEALTH = 2, TS_LOS = 4, TS_ME = 8 };
  // the target search family (0x30871 / 0x308f9 / 0x30974 / 0x309fe): the nearest unit that satisfies the side and type mask and is within range;
  // TS_MIN: farther than minDist; TS_STEALTH: for hidden (+2 & 0x22) targets the range * +0xa3; TS_LOS: line of sight; TS_ME: the target is targeting me
  uint32_t findTarget(uint8_t sideMask, uint8_t typeMask, uint32_t range, uint32_t minDist, int flags, uint32_t* bestDist = nullptr);
  void assignWing();
  std::function<void(const OUnit&, int32_t v[3])> slotVec;   // vector of the formation slot record (relative to the leader)
  bool maneuver();                                           // 0x26142: turns toward the commanded pitch / heading / roll; done when all are reached
  void formationStep();                                      // 0x25e4b: formation flying: aims at the slot next to the leader, matches speed
  std::function<void(int kind)> weapon;                      // weapon release: 0 cannon (0x3a32d), 1 missile (0x39b6f, type from performance record +0x26), 2 bomb (0x3a0e4)
  std::function<uint32_t(int idx)> groupLeader;              // 0x24940: the group's leader (pointer; 0: none)
  void notifyAttacked(uint32_t victimPtr, bool force = false);                   // 0x270a6 / 0x270ed: a wingman of the attacked group goes after the attacker (me)
  bool fireAtTarget();                                       // 0x25cde: fires at the target by distance (cannon at close range, missile at long range, bomb on a ground target)
  bool pursuitStep();                                        // 0x25dab: pursuit: aiming, speed matching, done when the distance <= +0x10f
  std::function<int32_t(uint32_t ptr, uint32_t off)> wpWord;  // a word of a ground waypoint record (at address ptr + off, 16 bit)
  std::function<void(int kind, int cond)> onEvent;           // 0x23cc0: event dispatcher (cl = kind: 1 unit, 0 group; ch = cond: 1 took off, 2 waypoint, ...)
  std::function<void()> onLiftoff;                           // 0x2691d: takeoff (landing gear etc.)
  const uint32_t* skillSpeed36c02 = nullptr;                 // (unused: the tables come from the EXE image)
  void takeoffStep(AiCtx& c);                                // op 22 (0x372e8)
  void taxiWaypoint(AiCtx& c);                               // op 20 (0x37259)
  void formOp(AiCtx& c);                                     // op 25 (0x37395)
  uint32_t coverFor(uint32_t leaderPtr);                     // 0x30825 on behalf of the given unit
  void actOp(int which);                                     // op 1a (0x37154): 0 target search, 1 the leader's target, 3 the nearest attacker of the leader
  void classOp(int cls);                                     // op 24 (0x3737c): operation of the air waypoint's class (0 next, 1 jump, 2/3 target assignment, 6 event)
  void assignToUnit(OUnit& w, uint32_t targetPtr, uint8_t f8);   // sets the target, the 0x375a8 entry and, from the formation slot, the commanded heading / pitch
  std::function<int32_t(int32_t x, int32_t z)> groundI;      // 0x52796: ground height, bilinearly interpolated (default: groundH)
  bool visible(const OUnit& t) const;                        // 0x30c05: line of sight - the terrain does not block along the ray (true = visible)
  void move();                                               // 0x40cfa: position advances by movement direction * speed / 65536
  int32_t now = 0;                                           // 0x52d34: the game's step counter
  std::function<void(uint32_t ptr, uint32_t off, int v)> wpSetWord;   // write a word of a waypoint record (CLS 5: one-time)
  std::function<void(int idx)> missionRadio;                 // CLS 5 (0x27366): the mission radio message (0x90-byte records: text + WAV)
  std::function<void(OUnit& launcher, uint32_t launcherPtr)> launchSam;   // 0x39b95: launch a SAM from the launcher (its target is +0x8f)
  std::function<void(int32_t hdg, int32_t pitch)> aaaBurst;  // 0x3a345: anti-aircraft burst from the aircraft position (angles: 2^25 = 360°)
  uint32_t pickLauncher(const OUnit& t);                     // 0x26cb3: of the launchers in the chain, the one facing the target most, if it has not fired for 0x12c0 steps
  void radarStep();                                          // op 26 (0x26d32): radar - target search every 32 steps up to 3800 wu, event, SAM launch
  void aaaStep();                                            // op 27 (0x26d9a): anti-aircraft gun - nearest target every 32 steps, lead, turn, burst
  void assignFromLeader();                                   // CLS 4 (0x2728c): target assignment up to 0x16440000, with ever farther targets, for the wingmen and the leader
  bool leaderFar();                                          // 0x25c9e: the leader is far (> 0x16440000) -> back to the formation script (state 4)
  bool groundAttack();                                       // 0x25fa4: attack on a ground target (performance record +0x26 == 3): aims at 0x57e0000 above the target
  bool attackStep();                                         // 0x26028: attack on the target: aiming, speed matching, the target goes defensive
  bool patrolStep();                                         // 0x25b31: patrol between the waypoints, firing / wingman assignment meanwhile                                         // 0x27166: the leader assigns ever farther targets to its wingmen
  std::function<const uint8_t*(const OUnit&)> perfOf;        // the performance record of a unit (default: its own `perf`)
  std::function<void(int n)> radio;                         // RADIO 1b (0x37169): the escort's radio message (text 0x36bc8, sound 0x36bf8); the aircraft (u) is the caller
  std::function<void(bool missile)> spawnShot;               // 0x3a5da / 0x3a722: release of a decoy (true: flare, false: chaff) - received by the simulation
  bool near3(int32_t x, int32_t y, int32_t z, int32_t range) const;   // 0x259d4: within the limit on all three axes
  bool provoke(OUnit& v, bool force);                        // 0x258b1: unit v enters the "defensive" (6th) state, with a random evasion script
  bool fireCheck();                                          // 0x257e9: fires from time to time, then the aircraft itself evades
  uint32_t self = 0;                                         // the aircraft's "address" (ebp)
  int32_t dist3to(const OUnit& o) const;                     // 0x30780: distance from another unit
  uint32_t coverUnit();                                      // 0x30825: the nearest unit targeting me (0: none)
  void aimAt(int32_t tx, int32_t ty, int32_t tz, int32_t* yawErr = nullptr, int32_t* pitchErr = nullptr);   // 0x25a36 (+0x260fe, 0x2611d, 0x25ae9)                                            // 0x36f9c: speed toward the commanded
  virtual bool other(int code, const uint8_t* ins, AiCtx& c) { (void)code; (void)ins; c.flags |= F_CF; c.pc += 4; return true; }
};

}  // namespace sim
