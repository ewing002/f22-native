#include "world.h"

#include <direct.h>

#include "imgio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool gHdTex = false;
bool gDumpTex = false;
std::string gTexDir;

// 2x upscaling: edge-preserving EPX / Scale2x on transparent (cut-out) textures, otherwise Catmull-Rom (bicubic) + mild sharpening
std::vector<uint8_t> hdUpscale(const std::vector<uint8_t>& src, int w, int h, int& ow, int& oh) {
  ow = w * 2; oh = h * 2;
  std::vector<uint8_t> out(size_t(ow) * oh * 4);
  bool keyed = false;
  for (size_t i = 3; i < src.size(); i += 4) if (src[i] < 255) { keyed = true; break; }
  auto px = [&](int x, int y) { x = x < 0 ? 0 : x >= w ? w - 1 : x; y = y < 0 ? 0 : y >= h ? h - 1 : y; return reinterpret_cast<const uint32_t*>(src.data())[size_t(y) * w + x]; };
  if (keyed) {
    uint32_t* o = reinterpret_cast<uint32_t*>(out.data());
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      uint32_t P = px(x, y), A = px(x, y - 1), B = px(x + 1, y), C = px(x - 1, y), D = px(x, y + 1);
      uint32_t p1 = P, p2 = P, p3 = P, p4 = P;
      if (C == A && C != D && A != B) p1 = A;
      if (A == B && A != C && B != D) p2 = B;
      if (D == C && D != B && C != A) p3 = C;
      if (B == D && B != A && D != C) p4 = D;
      o[size_t(2 * y) * ow + 2 * x] = p1; o[size_t(2 * y) * ow + 2 * x + 1] = p2;
      o[size_t(2 * y + 1) * ow + 2 * x] = p3; o[size_t(2 * y + 1) * ow + 2 * x + 1] = p4;
    }
    return out;
  }
  // Catmull-Rom: the two sample points of the 2x resolution within the source pixel (-0.25 / +0.25)
  static const float W0[4] = {-0.5f * 0.140625f * 0 + (-0.0351562f), 0.8398438f * 0 + 0.2851562f * 0, 0, 0};
  (void)W0;
  auto wts = [](float t, float* k) {
    float t2 = t * t, t3 = t2 * t;
    k[0] = -0.5f * t3 + t2 - 0.5f * t; k[1] = 1.5f * t3 - 2.5f * t2 + 1; k[2] = -1.5f * t3 + 2 * t2 + 0.5f * t; k[3] = 0.5f * t3 - 0.5f * t2;
  };
  float kl[4], kr[4];
  wts(0.25f, kl); wts(0.75f, kr);                       // the left / right output sample in the neighborhood [x-1 .. x+2] of the source pixel
  std::vector<float> tmp(size_t(ow) * h * 3), f(size_t(ow) * oh * 3);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int s = 0; s < 2; s++) {
    const float* k = s ? kr : kl;
    float acc[3] = {0, 0, 0};
    for (int j = 0; j < 4; j++) { uint32_t p = px(x - 1 + j, y); for (int c = 0; c < 3; c++) acc[c] += k[j] * float((p >> (8 * c)) & 255); }
    for (int c = 0; c < 3; c++) tmp[(size_t(y) * ow + 2 * x + s) * 3 + c] = acc[c];
  }
  for (int y = 0; y < h; y++) for (int s = 0; s < 2; s++) {
    const float* k = s ? kr : kl;
    for (int x = 0; x < ow; x++) {
      float acc[3] = {0, 0, 0};
      for (int j = 0; j < 4; j++) { int yy = std::min(std::max(y - 1 + j, 0), h - 1); for (int c = 0; c < 3; c++) acc[c] += k[j] * tmp[(size_t(yy) * ow + x) * 3 + c]; }
      for (int c = 0; c < 3; c++) f[(size_t(2 * y + s) * ow + x) * 3 + c] = acc[c];
    }
  }
  for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {           // sharpening: c + 0.45 * (c - 3x3 average)
    for (int c = 0; c < 3; c++) {
      float sum = 0;
      for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int xx = std::min(std::max(x + dx, 0), ow - 1), yy = std::min(std::max(y + dy, 0), oh - 1);
        sum += f[(size_t(yy) * ow + xx) * 3 + c];
      }
      float v0 = f[(size_t(y) * ow + x) * 3 + c], v = v0 + 0.45f * (v0 - sum / 9);
      out[(size_t(y) * ow + x) * 4 + c] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
    }
    out[(size_t(y) * ow + x) * 4 + 3] = 255;
  }
  return out;
}

// ============================ terrain ============================

// tile orientation code -> (transpose, horizontal mirror, vertical mirror), see f22terrain.py
static const int ORIENT[8][3] = {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1}, {1, 1, 0}, {1, 0, 1}, {1, 1, 1}, {1, 0, 0}};

static GLuint grayTexture(const Image& img, std::vector<uint8_t>* keep, float* modeOut) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, img.w, img.h, 0, GL_RED, GL_UNSIGNED_BYTE, img.px.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (keep) *keep = img.px;
  if (modeOut) {
    int hist[256] = {}, best = 0;
    for (uint8_t v : img.px) hist[v]++;
    for (int i = 1; i < 256; i++) if (hist[i] > hist[best]) best = i;
    *modeOut = float(best);
  }
  return t;
}

