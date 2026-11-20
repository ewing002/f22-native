// Small vector/matrix library (column-major matrices, as in OpenGL).
#pragma once
#include <cmath>

struct V3 {
  float x = 0, y = 0, z = 0;
  V3() = default;
  V3(float a, float b, float c) : x(a), y(b), z(c) {}
  V3 operator+(V3 o) const { return {x + o.x, y + o.y, z + o.z}; }
  V3 operator-(V3 o) const { return {x - o.x, y - o.y, z - o.z}; }
  V3 operator*(float s) const { return {x * s, y * s, z * s}; }
  V3& operator+=(V3 o) { x += o.x; y += o.y; z += o.z; return *this; }
};
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 norm(V3 a) { float l = len(a); return l > 1e-12f ? a * (1 / l) : V3{0, 0, 0}; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
// rotate v around unit axis k (Rodrigues)
inline V3 rodrigues(V3 v, V3 k, float a) {
  float c = std::cos(a), s = std::sin(a);
  return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}

struct M4 { float m[16]; };
inline M4 mul(const M4& a, const M4& b) {
  M4 r;
  for (int c = 0; c < 4; c++)
    for (int rr = 0; rr < 4; rr++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
      r.m[c * 4 + rr] = s;
    }
  return r;
}
inline M4 perspective(float fovy, float aspect, float n, float f) {
  float t = 1 / std::tan(fovy / 2), nf = 1 / (n - f);
  return {{t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) * nf, -1, 0, 0, 2 * f * n * nf, 0}};
}
// view rotation without translation (the scene is drawn relative to the camera)
// The original world is left-handed (x right, y up, z forward): it is carried into the right-handed OpenGL image by an X mirror,
// otherwise the image would be a mirror image of the original.
inline M4 flipX() { return {{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}; }
inline M4 viewRot(V3 fwd, V3 up) {
  V3 s = norm(cross(fwd, up)), u = cross(s, fwd);
  return {{s.x, u.x, -fwd.x, 0, s.y, u.y, -fwd.y, 0, s.z, u.z, -fwd.z, 0, 0, 0, 0, 1}};
}
// model rotation (column-major mat3): right, up, back - model noses point toward -Z
struct M3 { float m[9]; };
inline M3 modelR(V3 f, V3 up) {
  V3 r = cross(f, up);
  return {{r.x, r.y, r.z, up.x, up.y, up.z, -f.x, -f.y, -f.z}};
}
inline M3 yawMat(float a) {
  float c = std::cos(a), s = std::sin(a);
  return {{c, 0, -s, 0, 1, 0, s, 0, c}};
}
