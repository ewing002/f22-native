#include "kdv.h"

#include <cstdio>
#include <cstring>

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

bool Kdv::open(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  d_.resize(size_t(n));
  if (std::fread(d_.data(), 1, size_t(n), f) != size_t(n)) { std::fclose(f); return false; }
  std::fclose(f);
  if (d_.size() < 28 || std::memcmp(d_.data(), "xVDK", 4) != 0) return false;
  size_t hs = rd32(&d_[4]);
  w = int(rd32(&d_[8])); h = int(rd32(&d_[12])); frames = int(rd32(&d_[16]));
  uint32_t div = rd32(&d_[20]);
  fps = div ? 120.0 / div : 15.0;                          // PIT 120 Hz / divisor (8 = 15 fps, 10 = 12 fps)
  if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || (w & 3) || (h & 3)) return false;
  px_.assign(size_t(w) * h, 0);
  // audio: whole file collected up front (sVDK: two SAMP = left, right; aVDK: one SAMP = mono)
  for (size_t p = hs; p + 8 <= d_.size();) {
    uint32_t sz = rd32(&d_[p + 4]);
    if (sz < 8 || p + sz > d_.size()) break;
    if (std::memcmp(&d_[p], "sVDK", 4) == 0) {
      size_t q = p + 8;
      int k = 0;
      while (q + 16 <= p + sz) {
        uint32_t ln = rd32(&d_[q + 4]);
        if (q + 16 + ln > d_.size()) break;
        auto& dst = k == 0 ? audioL : audioR;
        dst.insert(dst.end(), d_.begin() + long(q + 16), d_.begin() + long(q + 16 + ln));
        q += 16 + ln; k++;
      }
    } else if (std::memcmp(&d_[p], "aVDK", 4) == 0 && p + 24 <= d_.size()) {
      uint32_t ln = rd32(&d_[p + 12]);
      if (p + 24 + ln <= d_.size()) audioL.insert(audioL.end(), d_.begin() + long(p + 24), d_.begin() + long(p + 24 + ln));
    }
    p += sz;
  }
  if (audioR.empty()) audioR = audioL;
  pos_ = hs;
  index = -1;
  return true;
}

bool Kdv::next() {
  while (pos_ + 8 <= d_.size()) {
    uint32_t sz = rd32(&d_[pos_ + 4]);
    if (sz < 8 || pos_ + sz > d_.size()) return false;
    const uint8_t* c = &d_[pos_];
    size_t at = pos_;
    pos_ += sz;
    if (std::memcmp(c, "pVDK", 4) == 0 && sz >= 8 + 768) {
      std::memcpy(pal_, c + 8, 768);
    } else if (std::memcmp(c, "iVDK", 4) == 0) {
      decodeFrame(c + 8, sz - 8);
      index++;
      (void)at;
      return true;
    }
  }
  return false;
}

// Per-block 2-bit command (four per byte, least significant first; the counter carries across frames):
//  0: 4 colors + 4 selector bytes (16 x 2 bit)   1: 2 colors + 16-bit mask, or (after two identical bytes) 16 raw pixels
//  2: solid-color block                          3: repeat the previous mode with the same color / palette
// Color index 0 is transparent (the pixel keeps its value from the previous frame).
void Kdv::decodeFrame(const uint8_t* data, size_t n) {
  const int W = w, H = h;
  size_t esi = 0;
  unsigned bl = 0, bh = 0;
  auto at = [&](size_t i) -> uint8_t { return i < n ? data[i] : 0; };
  auto put = [&](int x0, int y0, int i, uint8_t v) { if (v) px_[size_t(y0 + (i >> 2)) * W + size_t(x0 + (i & 3))] = v; };
  auto fill = [&](int x0, int y0, uint8_t c) { if (c) for (int i = 0; i < 16; i++) put(x0, y0, i, c); };
  auto sel4 = [&](int x0, int y0, size_t pal) {
    uint32_t s = at(esi) | at(esi + 1) << 8 | at(esi + 2) << 16 | uint32_t(at(esi + 3)) << 24;
    for (int i = 0; i < 16; i++) put(x0, y0, i, at(pal + ((s >> (2 * i)) & 3)));
    esi += 4;
  };
  auto mask2 = [&](int x0, int y0, size_t pal) {
    unsigned m = at(esi) | at(esi + 1) << 8;
    for (int i = 0; i < 16; i++) put(x0, y0, i, at(pal + ((m >> i) & 1)));
    esi += 2;
  };
  for (int by = 0; by < H / 4; by++)
    for (int bx = 0; bx < W / 4; bx++) {
      if ((bh & 3) == 0) { bl = at(esi); esi++; }
      bh++;
      int x0 = bx * 4, y0 = by * 4;
      switch (bl & 3) {
        case 3:
          if (mode_ == 2) fill(x0, y0, at(palPos_));
          else if (mode_ == 0) sel4(x0, y0, palPos_);
          else mask2(x0, y0, palPos_);
          break;
        case 2:
          mode_ = 2; palPos_ = esi; esi++;
          fill(x0, y0, at(palPos_));
          break;
        case 0:
          mode_ = 0; palPos_ = esi; esi += 4;
          sel4(x0, y0, palPos_);
          break;
        default:
          if (at(esi) == at(esi + 1)) {
            for (int i = 0; i < 16; i++) put(x0, y0, i, at(esi + 2 + size_t(i)));
            esi += 18;
          } else {
            mode_ = 1; palPos_ = esi; esi += 2;
            mask2(x0, y0, palPos_);
          }
      }
      bl >>= 2;
    }
}

void Kdv::rgba(uint32_t* out) const {
  uint32_t lut[256];
  for (int i = 0; i < 256; i++) lut[i] = 0xFF000000u | pal_[i * 3] | uint32_t(pal_[i * 3 + 1]) << 8 | uint32_t(pal_[i * 3 + 2]) << 16;
  for (size_t i = 0; i < px_.size(); i++) out[i] = lut[px_[i]];
}