bool Terrain::buildTiles(const Res& res, int th, std::vector<uint8_t>& rgba) {
  char nm[32];
  std::snprintf(nm, sizeof nm, "BIN%02d.BIN", th);
  Blob bin = res.get(nm);
  if (!bin.ok()) return false;
  // tile set: 128 paletted 128x128 tiles + RGBQUAD palette
  const size_t DATA = 0x208, TRAILER = DATA + 128 * 16384;
  uint8_t pal[768];
  for (int i = 0; i < 256; i++) {
    pal[i * 3 + 0] = bin.p[TRAILER + 4 + i * 4 + 2];
    pal[i * 3 + 1] = bin.p[TRAILER + 4 + i * 4 + 1];
    pal[i * 3 + 2] = bin.p[TRAILER + 4 + i * 4 + 0];
  }
  rgba.assign(size_t(128) * 128 * 128 * 4, 255);
  for (int k = 0; k < 128; k++) {
    const uint8_t* src = bin.p + DATA + (bin.u32(4 + k * 4) & 0xFFFFFF);
    uint8_t* dst = &rgba[size_t(k) * 16384 * 4];
    for (int i = 0; i < 16384; i++) {
      dst[i * 4 + 0] = pal[src[i] * 3]; dst[i * 4 + 1] = pal[src[i] * 3 + 1];
      dst[i * 4 + 2] = pal[src[i] * 3 + 2]; dst[i * 4 + 3] = 255;
    }
  }
  return true;
}

void Terrain::uploadTiles() {
  glBindTexture(GL_TEXTURE_2D_ARRAY, tiles);
  int S = gHdTex ? 256 : 128;
  for (int w : customW) S = std::max(S, w);
  if (S > 512) S = 512;
  bool anyCustom = false;
  for (int w : customW) if (w) anyCustom = true;
  if (anyCustom) {                                      // replaced tiles: each brought to the largest size (S)
    std::vector<uint8_t> big(size_t(128) * S * S * 4);
    for (int k = 0; k < 128; k++) {
      std::vector<uint8_t> t;
      if (customW[k]) t = resizeRgba(customTile[k], customW[k], customW[k], S, S);
      else {
        std::vector<uint8_t> one(tileRgba.begin() + size_t(k) * 16384 * 4, tileRgba.begin() + size_t(k + 1) * 16384 * 4);
        int ow, oh;
        if (gHdTex) one = hdUpscale(one, 128, 128, ow, oh), t = resizeRgba(one, 256, 256, S, S);
        else t = resizeRgba(one, 128, 128, S, S);
      }
      std::memcpy(&big[size_t(k) * S * S * 4], t.data(), t.size());
    }
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, S, S, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, big.data());
  } else if (gHdTex) {
    std::vector<uint8_t> big(size_t(128) * 256 * 256 * 4);
    for (int k = 0; k < 128; k++) {
      std::vector<uint8_t> one(tileRgba.begin() + size_t(k) * 16384 * 4, tileRgba.begin() + size_t(k + 1) * 16384 * 4);
      int ow, oh;
      std::vector<uint8_t> up = hdUpscale(one, 128, 128, ow, oh);
      std::memcpy(&big[size_t(k) * 65536 * 4], up.data(), up.size());
    }
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 256, 256, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, big.data());
  } else {
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 128, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, tileRgba.data());
  }
  glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D_ARRAY, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY*/, 8.0f);
}

bool Terrain::load(const Res& res, int th, const std::string& mapName, std::string& err) {
  char nm[32];
  std::snprintf(nm, sizeof nm, "BIN%02d.BIN", th);
  Blob bin = res.get(nm);
  Blob ref = res.get(mapName + ".REF");
  if (!bin.ok() || !ref.ok() || ref.n < 24576) { err = "Missing terrain data: " + mapName; return false; }

  std::vector<uint8_t> rgba;
  buildTiles(res, th, rgba);
  tileRgba = rgba;
  theaterId = th;
  customTile.assign(128, {}); customW.assign(128, 0);
  if (!gTexDir.empty()) {
    for (int k = 0; k < 128; k++) {
      char fn[64]; std::snprintf(fn, sizeof fn, "terrain_T%d_%03d.png", th, k);
      int w, h;
      if (gDumpTex) { std::string pth = gTexDir + fn; if (FILE* t = std::fopen(pth.c_str(), "rb")) std::fclose(t); else { _mkdir(gTexDir.c_str()); savePng(pth, 128, 128, std::vector<uint8_t>(rgba.begin() + size_t(k) * 65536, rgba.begin() + size_t(k + 1) * 65536)); } }
      if (loadImageFile(gTexDir + fn, w, h, customTile[k]) && w == h) customW[k] = w; else customTile[k].clear();
    }
  }
  glGenTextures(1, &tiles);
  uploadTiles();

  // tile map: R = tile index, G = orientation bits (transp<<2 | fy<<1 | fx)
  std::vector<uint8_t> mp(128 * 128 * 2);
  for (int i = 0; i < 16384; i++) {
    int code = (ref.p[16384 + i / 2] >> ((i & 1) * 4)) & 7;
    mp[i * 2] = ref.p[i];
    mp[i * 2 + 1] = uint8_t(ORIENT[code][0] << 2 | ORIENT[code][2] << 1 | ORIENT[code][1]);
  }
  glGenTextures(1, &map);
  glBindTexture(GL_TEXTURE_2D, map);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8UI, 128, 128, 0, GL_RG_INTEGER, GL_UNSIGNED_BYTE, mp.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  Image d, s;
  std::snprintf(nm, sizeof nm, "DEPTH%02d.PCX", th);
  if (!decodePCX(res.get(nm), d) || d.w != 512 || d.h != 512) { err = std::string("Missing ") + nm; return false; }
  depth = grayTexture(d, &depthCPU, nullptr);
  std::snprintf(nm, sizeof nm, "SHADOW%02d.PCX", th);
  if (!decodePCX(res.get(nm), s)) { err = std::string("Missing ") + nm; return false; }
  shadow = grayTexture(s, nullptr, &shadowNorm);

  // grid mesh: 512x512 quads, the height is read by the vertex shader
  const int N = 512;
  std::vector<float> v;
  v.reserve(size_t(N + 1) * (N + 1) * 2);
  for (int y = 0; y <= N; y++) for (int x = 0; x <= N; x++) { v.push_back(float(x) / N); v.push_back(float(y) / N); }
  std::vector<uint32_t> idx;
  idx.reserve(size_t(N) * N * 6);
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      uint32_t a = y * (N + 1) + x, b = a + 1, c = a + N + 1, e = c + 1;
      idx.insert(idx.end(), {a, c, e, a, e, b});           // the diagonal (x, z) - (x+1, z+1), like the two triangles of 0x52796
    }
  GLuint vbo, ibo;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, v.size() * 4, v.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
  glGenBuffers(1, &ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STATIC_DRAW);
  glBindVertexArray(0);
  indexCount = int(idx.size());
  return true;
}

