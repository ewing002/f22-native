#include "imgio.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

bool readFile(const std::string& p, std::vector<uint8_t>& d) {
  FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > (256 << 20)) { std::fclose(f); return false; }
  d.resize(size_t(n));
  bool ok = std::fread(d.data(), 1, size_t(n), f) == size_t(n);
  std::fclose(f);
  return ok;
}

// ---- inflate (RFC 1951)
struct Bits {
  const uint8_t* p; size_t n, pos = 0; uint32_t buf = 0; int cnt = 0; bool err = false;
  uint32_t get(int k) {
    while (cnt < k) { if (pos >= n) { err = true; return 0; } buf |= uint32_t(p[pos++]) << cnt; cnt += 8; }
    uint32_t v = buf & ((1u << k) - 1); buf >>= k; cnt -= k; return v;
  }
};
struct Huff {
  uint16_t count[16] = {}, sym[320] = {};
  void build(const uint8_t* len, int n) {
    std::memset(count, 0, sizeof count);
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    uint16_t offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = uint16_t(offs[i] + count[i]);
    for (int i = 0; i < n; i++) if (len[i]) sym[offs[len[i]]++] = uint16_t(i);
  }
  int decode(Bits& b) const {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
      code |= int(b.get(1)); int c = count[l];
      if (code - c < first) return sym[index + (code - first)];
      index += c; first += c; first <<= 1; code <<= 1;
      if (b.err) return -1;
    }
    return -1;
  }
};
bool inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
  static const uint16_t LB[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint16_t LE[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t DB[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint16_t DE[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  Bits b{src, n};
  int last;
  do {
    last = int(b.get(1));
    int type = int(b.get(2));
    if (b.err) return false;
    if (type == 0) {
      b.buf = 0; b.cnt = 0;
      if (b.pos + 4 > n) return false;
      unsigned len = b.p[b.pos] | b.p[b.pos + 1] << 8;
      b.pos += 4;
      if (b.pos + len > n) return false;
      out.insert(out.end(), b.p + b.pos, b.p + b.pos + len);
      b.pos += len;
    } else if (type == 1 || type == 2) {
      Huff lit, dist;
      uint8_t len[320] = {};
      if (type == 1) {
        for (int i = 0; i < 144; i++) len[i] = 8;
        for (int i = 144; i < 256; i++) len[i] = 9;
        for (int i = 256; i < 280; i++) len[i] = 7;
        for (int i = 280; i < 288; i++) len[i] = 8;
        lit.build(len, 288);
        uint8_t dl[30]; for (int i = 0; i < 30; i++) dl[i] = 5;
        dist.build(dl, 30);
      } else {
        int nlen = int(b.get(5)) + 257, ndist = int(b.get(5)) + 1, ncode = int(b.get(4)) + 4;
        static const uint8_t ORD[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint8_t cl[19] = {};
        for (int i = 0; i < ncode; i++) cl[ORD[i]] = uint8_t(b.get(3));
        Huff ch; ch.build(cl, 19);
        int idx = 0;
        while (idx < nlen + ndist) {
          int s = ch.decode(b);
          if (s < 0 || b.err) return false;
          if (s < 16) len[idx++] = uint8_t(s);
          else {
            int rep, val = 0;
            if (s == 16) { if (!idx) return false; val = len[idx - 1]; rep = 3 + int(b.get(2)); }
            else if (s == 17) rep = 3 + int(b.get(3));
            else rep = 11 + int(b.get(7));
            if (idx + rep > nlen + ndist) return false;
            while (rep--) len[idx++] = uint8_t(val);
          }
        }
        lit.build(len, nlen);
        dist.build(len + nlen, ndist);
      }
      for (;;) {
        int s = lit.decode(b);
        if (s < 0 || b.err) return false;
        if (s < 256) out.push_back(uint8_t(s));
        else if (s == 256) break;
        else {
          s -= 257;
          if (s >= 29) return false;
          int l = LB[s] + int(b.get(LE[s]));
          int ds = dist.decode(b);
          if (ds < 0 || ds >= 30) return false;
          size_t d = DB[ds] + b.get(DE[ds]);
          if (d > out.size()) return false;
          for (int i = 0; i < l; i++) out.push_back(out[out.size() - d]);
        }
      }
    } else return false;
  } while (!last);
  return true;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool decodePng(const std::vector<uint8_t>& d, int& w, int& h, std::vector<uint8_t>& rgba, bool* hasAlpha) {
  static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  if (d.size() < 33 || std::memcmp(d.data(), SIG, 8)) return false;
  size_t p = 8;
  int depth = 0, ctype = 0, interlace = 0;
  std::vector<uint8_t> idat, pal, trns;
  while (p + 12 <= d.size()) {
    uint32_t len = be32(&d[p]);
    const uint8_t* t = &d[p + 4];
    const uint8_t* body = &d[p + 8];
    if (p + 12 + len > d.size()) return false;
    if (!std::memcmp(t, "IHDR", 4)) { w = int(be32(body)); h = int(be32(body + 4)); depth = body[8]; ctype = body[9]; interlace = body[12]; }
    else if (!std::memcmp(t, "PLTE", 4)) pal.assign(body, body + len);
    else if (!std::memcmp(t, "tRNS", 4)) trns.assign(body, body + len);
    else if (!std::memcmp(t, "IDAT", 4)) idat.insert(idat.end(), body, body + len);
    else if (!std::memcmp(t, "IEND", 4)) break;
    p += 12 + len;
  }
  if (depth != 8 || interlace || w <= 0 || h <= 0 || w > 16384 || h > 16384 || idat.size() < 6) return false;
  int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
  if (!ch) return false;
  std::vector<uint8_t> raw;
  raw.reserve(size_t(w) * h * ch + h);
  if (!inflate(idat.data() + 2, idat.size() - 2, raw)) return false;
  size_t stride = size_t(w) * ch;
  if (raw.size() < (stride + 1) * size_t(h)) return false;
  std::vector<uint8_t> img(stride * h);
  for (int y = 0; y < h; y++) {
    int f = raw[y * (stride + 1)];
    const uint8_t* in = &raw[y * (stride + 1) + 1];
    uint8_t* o = &img[y * stride];
    const uint8_t* up = y ? o - stride : nullptr;
    for (size_t x = 0; x < stride; x++) {
      int a = x >= size_t(ch) ? o[x - ch] : 0, b = up ? up[x] : 0, c = (up && x >= size_t(ch)) ? up[x - ch] : 0, v = in[x];
      switch (f) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: { int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c); v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c; break; }
        default: break;
      }
      o[x] = uint8_t(v);
    }
  }
  rgba.assign(size_t(w) * h * 4, 255);
  bool alpha = ctype == 4 || ctype == 6 || !trns.empty();
  for (size_t i = 0; i < size_t(w) * h; i++) {
    const uint8_t* s = &img[i * ch];
    uint8_t* o = &rgba[i * 4];
    switch (ctype) {
      case 0: o[0] = o[1] = o[2] = s[0]; break;
      case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; break;
      case 3: if (size_t(s[0]) * 3 + 2 < pal.size()) { o[0] = pal[s[0] * 3]; o[1] = pal[s[0] * 3 + 1]; o[2] = pal[s[0] * 3 + 2]; } if (s[0] < trns.size()) o[3] = trns[s[0]]; break;
      case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
      case 6: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
    }
  }
  if (hasAlpha) *hasAlpha = alpha;
  return true;
}

bool decodeBmp(const std::vector<uint8_t>& d, int& w, int& h, std::vector<uint8_t>& rgba, bool* hasAlpha) {
  if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
  auto r32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; };
  uint32_t off = r32(10);
  w = int(r32(18)); int hh = int(r32(22));
  int bpp = d[28] | d[29] << 8;
  bool top = hh < 0; h = top ? -hh : hh;
  if ((bpp != 24 && bpp != 32) || r32(30) != 0 && r32(30) != 3 || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
  size_t stride = (size_t(w) * bpp / 8 + 3) & ~size_t(3);
  if (off + stride * h > d.size()) return false;
  rgba.assign(size_t(w) * h * 4, 255);
  bool alpha = false;
  for (int y = 0; y < h; y++) {
    const uint8_t* s = &d[off + stride * (top ? y : h - 1 - y)];
    for (int x = 0; x < w; x++) {
      uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
      o[0] = s[x * bpp / 8 + 2]; o[1] = s[x * bpp / 8 + 1]; o[2] = s[x * bpp / 8];
      if (bpp == 32) { o[3] = s[x * 4 + 3]; if (o[3] != 255) alpha = true; }
    }
  }
  if (hasAlpha) *hasAlpha = alpha;
  return true;
}

uint32_t crcTable[256];
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xffffffffu) {
  if (!crcTable[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; crcTable[i] = v; }
  for (size_t i = 0; i < n; i++) c = crcTable[(c ^ p[i]) & 255] ^ (c >> 8);
  return c;
}
void put32(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v >> 24)); o.push_back(uint8_t(v >> 16)); o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v)); }
void chunk(std::vector<uint8_t>& o, const char* t, const std::vector<uint8_t>& body) {
  put32(o, uint32_t(body.size()));
  size_t s = o.size();
  o.insert(o.end(), t, t + 4);
  o.insert(o.end(), body.begin(), body.end());
  put32(o, crc32(&o[s], o.size() - s) ^ 0xffffffffu);
}

}  // namespace

