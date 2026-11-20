// KDV video (NovaLogic, 'xVDK' container): decoder based on the original F22.EXE routines
// (container 0x89b6f, frame decoder 0x89f6d; format description: tools/f22kdv.py). No external library.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Kdv {
 public:
  bool open(const std::string& path);
  int w = 0, h = 0, frames = 0;
  double fps = 15;
  std::vector<uint8_t> audioL, audioR;      // 8-bit unsigned PCM, 22050 Hz (identical for mono files)
  int index = -1;                           // index of the last decoded frame
  bool next();                              // decode the next frame (false: end)
  void rgba(uint32_t* out) const;           // 0xFFBBGGRR pixelek, w * h
 private:
  void decodeFrame(const uint8_t* data, size_t n);
  std::vector<uint8_t> d_;
  size_t pos_ = 0;
  std::vector<uint8_t> px_;
  uint8_t pal_[768] = {};
  int mode_ = 0;
  size_t palPos_ = 0;
};
