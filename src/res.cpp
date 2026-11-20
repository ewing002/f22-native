#include "res.h"

#include <algorithm>
#include <cstring>
#include <fstream>

std::string upper(std::string s) {
  for (char& c : s) c = char(toupper(uint8_t(c)));
  return s;
}

// Header: "RESOURCE2xxx", u32 count, then 20-byte entries:
// name[12] (XOR with repeating key 0xAD 0xDE 0xED 0xAC), u32 offset, u32 size.
// The data starts at offset+20.
bool Res::open(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  data_.assign(std::istreambuf_iterator<char>(f), {});
  if (data_.size() < 16 || std::memcmp(data_.data(), "RESOURCE2xxx", 12) != 0) return false;
  Blob b{data_.data(), data_.size()};
  const uint8_t key[4] = {0xAD, 0xDE, 0xED, 0xAC};
  uint32_t n = b.u32(12);
  for (uint32_t i = 0; i < n; i++) {
    size_t e = 16 + size_t(i) * 20;
    char name[13] = {};
    for (int j = 0; j < 12; j++) name[j] = char(data_[e + j] ^ key[j % 4]);
    uint32_t off = b.u32(e + 12), size = b.u32(e + 16);
    if (size == 0 || off + 20 + size > data_.size()) continue;
    idx_[upper(name)] = {off + 20, size};
  }
  return !idx_.empty();
}

Blob Res::get(const std::string& name) const {
  auto it = idx_.find(upper(name));
  if (it == idx_.end()) return {};
  return {data_.data() + it->second.first, it->second.second};
}

bool Res::has(const std::string& name) const { return idx_.count(upper(name)) != 0; }

std::vector<std::string> Res::names(const std::string& ext) const {
  std::vector<std::string> out;
  for (auto& kv : idx_)
    if (kv.first.size() > ext.size() && kv.first.compare(kv.first.size() - ext.size(), ext.size(), ext) == 0)
      out.push_back(kv.first.substr(0, kv.first.size() - ext.size()));
  std::sort(out.begin(), out.end());
  return out;
}

// 8-bit, single-plane, RLE-compressed PCX; the 256-color palette is at the end of the file (marked by 0x0C).
bool decodePCX(const Blob& b, Image& img) {
  if (!b.ok() || b.n < 128 || b.p[0] != 0x0A || b.p[3] != 8 || b.p[65] != 1) return false;
  int xmin = b.u16(4), ymin = b.u16(6), xmax = b.u16(8), ymax = b.u16(10), bpl = b.u16(66);
  img.w = xmax - xmin + 1;
  img.h = ymax - ymin + 1;
  bool hasPal = b.n >= 769 && b.p[b.n - 769] == 0x0C;
  size_t end = hasPal ? b.n - 769 : b.n;
  std::vector<uint8_t> raw;
  raw.reserve(size_t(bpl) * img.h);
  size_t need = size_t(bpl) * img.h, i = 128;
  bool rle = b.p[2] == 1;
  while (raw.size() < need && i < end) {
    uint8_t c = b.p[i++];
    if (rle && c >= 0xC0) {
      uint8_t v = i < end ? b.p[i++] : 0;
      raw.insert(raw.end(), c & 0x3F, v);
    } else {
      raw.push_back(c);
    }
  }
  raw.resize(need, 0);
  img.px.resize(size_t(img.w) * img.h);
  for (int y = 0; y < img.h; y++) std::memcpy(&img.px[size_t(y) * img.w], &raw[size_t(y) * bpl], img.w);
  if (hasPal) std::memcpy(img.pal, b.p + end + 1, 768);
  else for (int v = 0; v < 256; v++) img.pal[v * 3] = img.pal[v * 3 + 1] = img.pal[v * 3 + 2] = uint8_t(v);
  return true;
}
