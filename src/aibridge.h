// Hooking the original AI scripts (aivm / aiunit) into the native simulation (experimental: --aivm switch or F22_AIVM=1).
// Each unit has an original record (OUnit); the air AI aircraft are driven by the VM (taxiing, takeoff, route, formation, attack, evasion),
// the native state machine does not run for them. Position / angles / speed live in the record in original units and are written back to the native Unit at the end of every step.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>


namespace sim {

class World;
struct Unit;

class AiBridge {
 public:
  explicit AiBridge(const std::vector<uint8_t>* exeImage);
  ~AiBridge();
  bool dirty = true;                                       // must be rebuilt before the next step (after restart)
  void step(World& w);                                     // one 40 Hz step for every air AI aircraft
  bool drives(const Unit& u) const;                        // whether the VM drives it
  void onActivate(World& w, Unit& u);                      // aircraft activated by a mission event (taxi from parking / route)
  void onHit(World& w, Unit& victim, Unit* by);
  bool wingCommand(World& w, int cmd);
  void reseed(World& w, Unit& u);                          // record of an aircraft moved on the native side (tests) rebuilt from the native state; the group chain stays                     // the player's escort command (0x2770d ... 0x27780): new script for the first wingman            // hit: evasion (0x258b1) and notifying the group (0x270a6)
  int steps = 0;                                           // statistics: VM steps executed
 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
  void rebuild(World& w);
};

}  // namespace sim