float Terrain::heightAt(float x, float z) const {      // interpolation of 0x52796 (touchdown and the rendered mesh use the same)
  if (depthCPU.empty()) return 0;
  double cx = double(x) / MAP_M * 512.0, cz = double(z) / MAP_M * 512.0;     // in cells (128 wu)
  int64_t xi = int64_t(std::floor(cx * 128.0)), zi = int64_t(std::floor(cz * 128.0));
  int ix = int((xi >> 7) & 511), iz = int((zi >> 7) & 511), fx = int(xi & 0x7f), fz = int(zi & 0x7f);
  const uint8_t* D = depthCPU.data();
  auto H = [&](int i2, int j2) { return int(D[(j2 & 511) * 512 + (i2 & 511)]) * 4; };
  int h;
  if (fx <= fz) {
    int h00 = H(ix, iz), h01 = H(ix, iz + 1), h11 = H(ix + 1, iz + 1);
    h = h00 + (((h01 - h00) * fz) >> 7) + (((h11 - h01) * fx) >> 7);
  } else {
    int gx = 128 - fx, gz = 128 - fz, a = H(ix + 1, iz + 1), b = H(ix + 1, iz), c = H(ix, iz);
    h = a + (((b - a) * gz) >> 7) + (((c - b) * gx) >> 7);
  }
  return float(h) / 1020.0f * HMAX_M;
}

// ============================ modellek (3DPK) ============================

GLuint Models::texture(const std::string& name, int mode) {
  std::string key = name + "#" + char('0' + mode);
  auto it = tex_.find(key);
  if (it != tex_.end()) return it->second;
  Image img;
  GLuint t = 0;
  if (decodePCX(res_.get(name), img)) {
    std::vector<uint8_t> rgba(size_t(img.w) * img.h * 4);
    for (size_t i = 0; i < img.px.size(); i++) {
      uint8_t c = img.px[i];
      if (mode == 2) {              // gray brightness map: the index itself is the value, 128 = neutral
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = c; rgba[i * 4 + 3] = 255;
      } else {
        rgba[i * 4] = img.pal[c * 3]; rgba[i * 4 + 1] = img.pal[c * 3 + 1]; rgba[i * 4 + 2] = img.pal[c * 3 + 2];
        rgba[i * 4 + 3] = (mode == 1 && c == 0) ? 0 : 255;
      }
    }
    glGenTextures(1, &t);
    Src& s = src_[key];
    s.rgba = std::move(rgba); s.w = img.w; s.h = img.h; s.mode = mode;
    if (!gTexDir.empty() && mode != 2) {                // replaceable texture: textures\<NAME>.png (or .bmp)
      std::string base = name.substr(0, name.rfind('.'));
      for (auto& c : base) c = char(std::toupper((unsigned char)c));
      std::string png = gTexDir + base + ".png", bmp = gTexDir + base + ".bmp";
      int cw, ch; std::vector<uint8_t> crgba; bool ha = false;
      if (loadImageFile(png, cw, ch, crgba, &ha) || loadImageFile(bmp, cw, ch, crgba, &ha)) {
        if (mode == 1 && !ha)                            // image without alpha: it stays transparent at the key color (the original index 0)
          for (size_t i = 0; i < size_t(cw) * ch; i++) { size_t sx = (i % cw) * img.w / cw, sy = (i / cw) * img.h / ch; crgba[i * 4 + 3] = img.px[sy * img.w + sx] == 0 ? 0 : 255; }
        s.rgba = std::move(crgba); s.w = cw; s.h = ch; s.custom = true;
      } else if (gDumpTex) {
        _mkdir(gTexDir.c_str());
        savePng(png, s.w, s.h, s.rgba);
      }
    }
    upload(t, s);
  }
  tex_[key] = t;
  return t;
}

void Models::upload(GLuint t, const Src& s) {
  const std::vector<uint8_t>* data = &s.rgba;
  std::vector<uint8_t> big;
  int w = s.w, h = s.h;
  if (gHdTex && !s.custom && w * h <= 512 * 512) { big = hdUpscale(s.rgba, s.w, s.h, w, h); data = &big; }
  glBindTexture(GL_TEXTURE_2D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data->data());
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (gHdTex || s.custom) ? GL_LINEAR : GL_NEAREST);
  if (gHdTex || s.custom) glTexParameterf(GL_TEXTURE_2D, 0x84FE, 8.0f);
}

void Models::reloadTextures() {
  for (auto& [key, s] : src_) { auto it = tex_.find(key); if (it != tex_.end() && it->second) upload(it->second, s); }
}

