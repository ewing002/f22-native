// Reading the RESOURCE.RES archive and PCX images (reverse-engineered formats, see tools/f22res.py).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct Blob {
  const uint8_t* p = nullptr;
  size_t n = 0;
  bool ok() const { return p != nullptr; }
  uint32_t u32(size_t o) const { return p[o] | p[o + 1] << 8 | p[o + 2] << 16 | uint32_t(p[o + 3]) << 24; }
  int32_t i32(size_t o) const { return int32_t(u32(o)); }
  uint16_t u16(size_t o) const { return uint16_t(p[o] | p[o + 1] << 8); }
};

class Res {
 public:
  bool open(const std::string& path);
  Blob get(const std::string& name) const;     // case-insensitive
  bool has(const std::string& name) const;
  std::vector<std::string> names(const std::string& ext) const;  // pl. ".REF"
 private:
  std::vector<uint8_t> data_;
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> idx_;
};

struct Image {           // 8-bit paletted image
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  uint8_t pal[768] = {};
};

bool decodePCX(const Blob& b, Image& out);
std::string upper(std::string s);
