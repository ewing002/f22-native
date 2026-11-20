#include "fx.h"

#include <algorithm>
#include <cmath>

using sim::D3;

static int framesOf(const Model* m) {
  size_t n = 1;
  if (m) for (auto& b : m->batches) n = std::max(n, b.frames.size());
  return int(n);
}
static constexpr double F16 = 1.0 / 65536;   // 16.16 -> wu

void Effects::reset() { ps_.clear(); wrecks_.clear(); craters_.clear(); }

void Effects::puffFsmoke(D3 at, int life, int shift, double scale, double sv) {
  if (!m_.fsmoke) return;
  P p{PUFF, m_.fsmoke, at, {}, {}};
  p.life = life; p.shift = shift; p.scale = scale; p.sv = sv;
  ps_.push_back(p);
}

void Effects::step(sim::World& W) {
  // destructions
  for (auto& k : W.killEv) {
    if (k.idx < 0 || k.idx >= int(W.units.size())) continue;
    sim::Unit& u = W.units[k.idx];
    // debris (0x317b4): 5-12 pieces for an air target, 5 for a ground one (when the aircraft / building dies), 0x317ba-0x31887
    if (m_.debris) {
      int n = k.debris >= 0 ? k.debris : k.air ? 5 + (rnd() & 7) : 5;
      for (int i = 0; i < n; i++) {
        D3 d{rnd() / 32768.0 - 1, rnd() / 32768.0 - 1, rnd() / 32768.0 - 1};
        double l = sim::len(d);
        if (l < 1e-3) d = {0, 1, 0}; else d = d * (1 / l);
        P p{DEBRIS, m_.debris, u.pos, d, {}};
        p.speed = 0x6000 * F16; p.accel = 2 * F16;
        p.scale = (0x18000 + (rnd() & 0x1ffff)) * F16;
        p.shift = 30; p.life = 0xA0;
        p.timer = rnd() & 3;                              // frame (0x31824)
        p.r1 = rnd() / 65535.0 * 6.28; p.r2 = rnd() / 65535.0 * 6.28; p.r3 = rnd() / 65535.0 * 6.28;
        ps_.push_back(p);
      }
    }
    if (k.air) {                                          // 0x2bbc2: falling wreck
      wrecks_.push_back({k.idx, u.pos, u.hdg, u.pitch, u.roll, std::max(u.speed, 0.4)});
      continue;
    }
    if (u.cls != sim::C_RUNWAY) craters_.push_back(k.idx);   // 0x410ab: crater where the model was
    // 0x314ed: 6 fireballs along the 6 axes, randomly rotated by ±67.5°, 0.4 wu/step
    static const D3 AX[6] = {{0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}};
    for (int i = 0; i < 6 && m_.ex3; i++) {
      double a = (rnd() / 65535.0 * 2 - 1) * 67.5 * sim::PI / 180, b = (rnd() / 65535.0 * 2 - 1) * 67.5 * sim::PI / 180;
      D3 d = AX[i];
      d = {d.x * std::cos(a) + d.z * std::sin(a), d.y, -d.x * std::sin(a) + d.z * std::cos(a)};
      d = {d.x, d.y * std::cos(b) - d.z * std::sin(b), d.y * std::sin(b) + d.z * std::cos(b)};
      D3 v = d * (26214 * F16);
      v.y = std::fabs(v.y) + 0x200 * F16;                 // always starts upward
      P p{FIREBALL, m_.ex3, u.pos, v, {}};
      p.timer = (rnd() & 31) + 4; p.life = 1 << 30; p.shift = 4;
      ps_.push_back(p);
    }
    if (m_.flames) { P f{FLAMES, m_.flames, u.pos, {}, {}}; f.life = 0x4B0; f.shift = 0; f.timer = 1; ps_.push_back(f); }
  }
  W.killEv.clear();

  // missile smoke trails: during the guided phase, below 100,000 ft (0x3982e), per weapon table +0x30
  if (m_.strail)
    for (auto& m : W.missiles)
      if (!m.dead && m.drop == 0 && m.w != 2 && m.w != 0 && m.pos.y < 0x186A) {
        P p{PUFF, m_.strail, m.pos, {}, {}};
        p.hdg = m.hdg; p.pitch = m.pitch; p.oriented = true;   // the trail extends behind the missile
        p.shift = 3; p.life = 4 << 3;                        // the STRAIL texture record specifies 4 frames (+0x19)
        ps_.push_back(p);
      }

  // falling wrecks: nose pitches down (at most 0x6590 angle units / step), smoke every 4 steps
  for (auto it = wrecks_.begin(); it != wrecks_.end();) {
    Wreck& w = *it;
    double target = -sim::PI / 2, maxd = 0x6590 * sim::ANG;
    w.pitch += std::clamp(target - w.pitch, -maxd, maxd);
    w.roll += 0.03;
    w.pos += sim::dirOf(w.hdg, w.pitch) * w.speed;
    w.pos.x = std::fmod(w.pos.x + sim::WORLD, sim::WORLD); w.pos.z = std::fmod(w.pos.z + sim::WORLD, sim::WORLD);
    if ((++w.age & 3) == 0) puffFsmoke(w.pos, 0x50, 30, 0.25, 0x100 * F16);
    double g = W.groundWU(w.pos.x, w.pos.z);
    if (w.pos.y <= g || w.age > 4000) {                   // impact: explosion and burning wreck (0x2b106)
      D3 at{w.pos.x, g, w.pos.z};
      W.blasts.push_back({at, W.now, 3.2f});
      W.snd(sim::SND_EXPLODE, at, 255);
      if (m_.flames) { P f{FLAMES, m_.flames, at, {}, {}}; f.life = 0x4B0; f.shift = 0; f.timer = 1; ps_.push_back(f); }
      it = wrecks_.erase(it);
    } else ++it;
  }

  // particles
  std::vector<P> born;
  for (auto& p : ps_) {
    p.age++;
    switch (p.kind) {
      case PUFF: p.scale += p.sv; break;
      case DEBRIS:                                        // 0x31754: flight that keeps slowing, tumbling at 14°/step
        p.accel += 2 * F16; p.speed = std::max(0.0, p.speed - p.accel);
        p.pos += p.vel * p.speed;
        p.r1 += 0x140000 * sim::ANG; p.r2 += 0x140000 * sim::ANG; p.r3 += 0x140000 * sim::ANG;
        break;
      case RISE:                                          // 0x31110: accelerating drift and rise, growing size
        p.vel += p.acc; p.pos += p.vel; p.sv += p.sa; p.scale += p.sv; break;
      case FLAMES:                                        // 0x31179: smoke puff every 30 steps
        if (--p.timer <= 0 && m_.fsmoke) {
          p.timer = 0x1E;
          P s{RISE, m_.fsmoke, p.pos, {0, 0x2000 * F16, 0}, {((rnd() & 3) - 1 + 0x19) * F16, 0xC * F16, ((rnd() & 3) - 2) * F16}};
          s.life = 0x200; s.shift = 7; s.scale = 0.75; s.sa = 4 * F16;
          born.push_back(s);
        }
        break;
      case FIREBALL:                                      // 0x313cb: gravity, 1/256 air drag
        p.vel.y -= 0x51 * F16;
        p.pos += p.vel;
        p.vel = p.vel - p.vel * (1.0 / 256);
        if (--p.timer <= 0) { p.kind = EMITTER; p.life = p.age + 0x80; p.timer = 0; }
        break;
      case EMITTER:                                       // 0x31357: smoke every 16 steps
        if ((p.timer++ & 15) == 0) {
          P s{PUFF, m_.fsmoke, p.pos, {}, {}};
          s.life = framesOf(m_.fsmoke) << 4; s.shift = 4; s.scale = 0.5; s.sv = 0x300 * F16;
          if (s.m) born.push_back(s);
        }
        break;
    }
  }
  ps_.erase(std::remove_if(ps_.begin(), ps_.end(), [](const P& p) { return p.age >= p.life; }), ps_.end());
  ps_.insert(ps_.end(), born.begin(), born.end());
  if (ps_.size() > 6000) ps_.erase(ps_.begin(), ps_.begin() + (ps_.size() - 6000));
}