namespace {
struct Tex3 { std::string name; int flags, mode, frames, delay; float rgb[3]; };
struct Part {
  std::string name;
  std::vector<Tex3> tex;
  std::vector<int32_t> v;        // x,y,z,u,v (u,v 16.16)
  std::vector<float> n;          // nx,ny,nz
  std::vector<int> f;            // tex, a,b,c, na,nb,nc
  std::vector<int32_t> pts;      // attachment points x,y,z
  int32_t off[3] = {0, 0, 0};
  int32_t radius = 0;
};

std::string cstr(const uint8_t* p, int n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  s = s.substr(0, s.find('\0'));
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

Part parsePart(const Blob& b, size_t at) {
  Part P;
  P.name = cstr(b.p + at + 8, 8);
  int32_t h[19];
  for (int i = 0; i < 19; i++) h[i] = b.i32(at + 0x10 + i * 4);
  int nt = h[7], nv = h[8], nf = h[9], nn = h[10], n5 = h[11];
  P.radius = h[0];
  size_t ot = at + h[13], ov = at + h[14], of = at + h[15], on = at + h[16], o5 = at + h[17];
  for (int k = 0; k < nt; k++) {
    const uint8_t* r = b.p + ot + k * 28;
    Tex3 t;
    t.name = cstr(r, 13);
    t.flags = r[15];
    int flags2 = r[22];
    // 0x80: brightness-modulating (128 = unchanged); without a name the color gives the multiplier - e.g. the F-22 canopy is transparent, tinted glass
    t.mode = (flags2 & 0x80) ? 2 : t.name.empty() ? 0 : (flags2 & 0x10) ? 1 : 0;
    t.frames = r[25]; t.delay = r[26];
    for (int c = 0; c < 3; c++) t.rgb[c] = r[16 + c] / 255.0f;
    P.tex.push_back(t);
  }
  for (int k = 0; k < nv; k++) {
    size_t o = ov + k * 44;
    P.v.insert(P.v.end(), {b.i32(o), b.i32(o + 4), b.i32(o + 8), b.i32(o + 32), b.i32(o + 36)});
  }
  for (int k = 0; k < nn; k++) {
    size_t o = on + k * 16;
    P.n.insert(P.n.end(), {b.i32(o) / 32768.0f, b.i32(o + 4) / 32768.0f, b.i32(o + 8) / 32768.0f});
  }
  for (int k = 0; k < nf; k++) {
    size_t o = of + k * 32;
    P.f.insert(P.f.end(), {b.i32(o + 4) / 28, b.i32(o + 8) / 44, b.i32(o + 12) / 44, b.i32(o + 16) / 44,
                           b.i32(o + 20) / 16, b.i32(o + 24) / 16, b.i32(o + 28) / 16});
  }
  for (int k = 0; k < n5; k++) P.pts.insert(P.pts.end(), {b.i32(o5 + k * 12), b.i32(o5 + k * 12 + 4), b.i32(o5 + k * 12 + 8)});
  return P;
}
}  // namespace

// Animation: the game runs in 40 Hz steps, the explosion advances a frame every 5 steps (F22.EXE).
constexpr float ANIM_STEP = 5.0f / 40.0f;

Model* Models::get(const std::string& name, int lodWanted) {
  std::string key = upper(name) + "@" + std::to_string(lodWanted);
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second.get();
  Blob b = res_.get(name + ".PAK");
  if (!b.ok() || std::memcmp(b.p, "3DPK", 4) != 0) { cache_[key] = nullptr; return nullptr; }
  int nl = int(b.u32(0x28));
  int lod = lodWanted;
  if (lod < 0) {
    lod = 0;
    for (int li = 0; li < nl; li++) if (b.u16(0x40 + li * 8 + 2) >= 10) { lod = li; break; }
  }
  if (lod >= nl) lod = nl - 1;
  size_t off = b.u32(0x40 + lod * 8 + 4);
  int np = int(b.u32(off + 4));
  std::vector<Part> parts;
  for (int p = 0; p < np; p++) parts.push_back(parsePart(b, b.u32(off + 8 + p * 20)));
  // the parts after the fuselage go onto the fuselage's attachment points, in order
  for (size_t k = 1; k < parts.size(); k++)
    if ((k - 1) * 3 + 2 < parts[0].pts.size())
      for (int c = 0; c < 3; c++) parts[k].off[c] = parts[0].pts[(k - 1) * 3 + c];

  auto model = std::make_unique<Model>();
  if (!parts.empty()) model->radiusWU = parts[0].radius / 65536.0f;
  for (auto& P : parts) {
    std::map<int, std::vector<float>> byTex;
    for (size_t k = 0; k + 6 < P.f.size(); k += 7) {
      int t = P.f[k];
      auto& out = byTex[t];
      for (int c = 0; c < 3; c++) {
        int vi = P.f[k + 1 + c], ni = P.f[k + 4 + c];
        const int32_t* V = &P.v[vi * 5];
        // in-game (x, y=length, z=up) -> local (x, up, y), in meters; the nose faces -Z
        float x = (V[0] + P.off[0]) * MODEL_UNIT_M, y = (V[1] + P.off[1]) * MODEL_UNIT_M, z = (V[2] + P.off[2]) * MODEL_UNIT_M;
        float nx = 0, ny = 0, nz = 1;
        if (ni * 3 + 2 < int(P.n.size())) { nx = P.n[ni * 3]; ny = P.n[ni * 3 + 1]; nz = P.n[ni * 3 + 2]; }
        // we mirror the local X: together with the world X mirroring the model's handedness stays correct
        out.insert(out.end(), {-x, z, y, -nx, nz, ny, V[3] / 65536.0f, V[4] / 65536.0f});
        float& mn = model->partMinY.try_emplace(P.name, 1e9f).first->second;
        if (z < mn) mn = z;
      }
    }
    int partIdx = int(&P - parts.data());
    V3 pivot{-P.off[0] * MODEL_UNIT_M, P.off[2] * MODEL_UNIT_M, P.off[1] * MODEL_UNIT_M};
    for (auto& [t, data] : byTex) {
      if (t < 0 || t >= int(P.tex.size())) continue;
      const Tex3& T = P.tex[t];
      Batch B;
      B.part = P.name;
      B.texName = T.name;
      B.partIdx = partIdx;
      B.pivot = pivot;
      B.mode = T.mode;
      B.count = int(data.size() / 8);
      if (T.name.empty()) { for (int c = 0; c < 3; c++) B.color[c] = T.rgb[c]; }
      else B.tex = texture(T.name, T.mode);
      if (!T.name.empty() && !B.tex) { B.color[0] = 1; B.color[1] = 0; B.color[2] = 1; }
      B.unlit = (T.flags & 0x80) && T.name.empty() && T.mode == 0;
      if (T.frames > 1 && T.delay < 127)
        for (int k = 0; k < T.frames && t + k < int(P.tex.size()); k++) {
          const Tex3& F = P.tex[t + k];
          AnimFrame fr;
          if (F.name.empty()) for (int c = 0; c < 3; c++) fr.color[c] = F.rgb[c];
          else fr.tex = texture(F.name, T.mode);
          B.frames.push_back(fr);
        }
      GLuint vbo;
      glGenVertexArrays(1, &B.vao);
      glBindVertexArray(B.vao);
      glGenBuffers(1, &vbo);
      glBindBuffer(GL_ARRAY_BUFFER, vbo);
      glBufferData(GL_ARRAY_BUFFER, data.size() * 4, data.data(), GL_STATIC_DRAW);
      glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0);
      glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void*)12);
      glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void*)24);
      model->batches.push_back(std::move(B));
    }
  }
  glBindVertexArray(0);
  Model* raw = model.get();
  cache_[key] = std::move(model);
  return raw;
}

