// HUD: we draw with GDI into an in-memory bitmap (black background) that is placed
// over the 3D image as a texture; black is transparent, color intensity is opacity.
#pragma once
#include <string>

#include "gl.h"

struct Font;

class Hud {
 public:
  void init();
  void begin(int w, int h);      // clear; recreates the bitmap on a size change
  void text(int x, int y, const std::string& utf8, COLORREF c, int size = 16, int align = 0);  // align: 0 left, 1 center, 2 right
  void rect(int x, int y, int w, int h, COLORREF c);
  void circle(int x, int y, int r, COLORREF c);
  void line(int x0, int y0, int x1, int y1, COLORREF c);
  void horizon(int cx, int cy, int r, float rollDeg, float pitchDeg);
  float brightness = 1.0f;       // brightness (0.2..1.0): multiplies the drawing's color and opacity
  void present();                // upload and draw
  // pixel-level drawing for the original HUD (color: 0xRRGGBB); clipping: setClip
  void setClip(int x0, int y0, int x1, int y1) { cx0_ = x0; cy0_ = y0; cx1_ = x1; cy1_ = y1; }
  void noClip() { cx0_ = 0; cy0_ = 0; cx1_ = w_ - 1; cy1_ = h_ - 1; }
  void pset(int x, int y, uint32_t c);
  void pline(float x0, float y0, float x1, float y1, uint32_t c, int thick = 1, bool dotted = false);
  void pcircle(float x, float y, float r, uint32_t c, int thick = 1);
  void pclear(int x, int y, int w, int h);
  void ptext(const Font& f, int x, int y, const std::string& s, uint32_t c, int scale);
  int ptextW(const Font& f, const std::string& s, int scale) const;
  const uint32_t* bits() const { GdiFlush(); return static_cast<const uint32_t*>(bits_); }   // 0x00RRGGBB, top to bottom
  int w() const { return w_; }
  int h() const { return h_; }
 private:
  HDC dc_ = nullptr;
  HBITMAP bmp_ = nullptr;
  void* bits_ = nullptr;
  int w_ = 0, h_ = 0;
  int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;
  GLuint tex_ = 0, prog_ = 0, vao_ = 0;
  HFONT font(int size);
  HFONT fonts_[64] = {};
};
