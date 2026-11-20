// Original 640x480 menu screens: PCX backgrounds and sprite sheets, BMP fonts
// (a green marker row gives the glyph widths), "~N" color codes taken from the
// background palette's gradients (0x419c5: mode 3, index + 8*(N-1)). The image is assembled in RGBA
// and drawn scaled proportionally (4:3).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gl.h"
#include "res.h"

namespace sim { struct Exe; }

struct Pic {                       // 8-bit paletted image (PCX)
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  uint8_t pal[768] = {};
  bool load(const Res& res, const std::string& name);
  uint32_t rgba(uint8_t i) const { return 0xFF000000u | pal[i * 3] | pal[i * 3 + 1] << 8 | uint32_t(pal[i * 3 + 2]) << 16; }
};

struct Font {                      // BMP strip: glyphs above the marker row
  int w = 0, h = 0;                // strip width, glyph height
  std::vector<uint8_t> px;         // w * h index
  std::vector<std::pair<int, int>> glyph;   // x, width
  uint8_t map[256] = {};           // ASCII -> glyph (the original 0x41543 table)
  bool load(const Res& res, const std::string& name, const sim::Exe& exe);
  int index(char c) const;
};

class Screen {
 public:
  static constexpr int W = 640, H = 480;
  uint32_t px[W * H];
  const Pic* pal = nullptr;         // background supplying the text colors
  void clear(uint32_t c = 0xFF000000u);
  void pic(const Pic& p, int dx = 0, int dy = 0);   // whole image, no transparency
  void sprite(const Pic& p, int sx, int sy, int w, int h, int dx, int dy, int key = 0);   // index 0 is transparent
  void fill(int x, int y, int w, int h, uint32_t c);
  void shade(int x, int y, int w, int h, float k);  // darkening (disabled items)
  void line(int x0, int y0, int x1, int y1, uint32_t c);
  // text: code = initial color code (1..6); ~N codes in the text switch colors
  void text(const Font& f, int x, int y, const std::string& s, int code = 2, int clipW = 9999);
  int textW(const Font& f, const std::string& s) const;
  void textC(const Font& f, int cx, int y, const std::string& s, int code = 2) { text(f, cx - textW(f, s) / 2, y, s, code); }
  // word wrap (~N codes carry over between lines; \r\n = new line)
  static std::vector<std::string> wrap(const Font& f, const std::string& s, int width);
  uint32_t ramp(int code, int shade) const;         // color code -> RGBA from the background palette

  // draw into the window (4:3, centered); mouse: window -> 640x480
  void present(int winW, int winH);
  bool toScreen(int wx, int wy, int winW, int winH, int& x, int& y) const;

 private:
  GLuint tex_ = 0, prog_ = 0, vao_ = 0;
};

uint32_t rgb(int r, int g, int b);