// ============================ drawing ============================

static const char* TVS = R"(#version 330 core
layout(location=0) in vec2 aPos;
uniform sampler2D uDepth; uniform float uHMax, uMapSize; uniform mat4 uVP; uniform vec3 uEye;
out vec2 vUV; out vec2 vCell; out float vDist;
void main() {
  vec2 uv = aPos;
  ivec2 g = ivec2(floor(uv * 512.0 + 0.5)) & 511;             // a rácspont pontos magassága (mint a 0x5271a / 0x52796: x / 128 wu)
  float h = texelFetch(uDepth, g, 0).r * uHMax;
  vec3 p = vec3(uv.x * uMapSize, h, uv.y * uMapSize) - uEye;     // kamerához képest relatív
  vUV = uv; vCell = uv * 128.0; vDist = length(p);
  gl_Position = uVP * vec4(p, 1.0);
})";
static const char* TFS = R"(#version 330 core
in vec2 vUV; in vec2 vCell; in float vDist;
uniform sampler2DArray uTiles; uniform usampler2D uMapTex; uniform sampler2D uShadow;
uniform float uShadowNorm, uFog; uniform vec3 uSky; uniform mat3 uTodM; uniform vec3 uTodC;
out vec4 o;
void main() {
  vec2 c = clamp(vCell, 0.0, 128.0 - 1e-4);
  uvec2 m = texelFetch(uMapTex, ivec2(floor(c)), 0).rg;
  vec2 f = fract(c), dx = dFdx(vCell), dy = dFdy(vCell);
  uint t = m.g;
  if ((t & 2u) != 0u) { f.y = 1.0 - f.y; dx.y = -dx.y; dy.y = -dy.y; }
  if ((t & 1u) != 0u) { f.x = 1.0 - f.x; dx.x = -dx.x; dy.x = -dy.x; }
  if ((t & 4u) != 0u) { f = f.yx; dx = dx.yx; dy = dy.yx; }
  vec3 col = textureGrad(uTiles, vec3(f, float(m.r)), dx, dy).rgb;
  col *= texture(uShadow, vUV).r * 255.0 / uShadowNorm;
  float fog = clamp(1.0 - exp(-pow(vDist / uFog, 2.0)), 0.0, 1.0);
  col = clamp(uTodM * col + uTodC, 0.0, 1.0);             // napszak (palettacsere)
  o = vec4(mix(col, uSky, fog), 1.0);
})";
static const char* MVS = R"(#version 330 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNrm; layout(location=2) in vec2 aUV;
uniform mat4 uVP; uniform vec3 uOrigin; uniform mat3 uR; uniform float uScale; uniform mat3 uPR; uniform vec3 uPivot;
uniform float uSh, uShY; uniform vec3 uShL;
out vec3 vN; out vec2 vUV; out float vDist;
void main() {
  vec3 lp = uPR * (aPos - uPivot) + uPivot;     // a rész saját forgatása a csatlakozási pontja körül
  vec3 w = uOrigin + uR * (lp * uScale);       // uOrigin már kamerához képest relatív
  if (uSh > 0.5) { vec3 L = normalize(uShL); w -= L * ((w.y - uShY) / L.y); }   // vetítés a talajsíkra
  vN = uR * (uPR * aNrm); vUV = aUV; vDist = length(w);
  gl_Position = uVP * vec4(w, 1.0);
})";
static const char* MFS = R"(#version 330 core
in vec3 vN; in vec2 vUV; in float vDist;
uniform sampler2D uTex; uniform vec3 uColor; uniform int uMode; uniform bool uHasTex, uUnlit;
uniform vec3 uSky; uniform float uFog; uniform mat3 uTodM; uniform vec3 uTodC; uniform float uSh, uShA;
out vec4 o;
void main() {
  vec4 t = uHasTex ? texture(uTex, vUV) : vec4(1.0);
  float fog = clamp(1.0 - exp(-pow(vDist / uFog, 2.0)), 0.0, 1.0);
  if (uSh > 0.5) { if (uMode == 2 || (uMode == 1 && t.a < 0.5)) discard; o = vec4(0.0, 0.0, 0.0, uShA * (1.0 - fog)); return; }
  if (uMode == 2) { o = vec4(mix(uHasTex ? t.rgb : uColor, vec3(0.5), fog), 1.0); return; }   // 2x moduláló
  if (uMode == 1 && t.a < 0.5) discard;
  vec3 n = normalize(gl_FrontFacing ? vN : -vN);
  float l = uUnlit ? 1.0 : 0.55 + 0.6 * max(dot(n, normalize(vec3(-0.5, 0.8, -0.35))), 0.0);
  o = vec4(mix(clamp(uTodM * (t.rgb * uColor * l) + uTodC, 0.0, 1.0), uSky, fog), 1.0);
})";

enum { T_DEPTH, T_HMAX, T_MAP, T_VP, T_EYE, T_TILES, T_MAPTEX, T_SHADOW, T_SNORM, T_FOG, T_SKY, T_TODM, T_TODC };
enum { M_VP, M_ORIGIN, M_R, M_SCALE, M_TEX, M_COLOR, M_MODE, M_HASTEX, M_UNLIT, M_SKY, M_FOG, M_TODM, M_TODC, M_PR, M_PIVOT, M_SH, M_SHY, M_SHL, M_SHA };