bool loadImageFile(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba, bool* hasAlpha) {
  std::vector<uint8_t> d;
  if (!readFile(path, d)) return false;
  if (hasAlpha) *hasAlpha = false;
  return decodePng(d, w, h, rgba, hasAlpha) || decodeBmp(d, w, h, rgba, hasAlpha);
}

bool savePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
  for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4); }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  for (size_t p = 0; p < raw.size(); p += 65535) {
    size_t n = std::min<size_t>(65535, raw.size() - p);
    z.push_back(p + n >= raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
  }
  put32(z, b << 16 | a);
  std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}, ih;
  put32(ih, uint32_t(w)); put32(ih, uint32_t(h));
  ih.push_back(8); ih.push_back(6); ih.push_back(0); ih.push_back(0); ih.push_back(0);
  chunk(o, "IHDR", ih); chunk(o, "IDAT", z); chunk(o, "IEND", {});
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(o.data(), 1, o.size(), f) == o.size();
  std::fclose(f);
  return ok;
}

std::vector<uint8_t> resizeRgba(const std::vector<uint8_t>& s, int w, int h, int nw, int nh) {
  if (w == nw && h == nh) return s;
  std::vector<uint8_t> o(size_t(nw) * nh * 4);
  for (int y = 0; y < nh; y++) {
    float fy = (y + 0.5f) * h / nh - 0.5f; int y0 = int(std::floor(fy)); float ty = fy - y0;
    int ya = std::min(std::max(y0, 0), h - 1), yb = std::min(std::max(y0 + 1, 0), h - 1);
    for (int x = 0; x < nw; x++) {
      float fx = (x + 0.5f) * w / nw - 0.5f; int x0 = int(std::floor(fx)); float tx = fx - x0;
      int xa = std::min(std::max(x0, 0), w - 1), xb = std::min(std::max(x0 + 1, 0), w - 1);
      for (int c = 0; c < 4; c++) {
        float v = (s[(size_t(ya) * w + xa) * 4 + c] * (1 - tx) + s[(size_t(ya) * w + xb) * 4 + c] * tx) * (1 - ty) +
                  (s[(size_t(yb) * w + xa) * 4 + c] * (1 - tx) + s[(size_t(yb) * w + xb) * 4 + c] * tx) * ty;
        o[(size_t(y) * nw + x) * 4 + c] = uint8_t(v + 0.5f);
      }
    }
  }
  return o;
}
