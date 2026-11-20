#include "ui.h"

#include <algorithm>
#include <cstring>

#include "sim.h"

uint32_t rgb(int r, int g, int b) { return 0xFF000000u | uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16; }

bool Pic::load(const Res& res, const std::string& name) {
  Image im;
  if (!decodePCX(res.get(name), im)) return false;
  w = im.w; h = im.h; px = std::move(im.px);
  std::memcpy(pal, im.pal, 768);
  return true;
}

// 8-bit uncompressed BMP (the font format)
static bool decodeBMP8(const Blob& b, int& w, int& h, std::vector<uint8_t>& px) {
  if (!b.ok() || b.n < 54 || b.p[0] != 'B' || b.p[1] != 'M') return false;
  uint32_t off = b.u32(10);
  w = b.i32(18);
  int hh = b.i32(22);
  if (b.u16(28) != 8 || b.u32(30) != 0) return false;
  bool bottomUp = hh > 0;
  h = std::abs(hh);
  int stride = (w + 3) & ~3;
  if (off + size_t(stride) * h > b.n) return false;
  px.assign(size_t(w) * h, 0);
  for (int y = 0; y < h; y++) {
    const uint8_t* src = b.p + off + size_t(bottomUp ? h - 1 - y : y) * stride;
    std::memcpy(&px[size_t(y) * w], src, w);
  }
  return true;
}

bool Font::load(const Res& res, const std::string& name, const sim::Exe& exe) {
  int fw, fh;
  std::vector<uint8_t> all;
  if (!decodeBMP8(res.get(name), fw, fh, all)) return false;
  // marker row: the row with the most index-1 (green) pixels; the glyphs are above it
  int mark = -1, best = 0;
  for (int y = 0; y < fh; y++) {
    int n = 0;
    for (int x = 0; x < fw; x++) n += all[size_t(y) * fw + x] == 1;
    if (n > best) { best = n; mark = y; }
  }
  if (mark <= 0) return false;
  w = fw; h = mark;
  px.assign(all.begin(), all.begin() + size_t(w) * h);
  glyph.clear();
  for (int x = 0, s = -1; x <= w; x++) {
    bool g = x < w && all[size_t(mark) * w + x] == 1;
    if (g && s < 0) s = x;
    if (!g && s >= 0) { glyph.push_back({s, x - s}); s = -1; }
  }
  // the segments of the row below the marker row are extra glyphs after the end of the main row (e.g. FATFNT: comma, apostrophe)
  if (mark + 1 < fh && !glyph.empty()) {
    int end = glyph.back().first + glyph.back().second;
    for (int x = 0, s = -1; x <= w; x++) {
      bool g = x < w && all[size_t(mark + 1) * w + x] == 1;
      if (g && s < 0) s = x;
      if (!g && s >= 0) { if (s >= end) glyph.push_back({s, x - s}); s = -1; }
    }
  }
  for (int i = 0; i < 256; i++) map[i] = exe.u8(0x41543 + i);
  return true;
}

int Font::index(char c) const {
  unsigned char u = static_cast<unsigned char>(c);
  if (u >= 'a' && u <= 'z') u -= 32;                      // the original uppercases (0x566b0)
  int i = map[u];
  if (i >= int(glyph.size())) i = 0;
  return i;
}

// ---------------------------------------------------------------- screen
void Screen::clear(uint32_t c) { std::fill(px, px + W * H, c); }

void Screen::pic(const Pic& p, int dx, int dy) {
  for (int y = 0; y < p.h; y++) {
    int ty = dy + y;
    if (ty < 0 || ty >= H) continue;
    for (int x = 0; x < p.w; x++) {
      int tx = dx + x;
      if (tx < 0 || tx >= W) continue;
      px[ty * W + tx] = p.rgba(p.px[size_t(y) * p.w + x]);
    }
  }
}

void Screen::sprite(const Pic& p, int sx, int sy, int w, int h, int dx, int dy, int key) {
  for (int y = 0; y < h; y++) {
    int ty = dy + y, py = sy + y;
    if (ty < 0 || ty >= H || py < 0 || py >= p.h) continue;
    for (int x = 0; x < w; x++) {
      int tx = dx + x, pxx = sx + x;
      if (tx < 0 || tx >= W || pxx < 0 || pxx >= p.w) continue;
      uint8_t i = p.px[size_t(py) * p.w + pxx];
      if (i == key) continue;
      px[ty * W + tx] = p.rgba(i);
    }
  }
}

void Screen::fill(int x, int y, int w, int h, uint32_t c) {
  for (int j = std::max(0, y); j < std::min(H, y + h); j++)
    for (int i = std::max(0, x); i < std::min(W, x + w); i++) px[j * W + i] = c;
}

void Screen::shade(int x, int y, int w, int h, float k) {
  for (int j = std::max(0, y); j < std::min(H, y + h); j++)
    for (int i = std::max(0, x); i < std::min(W, x + w); i++) {
      uint32_t c = px[j * W + i];
      int r = int((c & 255) * k), g = int((c >> 8 & 255) * k), b = int((c >> 16 & 255) * k);
      px[j * W + i] = rgb(r, g, b);
    }
}