void Renderer::init() {
  tprog = makeProgram(TVS, TFS);
  const char* tn[] = {"uDepth", "uHMax", "uMapSize", "uVP", "uEye", "uTiles", "uMapTex", "uShadow", "uShadowNorm", "uFog", "uSky", "uTodM", "uTodC"};
  for (int i = 0; i < 13; i++) tu[i] = glGetUniformLocation(tprog, tn[i]);
  mprog = makeProgram(MVS, MFS);
  const char* mn[] = {"uVP", "uOrigin", "uR", "uScale", "uTex", "uColor", "uMode", "uHasTex", "uUnlit", "uSky", "uFog", "uTodM", "uTodC", "uPR", "uPivot", "uSh", "uShY", "uShL", "uShA"};
  for (int i = 0; i < 19; i++) mu[i] = glGetUniformLocation(mprog, mn[i]);
}

void Renderer::drawTerrain(const Terrain& t, const M4& vp, V3 eye, float fog) {
  glUseProgram(tprog);
  glUniformMatrix4fv(tu[T_VP], 1, GL_FALSE, vp.m);
  glUniform3f(tu[T_EYE], eye.x, eye.y, eye.z);
  glUniform1f(tu[T_HMAX], HMAX_M);
  glUniform1f(tu[T_MAP], MAP_M);
  glUniform1f(tu[T_SNORM], t.shadowNorm);
  glUniform1f(tu[T_FOG], fog);
  glUniform3f(tu[T_SKY], sky[0], sky[1], sky[2]);
  glUniformMatrix3fv(tu[T_TODM], 1, GL_TRUE, todM); glUniform3f(tu[T_TODC], todC[0], todC[1], todC[2]);
  glUniform1i(tu[T_TILES], 0); glUniform1i(tu[T_MAPTEX], 1);
  glUniform1i(tu[T_DEPTH], 2); glUniform1i(tu[T_SHADOW], 3);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, t.tiles);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, t.map);
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, t.depth);
  glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, t.shadow);
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CW);   // the world is drawn X-mirrored (vmath.h: flipX)
  glBindVertexArray(t.vao);
  glDrawElements(GL_TRIANGLES, t.indexCount, GL_UNSIGNED_INT, nullptr);
  glBindVertexArray(0);
  glDisable(GL_CULL_FACE);
  glFrontFace(GL_CCW);                         // the models' gl_FrontFacing-based shading expects this
}

void Renderer::beginModels(const M4& vp, V3, float fog) {
  glUseProgram(mprog);
  glUniformMatrix4fv(mu[M_VP], 1, GL_FALSE, vp.m);
  glUniform3f(mu[M_SKY], sky[0], sky[1], sky[2]);
  glUniformMatrix3fv(mu[M_TODM], 1, GL_TRUE, todM); glUniform3f(mu[M_TODC], todC[0], todC[1], todC[2]);
  glUniform1f(mu[M_FOG], fog);
  glUniform1f(mu[M_SH], 0);
  glUniform1i(mu[M_TEX], 0);
  glActiveTexture(GL_TEXTURE0);
}

void Renderer::drawShadow(const DrawItem& it, V3 eye) {
  if (!it.model || it.shadowY > 1e8f) return;
  glUniform1f(mu[M_SH], 1); glUniform1f(mu[M_SHY], it.shadowY - eye.y + 0.25f);
  glUniform3f(mu[M_SHL], shL[0], shL[1], shL[2]); glUniform1f(mu[M_SHA], shA);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE);
  glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-8.0f, -256.0f);
  glEnable(GL_STENCIL_TEST); glClear(GL_STENCIL_BUFFER_BIT);   // a pixel is darkened only once
  glStencilFunc(GL_NOTEQUAL, 1, 0xFF); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  DrawItem d = it; d.decal = false; d.shadowY = 1e9f; d.hide = it.hide;
  drawModel(d, eye, 0, 0);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_BLEND); glDepthMask(GL_TRUE);
  glUniform1f(mu[M_SH], 0);
}

void Renderer::drawModel(const DrawItem& it, V3 eye, int pass, float now) {
  if (!it.model) return;
  V3 o = it.pos - eye;
  if (it.decal) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-4.0f, -64.0f); }
  glUniform3f(mu[M_ORIGIN], o.x, o.y, o.z);
  glUniformMatrix3fv(mu[M_R], 1, GL_FALSE, it.R.m);
  glUniform1f(mu[M_SCALE], it.scale);
  static const float I3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  glUniformMatrix3fv(mu[M_PR], 1, GL_FALSE, I3);
  glUniform3f(mu[M_PIVOT], 0, 0, 0);
  bool posed = false;
  for (const Batch& b : it.model->batches) {
    if ((b.mode == 2) != (pass == 1)) continue;
    if (it.hide && it.hide->count(b.part)) continue;
    const PartPose* pp = it.pose && b.partIdx < int(it.pose->size()) ? &(*it.pose)[b.partIdx] : nullptr;
    if (pp && pp->hidden) continue;
    if (pp && (pp->a != 0 || pp->b != 0 || pp->c != 0)) {
      // rotation about the original axes (x span, y length, z up), converted to the local (-x, z, y) system
      // in the left-handed original a positive angle rotates the opposite way: negated angles
      float ca = std::cos(pp->a), sa = -std::sin(pp->a), cb = std::cos(pp->b), sb = -std::sin(pp->b), cc = std::cos(pp->c), sc = -std::sin(pp->c);
      float Rz[9] = {ca, -sa, 0, sa, ca, 0, 0, 0, 1}, Rx[9] = {1, 0, 0, 0, cb, -sb, 0, sb, cb}, Ry[9] = {cc, 0, sc, 0, 1, 0, -sc, 0, cc};
      auto mm = [](const float* A, const float* B, float* O) {
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) O[r * 3 + c] = A[r * 3] * B[c] + A[r * 3 + 1] * B[3 + c] + A[r * 3 + 2] * B[6 + c];
      };
      float T[9], Ro[9], L[9];
      mm(Rz, Rx, T); mm(T, Ry, Ro);
      static const int map[3] = {0, 2, 1};                // local axis -> original axis; the sign of x is -1
      static const float sgn[3] = {-1, 1, 1};
      for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) L[c * 3 + r] = sgn[r] * sgn[c] * Ro[map[r] * 3 + map[c]];   // oszlopfolytonos
      glUniformMatrix3fv(mu[M_PR], 1, GL_FALSE, L);
      glUniform3f(mu[M_PIVOT], b.pivot.x, b.pivot.y, b.pivot.z);
      posed = true;
    } else if (posed) {
      glUniformMatrix3fv(mu[M_PR], 1, GL_FALSE, I3);
      posed = false;
    }
    GLuint tex = b.tex;
    const float* col = b.color;
    if (!b.frames.empty()) {
      int k = (it.frame >= 0 ? it.frame : int(std::floor((now - it.t0) / ANIM_STEP))) % int(b.frames.size());
      if (k < 0) k += int(b.frames.size());
      tex = b.frames[k].tex;
      col = b.frames[k].color;
    }
    glUniform1i(mu[M_MODE], b.mode);
    glUniform1i(mu[M_UNLIT], b.unlit);
    glUniform3f(mu[M_COLOR], col[0], col[1], col[2]);
    glUniform1i(mu[M_HASTEX], tex != 0);
    if (tex) glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(b.vao);
    glDrawArrays(GL_TRIANGLES, 0, b.count);
  }
  glBindVertexArray(0);
  glDisable(GL_POLYGON_OFFSET_FILL);
}

