// Visual effects following the original object behaviors (F22.EXE):
//  - missile smoke trail (0x397b7 / 0x31328): one STRAIL puff per step, 4 frames x 8 steps
//  - ground target destruction (0x4107c): the model turns into a crater (POTHOLE), 6 flying-apart fireballs
//    (0x314ed / 0x31424, EX3, then smoke: 0x31357), and burns for 30 s (0x3121b, FLAMES), from which
//    a rising, drifting, growing smoke puff (FSMOKE, 0x311a2 / 0x31110) emerges every 30 steps
//  - shot-down aircraft (0x2bbc2): pitches nose down and crashes, smoking every 4 steps (0x2bc48);
//    explodes and burns at the impact point
#pragma once
#include <functional>
#include <vector>

#include "sim.h"
#include "world.h"

class Effects {
 public:
  struct Models { Model *debris = nullptr, *strail = nullptr, *fsmoke = nullptr, *flames = nullptr, *ex3 = nullptr, *pothole = nullptr; };
  void init(const Models& m) { m_ = m; }
  void reset();
  // one 40 Hz step after the simulation (processes and clears the W.killEv events)
  void step(sim::World& W);
  // elements to draw: rpos = world (wu) -> camera-relative meters; bb = rotation facing the camera
  void draw(sim::World& W, const std::vector<Model*>& unitModel, std::vector<DrawItem>& out,
            const std::function<V3(sim::D3)>& rpos, const std::function<M3(V3)>& bb,
            const std::function<M3(double, double, double)>& att, const std::set<std::string>* airGear) const;

 private:
  enum Kind { PUFF, RISE, FLAMES, FIREBALL, EMITTER, DEBRIS };
  struct P {
    Kind kind; Model* m;
    sim::D3 pos, vel, acc;
    double scale = 1, sv = 0, sa = 0;
    int age = 0, life = 0, shift = 3, timer = 0;
    double hdg = 0, pitch = 0;
    double speed = 0, accel = 0, r1 = 0, r2 = 0, r3 = 0;   // debris: speed, deceleration, rotation angles
    bool oriented = false;            // with the creator's angles (0x40e08), not facing the camera
  };
  struct Wreck { int idx; sim::D3 pos; double hdg, pitch, roll, speed; int age = 0; };
  void puffFsmoke(sim::D3 at, int life, int shift, double scale, double sv);
  Models m_;
  std::vector<P> ps_;
  std::vector<Wreck> wrecks_;
  std::vector<int> craters_;
  uint32_t seed_ = 12345;
  int rnd() { seed_ = seed_ * 1103515245u + 12345u; return int((seed_ >> 8) & 0xFFFF); }
};
