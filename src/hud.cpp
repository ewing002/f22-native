#include "hud.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>

#include "ui.h"

static std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
  return w;
}

void Hud::init() {
  dc_ = CreateCompatibleDC(nullptr);
  glGenTextures(1, &tex_);
  glBindTexture(GL_TEXTURE_2D, tex_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  prog_ = makeProgram(R"(#version 330 core
    out vec2 vUV;
    void main() {
      vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
      vUV = vec2(p.x, 1.0 - p.y);
      gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    })",
                      R"(#version 330 core
    in vec2 vUV; uniform sampler2D uTex; uniform float uBright; out vec4 o;
    // the black background is transparent; color intensity gives the opacity (antialiased edges)
    void main() {
      vec3 c = texture(uTex, vUV).bgr;
      float a = max(c.r, max(c.g, c.b));
      o = vec4(c / max(a, 1e-3) * min(uBright, 1.0), clamp(a * 1.6 * uBright, 0.0, 1.0));
    })");
  glGenVertexArrays(1, &vao_);
}

HFONT Hud::font(int size) {
  if (size < 8) size = 8;
  if (size > 63) size = 63;
  if (!fonts_[size])
    fonts_[size] = CreateFontW(-size, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
  return fonts_[size];
}

void Hud::begin(int w, int h) {
  if (w != w_ || h != h_ || !bmp_) {
    if (bmp_) DeleteObject(bmp_);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top to bottom
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    SelectObject(dc_, bmp_);
    w_ = w; h_ = h;
  }
  GdiFlush();
  std::memset(bits_, 0, size_t(w_) * h_ * 4);
  noClip();
  SetBkMode(dc_, TRANSPARENT);
}

void Hud::text(int x, int y, const std::string& s, COLORREF c, int size, int align) {
  SelectObject(dc_, font(size));
  SetTextColor(dc_, c);
  SetTextAlign(dc_, (align == 1 ? TA_CENTER : align == 2 ? TA_RIGHT : TA_LEFT) | TA_BASELINE);
  std::wstring w = widen(s);
  TextOutW(dc_, x, y, w.c_str(), int(w.size()));
}

void Hud::rect(int x, int y, int w, int h, COLORREF c) {
  HPEN pen = CreatePen(PS_SOLID, 2, c);
  HGDIOBJ old = SelectObject(dc_, pen);
  SelectObject(dc_, GetStockObject(NULL_BRUSH));
  Rectangle(dc_, x, y, x + w, y + h);
  SelectObject(dc_, old);
  DeleteObject(pen);
}

void Hud::circle(int x, int y, int r, COLORREF c) {
  HPEN pen = CreatePen(PS_SOLID, 2, c);
  HGDIOBJ old = SelectObject(dc_, pen);
  SelectObject(dc_, GetStockObject(NULL_BRUSH));
  Ellipse(dc_, x - r, y - r, x + r, y + r);
  SelectObject(dc_, old);
  DeleteObject(pen);
}

void Hud::line(int x0, int y0, int x1, int y1, COLORREF c) {
  HPEN pen = CreatePen(PS_SOLID, 2, c);
  HGDIOBJ old = SelectObject(dc_, pen);
  MoveToEx(dc_, x0, y0, nullptr);
  LineTo(dc_, x1, y1);
  SelectObject(dc_, old);
  DeleteObject(pen);
}

// artificial horizon: sky/ground half-circle according to roll and pitch
void Hud::horizon(int cx, int cy, int r, float roll, float pitch) {
  HRGN clip = CreateEllipticRgn(cx - r, cy - r, cx + r, cy + r);
  SelectClipRgn(dc_, clip);
  HBRUSH skyB = CreateSolidBrush(RGB(30, 55, 95)), gndB = CreateSolidBrush(RGB(80, 52, 28));
  RECT all = {cx - r, cy - r, cx + r, cy + r};
  FillRect(dc_, &all, skyB);
  float a = -roll * 3.14159265f / 180, off = pitch * 1.2f;
  float c = std::cos(a), s = std::sin(a);
  auto P = [&](float x, float y) { return POINT{LONG(cx + x * c - y * s), LONG(cy + x * s + y * c)}; };
  POINT g[4] = {P(-2 * r, off), P(2 * r, off), P(2 * r, 3 * r), P(-2 * r, 3 * r)};
  HGDIOBJ ob = SelectObject(dc_, gndB);
  HGDIOBJ op = SelectObject(dc_, GetStockObject(NULL_PEN));
  Polygon(dc_, g, 4);
  SelectObject(dc_, op);
  SelectObject(dc_, ob);
  SelectClipRgn(dc_, nullptr);
  DeleteObject(clip); DeleteObject(skyB); DeleteObject(gndB);
  POINT h0 = P(-2 * r, off), h1 = P(2 * r, off);
  HRGN clip2 = CreateEllipticRgn(cx - r, cy - r, cx + r, cy + r);
  SelectClipRgn(dc_, clip2);
  line(h0.x, h0.y, h1.x, h1.y, RGB(230, 230, 230));
  SelectClipRgn(dc_, nullptr);
  DeleteObject(clip2);
  circle(cx, cy, r, RGB(125, 255, 154));
  line(cx - 22, cy, cx - 8, cy, RGB(255, 210, 77));
  line(cx + 8, cy, cx + 22, cy, RGB(255, 210, 77));
}

void Hud::present() {
  GdiFlush();
  glBindTexture(GL_TEXTURE_2D, tex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w_, h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, bits_);
  glUseProgram(prog_);
  glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
  glUniform1f(glGetUniformLocation(prog_, "uBright"), brightness);
  glActiveTexture(GL_TEXTURE0);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glDisable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------- pixel-level drawing (original HUD)
void Hud::pset(int x, int y, uint32_t c) {
  if (x < cx0_ || x > cx1_ || y < cy0_ || y > cy1_ || x < 0 || y < 0 || x >= w_ || y >= h_) return;
  static_cast<uint32_t*>(bits_)[size_t(y) * w_ + x] = c;            // DIB: 0x00RRGGBB
}

void Hud::pline(float x0, float y0, float x1, float y1, uint32_t c, int thick, bool dotted) {
  float dx = x1 - x0, dy = y1 - y0;
  int n = int(std::max(std::fabs(dx), std::fabs(dy))) + 1;
  int t0 = -(thick - 1) / 2, t1 = thick / 2;
  bool steep = std::fabs(dy) > std::fabs(dx);
  for (int i = 0; i <= n; i++) {
    if (dotted && (i / std::max(1, thick)) % 3 != 0) continue;       // 1 on, 2 off (0x3015d)
    int x = int(std::lround(x0 + dx * i / n)), y = int(std::lround(y0 + dy * i / n));
    for (int k = t0; k <= t1; k++) steep ? pset(x + k, y, c) : pset(x, y + k, c);
  }
}

void Hud::pcircle(float cx, float cy, float r, uint32_t c, int thick) {
  int n = std::max(16, int(r * 7));
  for (int i = 0; i < n; i++) {
    float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
    pline(cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1), c, thick);
  }
}

void Hud::pclear(int x, int y, int w, int h) {
  for (int j = std::max(0, y); j < std::min(h_, y + h); j++)
    for (int i = std::max(0, x); i < std::min(w_, x + w); i++) static_cast<uint32_t*>(bits_)[size_t(j) * w_ + i] = 0;
}

int Hud::ptextW(const Font& f, const std::string& s, int scale) const {
  int w = 0;
  for (char ch : s) w += (f.glyph[f.index(ch)].second + 1) * scale;
  return w;
}

void Hud::ptext(const Font& f, int x, int y, const std::string& s, uint32_t c, int scale) {
  for (char ch : s) {
    auto [gx, gw] = f.glyph[f.index(ch)];
    for (int yy = 0; yy < f.h; yy++)
      for (int xx = 0; xx < gw; xx++) {
        if (f.px[size_t(yy) * f.w + gx + xx] <= 2) continue;          // background / marker
        for (int a = 0; a < scale; a++)
          for (int b = 0; b < scale; b++) pset(x + xx * scale + b, y + yy * scale + a, c);
      }
    x += (gw + 1) * scale;
  }
}