// ---------------------------------------------------------------- time of day and sky
// The original is 8-bit: the time of day is given by a full palette swap (PALttnn.PCX, 0x2309c / 0x230cb).
// In region 4 the PAL04nn palette matches the tile (BIN04) palette index by index, so
// an affine color mapping can be fitted per time of day (weighted least squares, ~2 color-value
// error). We apply it to the final color in every region (terrain, models, clouds).
// The sky color is measured from the top of the screen images (PAL04nn).
static const float TOD_M[6][12] = {
    {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
    {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},                                                      // 1 day
    {0.202f, 0.021f, 0.087f, -2.646f, -0.044f, 0.237f, 0.104f, -3.593f, -0.039f, 0.015f, 0.366f, -3.063f},   // 2 night
    {0.577f, 0.014f, -0.012f, 2.204f, -0.049f, 0.559f, 0.040f, 1.103f, -0.027f, 0.003f, 0.656f, 3.550f},     // 3 dawn
    {0.386f, 0.047f, -0.101f, 18.571f, -0.032f, 0.469f, -0.105f, 18.889f, 0.041f, 0.0f, 0.295f, 18.154f},    // 4 overcast
    {1.007f, 0.064f, -0.098f, 10.779f, 0.0f, 0.610f, 0.042f, 2.982f, 0.005f, 0.025f, 0.539f, 0.811f}};      // 5 sunset
static const float TOD_SKY[6][3] = {{188, 213, 233}, {188, 213, 233}, {14, 14, 27}, {104, 105, 150}, {99, 93, 96}, {213, 138, 105}};

void Renderer::setTimeOfDay(int tod) {
  tod_ = (tod >= 1 && tod <= 5) ? tod : 1;
  {   // shadow: from the direction of the displayed sun / moon (at least 18° when low, otherwise it would be infinitely long)
    // the shadow's light is the original's light direction (0x3f6b0: (1, 1, 1)); the shadow strength is estimated per time of day
    static const float AZ[6] = {45, 45, 45, 45, 45, 45}, EL[6] = {35.26f, 35.26f, 35.26f, 35.26f, 35.26f, 35.26f}, AL[6] = {0, 0.5f, 0.3f, 0.45f, 0.2f, 0.45f};
    float az = AZ[tod_] * 3.14159265f / 180, el = EL[tod_] * 3.14159265f / 180;
    shL[0] = std::sin(az) * std::cos(el); shL[1] = std::sin(el); shL[2] = std::cos(az) * std::cos(el); shA = AL[tod_];
  }
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) todM[r * 3 + c] = TOD_M[tod_][r * 4 + c];
    todC[r] = TOD_M[tod_][r * 4 + 3] / 255.0f;
    sky[r] = TOD_SKY[tod_][r] / 255.0f;
  }
}

static const char* SVS = R"(#version 330 core
uniform mat4 uVP; uniform vec3 uEye; uniform float uH, uR, uTile;
out vec2 vUV; out vec2 vXZ;
void main() {
  vec2 q = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0;
  vec3 p = vec3(q.x * uR, uH, q.y * uR);                 // a felhősík a kamerához képest
  vUV = (p.xz + uEye.xz) / uTile; vXZ = p.xz;
  gl_Position = uVP * vec4(p, 1.0);
})";
static const char* SFS = R"(#version 330 core
in vec2 vUV; in vec2 vXZ;
uniform sampler2D uTex; uniform vec3 uSky; uniform float uFog; uniform mat3 uTodM; uniform vec3 uTodC;
out vec4 o;
void main() {
  vec3 c = clamp(uTodM * texture(uTex, vUV).rgb + uTodC, 0.0, 1.0);
  float fog = clamp(1.0 - exp(-pow(length(vXZ) / uFog, 2.0)), 0.0, 1.0);   // távolság pixelenként
  o = vec4(mix(c, uSky, fog), 1.0);
})";
static const char* BVS = R"(#version 330 core
uniform mat4 uVP; uniform vec3 uC, uRt, uUp; uniform float uS;
out vec2 vUV;
void main() {
  vec2 q = vec2(gl_VertexID & 1, gl_VertexID >> 1);
  vUV = vec2(q.x, 1.0 - q.y);
  vec3 p = uC + (uRt * (q.x * 2.0 - 1.0) + uUp * (q.y * 2.0 - 1.0)) * uS;
  gl_Position = uVP * vec4(p, 1.0);
})";
static const char* BFS = R"(#version 330 core
in vec2 vUV; uniform sampler2D uTex; uniform float uA; out vec4 o;
void main() { vec4 t = texture(uTex, vUV); if (t.a < 0.5) discard; o = vec4(t.rgb, uA); })";