void Effects::draw(sim::World& W, const std::vector<Model*>& unitModel, std::vector<DrawItem>& out,
                   const std::function<V3(D3)>& rpos, const std::function<M3(V3)>& bb,
                   const std::function<M3(double, double, double)>& att, const std::set<std::string>* airGear) const {
  for (int i : craters_) {
    if (!m_.pothole || i >= int(W.units.size())) continue;
    const sim::Unit& u = W.units[i];
    DrawItem it{m_.pothole, rpos(u.pos), att(u.hdg, 0, 0)};
    it.decal = true;
    out.push_back(it);
  }
  for (auto& w : wrecks_) {
    if (w.idx >= int(unitModel.size()) || !unitModel[w.idx]) continue;
    DrawItem it{unitModel[w.idx], rpos(w.pos), att(w.hdg, w.pitch, w.roll)};
    it.hide = airGear;
    out.push_back(it);
  }
  for (auto& p : ps_) {
    if (p.kind == EMITTER || !p.m) continue;
    V3 q = rpos(p.pos);
    DrawItem it{p.m, q, p.kind == DEBRIS ? att(p.r1, p.r2, p.r3) : p.oriented ? att(p.hdg, p.pitch, 0) : bb(q), float(p.scale)};
    it.frame = p.kind == DEBRIS ? p.timer : p.age >> p.shift;
    out.push_back(it);
  }
}