void Screen::line(int x0, int y0, int x1, int y1, uint32_t c) {
  int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) {
    if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H) px[y0 * W + x0] = c;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}

uint32_t Screen::ramp(int code, int shade) const {
  code = std::clamp(code, 1, 6);
  if (!pal) {                                              // fallback colors
    static const int C[6][3] = {{0, 160, 20}, {40, 230, 40}, {255, 200, 0}, {255, 60, 0}, {200, 200, 200}, {120, 200, 255}};
    float k = 0.55f + 0.45f * std::clamp(shade, 0, 7) / 7.0f;
    return rgb(int(C[code - 1][0] * k), int(C[code - 1][1] * k), int(C[code - 1][2] * k));
  }
  int i = std::clamp(8 * code + std::clamp(shade, 0, 7), 0, 255);
  return pal->rgba(uint8_t(i));
}

int Screen::textW(const Font& f, const std::string& s) const {
  int w = 0;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '~') { i++; continue; }
    if (s[i] == '\r' || s[i] == '\n') continue;
    w += f.glyph[f.index(s[i])].second + 1;
  }
  return w;
}

void Screen::text(const Font& f, int x, int y, const std::string& s, int code, int clipW) {
  int x0 = x;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '~' && i + 1 < s.size()) { code = s[++i] - '0'; continue; }
    if (c == '\r' || c == '\n') continue;
    auto [gx, gw] = f.glyph[f.index(c)];
    if (x + gw - x0 > clipW) break;
    for (int yy = 0; yy < f.h; yy++) {
      int ty = y + yy;
      if (ty < 0 || ty >= H) continue;
      for (int xx = 0; xx < gw; xx++) {
        int tx = x + xx;
        if (tx < 0 || tx >= W) continue;
        uint8_t v = f.px[size_t(yy) * f.w + gx + xx];
        if (v <= 2) continue;                              // background, markers
        // shaded letters (9..15) get the matching step of the gradient, the rest get the brightest
        px[ty * W + tx] = ramp(code, v >= 8 && v < 16 ? v - 8 : 7);
      }
    }
    x += gw + 1;
  }
}

std::vector<std::string> Screen::wrap(const Font& f, const std::string& s, int width) {
  std::vector<std::string> out;
  std::string line, word;
  int lineW = 0, wordW = 0;
  char code = '2';
  std::string lineStartCode = "~2";
  auto flushLine = [&]() { out.push_back(lineStartCode + line); line.clear(); lineW = 0; lineStartCode = std::string("~") + code; };
  auto flushWord = [&]() {
    if (word.empty()) return;
    int spaceW = line.empty() ? 0 : f.glyph[f.index(' ')].second + 1;
    if (lineW + spaceW + wordW > width && !line.empty()) flushLine();
    if (!line.empty()) { line += ' '; lineW += spaceW; }
    line += word; lineW += wordW;
    word.clear(); wordW = 0;
  };
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '~' && i + 1 < s.size()) { code = s[i + 1]; word += c; word += s[++i]; continue; }
    if (c == '\r') continue;
    if (c == '\n') { flushWord(); flushLine(); continue; }
    if (c == ' ' || c == '\t') { flushWord(); continue; }
    word += c;
    wordW += f.glyph[f.index(c)].second + 1;
  }
  flushWord();
  if (!line.empty()) flushLine();
  return out;
}

// ---------------------------------------------------------------- drawing
void Screen::present(int winW, int winH) {
  if (!prog_) {
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    prog_ = makeProgram(R"(#version 330 core
      out vec2 vUV;
      void main() {
        vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
        vUV = vec2(p.x, 1.0 - p.y);
        gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
      })",
                        R"(#version 330 core
      in vec2 vUV; uniform sampler2D uTex; out vec4 o;
      void main() { o = vec4(texture(uTex, vUV).rgb, 1.0); })");
    glGenVertexArrays(1, &vao_);
  }
  glBindTexture(GL_TEXTURE_2D, tex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  int vw = winW, vh = winH;
  if (vw * 3 > vh * 4) vw = vh * 4 / 3; else vh = vw * 3 / 4;
  glViewport(0, 0, winW, winH);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glViewport((winW - vw) / 2, (winH - vh) / 2, vw, vh);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glUseProgram(prog_);
  glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glViewport(0, 0, winW, winH);
  glEnable(GL_DEPTH_TEST);
}

bool Screen::toScreen(int wx, int wy, int winW, int winH, int& x, int& y) const {
  int vw = winW, vh = winH;
  if (vw * 3 > vh * 4) vw = vh * 4 / 3; else vh = vw * 3 / 4;
  if (vw <= 0 || vh <= 0) return false;
  x = (wx - (winW - vw) / 2) * W / vw;
  y = (wy - (winH - vh) / 2) * H / vh;
  return x >= 0 && x < W && y >= 0 && y < H;
}