static GLuint uploadRGBA(int w, int h, const std::vector<uint8_t>& rgba, bool repeat) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, 0x84FE, 8.0f);
  return t;
}

bool Renderer::initSky(const Res& res) {
  sprog = makeProgram(SVS, SFS);
  bprog = makeProgram(BVS, BFS);
  glGenVertexArrays(1, &svao);
  // SKY5.BMP: 8-bit, with its own palette (the original sky drawing routine, 0x93e7)
  Blob b = res.get("SKY5.BMP");
  if (b.ok() && b.n > 54 + 1024 && b.u16(28) == 8) {
    int w = b.i32(18), h = std::abs(b.i32(22));
    uint32_t off = b.u32(10), pal = 14 + b.u32(14);
    int stride = (w + 3) & ~3;
    if (off + size_t(stride) * h <= b.n) {
      std::vector<uint8_t> rgba(size_t(w) * h * 4);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
          uint8_t i = b.p[off + size_t(b.i32(22) > 0 ? h - 1 - y : y) * stride + x];
          uint8_t* d = &rgba[(size_t(y) * w + x) * 4];
          d[0] = b.p[pal + i * 4 + 2]; d[1] = b.p[pal + i * 4 + 1]; d[2] = b.p[pal + i * 4]; d[3] = 255;
        }
      skyTex = uploadRGBA(w, h, rgba, true);
    }
  }
  // sun and moon (SUN01.PCX, MOON64.PCX): the corner pixel's color is transparent
  const char* nm[2] = {"SUN01.PCX", "MOON64.PCX"};
  for (int k = 0; k < 2; k++) {
    Image im;
    if (!decodePCX(res.get(nm[k]), im)) continue;
    std::vector<uint8_t> rgba(size_t(im.w) * im.h * 4);
    uint8_t key = im.px[0];
    for (size_t i = 0; i < size_t(im.w) * im.h; i++) {
      uint8_t c = im.px[i];
      rgba[i * 4] = im.pal[c * 3]; rgba[i * 4 + 1] = im.pal[c * 3 + 1]; rgba[i * 4 + 2] = im.pal[c * 3 + 2];
      rgba[i * 4 + 3] = c == key ? 0 : 255;
    }
    if (k == 0) {                                   // sun: we fade the hard palette edge radially (addition; not the original edge)
      for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
          float u = (x + 0.5f) / im.w * 2 - 1, v = (y + 0.5f) / im.h * 2 - 1;
          float r = std::sqrt(u * u + v * v);
          float f = std::clamp((0.98f - r) / 0.45f, 0.0f, 1.0f);
          f = f * f * (3 - 2 * f);
          uint8_t& a = rgba[(size_t(y) * im.w + x) * 4 + 3];
          a = uint8_t(a * f);
        }
    }
    sunTex[k] = uploadRGBA(im.w, im.h, rgba, false);
  }
  return skyTex != 0;
}

void Renderer::drawSky(const M4& vp, V3 eye, V3 camRight, V3 camUp, float fog) {
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  // cloud plane at 2000 wu altitude (0x72b0 = 0x7D0); not visible above it
  float H = 2000 * WORLD_UNIT_M - eye.y;
  if (skyTex && H > 50 && skyHigh) {
    glUseProgram(sprog);
    glUniformMatrix4fv(glGetUniformLocation(sprog, "uVP"), 1, GL_FALSE, vp.m);
    glUniform3f(glGetUniformLocation(sprog, "uEye"), eye.x, eye.y, eye.z);
    glUniform1f(glGetUniformLocation(sprog, "uH"), H);
    glUniform1f(glGetUniformLocation(sprog, "uR"), 400000.0f);
    glUniform1f(glGetUniformLocation(sprog, "uTile"), 256 * 16 * WORLD_UNIT_M);   // APPROXIMATION: 16 wu / texel
    glUniform1f(glGetUniformLocation(sprog, "uFog"), fog);
    glUniform3f(glGetUniformLocation(sprog, "uSky"), sky[0], sky[1], sky[2]);
    glUniformMatrix3fv(glGetUniformLocation(sprog, "uTodM"), 1, GL_TRUE, todM);
    glUniform3f(glGetUniformLocation(sprog, "uTodC"), todC[0], todC[1], todC[2]);
    glUniform1i(glGetUniformLocation(sprog, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, skyTex);
    glBindVertexArray(svao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  // nap / hold
  // the direction in the original is (1, 1, 1) at every time of day (0x37db0: sun object position, 0x3f6b0: light direction):
  // 45° azimuth, 35.26° elevation; the disc size (degrees) is estimated
  static const float SUNDIR[6][3] = {{0, 0, 0}, {45, 35.26f, 3.0f}, {45, 35.26f, 2.5f}, {45, 35.26f, 3.5f}, {0, 0, 0}, {45, 35.26f, 3.5f}};
  const float* sd = SUNDIR[tod_];
  int tex = tod_ == 2 ? 1 : 0;
  if (sd[2] > 0 && sunTex[tex]) {
    float az = sd[0] * 3.14159265f / 180, el = sd[1] * 3.14159265f / 180, D = 5000;
    V3 c{std::sin(az) * std::cos(el) * D, std::sin(el) * D, std::cos(az) * std::cos(el) * D};
    glUseProgram(bprog);
    glUniformMatrix4fv(glGetUniformLocation(bprog, "uVP"), 1, GL_FALSE, vp.m);
    glUniform3f(glGetUniformLocation(bprog, "uC"), c.x, c.y, c.z);
    glUniform3f(glGetUniformLocation(bprog, "uRt"), camRight.x, camRight.y, camRight.z);
    glUniform3f(glGetUniformLocation(bprog, "uUp"), camUp.x, camUp.y, camUp.z);
    glUniform1f(glGetUniformLocation(bprog, "uS"), D * std::tan(sd[2] * 3.14159265f / 180));
    glUniform1f(glGetUniformLocation(bprog, "uA"), 1.0f);
    glUniform1i(glGetUniformLocation(bprog, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sunTex[tex]);
    glBindVertexArray(svao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glBindVertexArray(0);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}
