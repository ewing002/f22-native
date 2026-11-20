// Terrain (BIN/REF/DEPTH/SHADOW), 3DPK models and mission objects (ORF).
// Format descriptions: tools/f22terrain.py and tools/f22model.py.
#pragma once
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gl.h"
#include "res.h"
#include "vmath.h"

// Units (meters). World unit (wu) = 16 ft = 4.8768 m - verified from the original HUD formulas,
// from gravity and from the thrust altitude limits; model unit = wu / 65536.
constexpr float WORLD_UNIT_M = 4.8768f;
constexpr float MODEL_UNIT_M = WORLD_UNIT_M / 65536.0f;
constexpr float CELL_M = 512 * WORLD_UNIT_M;                    // ~2,5 km
constexpr float MAP_M = 128 * CELL_M;                           // ~320 km (the world wraps around)
constexpr float HMAX_M = 255 * 4 * WORLD_UNIT_M;                // DEPTH 255 = 1020 wu (0x5271a: value x4)

// HD textures: 2x edge-preserving upscaling of the paletted textures at load time (toggleable: Options)
extern bool gHdTex;
extern bool gDumpTex;                // --dumptex: dump the original textures to PNG (into the textures folder)
extern std::string gTexDir;          // folder of replaceable textures (next to the program: textures\); empty: none
extern bool gDumpTex;                // --dumptex: dump the original textures to PNG (into the textures folder)
extern std::string gTexDir;          // folder of replaceable textures (next to the program: textures\); empty: none
std::vector<uint8_t> hdUpscale(const std::vector<uint8_t>& rgba, int w, int h, int& ow, int& oh);

struct Terrain {
  std::vector<uint8_t> tileRgba;     // the 128 128x128 tiles in original RGBA (for re-upload)
  int theaterId = 0;
  std::vector<std::vector<uint8_t>> customTile;   // replaced tiles (terrain_T<terrain>_<kkk>.png), empty = original
  std::vector<int> customW;
  void uploadTiles();                // (re)upload the tile array: according to gHdTex / replaced tiles
  static bool buildTiles(const Res& res, int th, std::vector<uint8_t>& rgba);
  GLuint tiles = 0, map = 0, depth = 0, shadow = 0, vao = 0;
  int indexCount = 0;
  float shadowNorm = 124;
  std::vector<uint8_t> depthCPU;     // 512x512
  bool load(const Res& res, int theater, const std::string& mapName, std::string& err);
  float heightAt(float x, float z) const;   // meters
};

struct AnimFrame { GLuint tex = 0; float color[3] = {1, 1, 1}; };
struct Batch {
  GLuint vao = 0;
  int count = 0;
  int mode = 0;               // 0 opaque, 1 cut-out (index 0 transparent), 2 brightness-modulating
  bool unlit = false;
  float color[3] = {1, 1, 1};
  GLuint tex = 0;
  std::string part;
  std::string texName;        // texture file name (the cockpit displays swap this)
  int partIdx = 0;            // index of the part in the LOD (the original 20-byte part record)
  V3 pivot;                   // attachment point of the part (local, m): the center of rotation
  std::vector<AnimFrame> frames;   // frames of the animated material (empty if not animated)
};
struct Model {
  std::vector<Batch> batches;
  float radiusWU = 2;                       // radius of the bounding sphere (wu) - hit box
  std::map<std::string, float> partMinY;   // lowest point per part (m)
  void overrideTexture(const std::string& name, GLuint tex) { for (auto& b : batches) if (b.texName == name && b.frames.empty()) b.tex = tex; }
};

class Models {
 public:
  explicit Models(const Res& r) : res_(r) {}
  // lod < 0: the most detailed exterior level (the F-22's level 0 is the cockpit)
  Model* get(const std::string& name, int lod = -1);
  void reloadTextures();   // after gHdTex is switched: re-upload of the already loaded textures (with the same ids)
 private:
  struct Src { std::vector<uint8_t> rgba; int w = 0, h = 0, mode = 0; bool custom = false; };
  std::map<std::string, Src> src_;
  void upload(GLuint t, const Src& s);
  GLuint texture(const std::string& name, int mode);
  const Res& res_;
  std::map<std::string, std::unique_ptr<Model>> cache_;
  std::map<std::string, GLuint> tex_;
};

// drawing
// per-part pose (angles at +4/+8/+0xC and flag at +0x10 of the original part record, 0x40bd3): a, b, c are rotations in radians
// about the original axes (z up, x span, y length); hidden = flag 0x80
struct PartPose { float a = 0, b = 0, c = 0; bool hidden = false; };
struct DrawItem { Model* model; V3 pos; M3 R; float scale = 1; float t0 = 0; const std::set<std::string>* hide = nullptr;
                  bool decal = false;    // model lying on the ground (runway): with depth offset so the terrain does not poke through it
                  const std::vector<PartPose>* pose = nullptr;
                  int frame = -1;      // >= 0: frame of the animated material
                  float shadowY = 1e9f; };   // plane of the cast shadow (meters, absolute); 1e9 = none   // >= 0: frame of the animated material (otherwise by time)   // model lying on the ground (runway): with depth offset so the terrain does not poke through it

class Renderer {
 public:
  void init();
  void drawTerrain(const Terrain& t, const M4& vp, V3 eye, float fog);
  // pass 0: opaque + cut-out, pass 1: modulating (2x multiplier blend)
  void beginModels(const M4& vp, V3 eye, float fog);
  void drawModel(const DrawItem& it, V3 eye, int pass, float now);
  void drawShadow(const DrawItem& it, V3 eye);     // flat projection of the model along the sunlight direction
  float sky[3] = {0.55f, 0.68f, 0.82f};
  // time of day (1 day, 2 night, 3 dawn, 4 overcast, 5 sunset): color mapping + sky color
  void setTimeOfDay(int tod);
  bool initSky(const Res& res);
  void drawSky(const M4& vp, V3 eye, V3 camRight, V3 camUp, float fog);
  float todM[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, todC[3] = {};
  bool skyHigh = true;               // the cloud layer (sky detail level)
  float shL[3] = {0.41f, 0.82f, -0.41f}, shA = 0.5f;   // shadow-casting direction (toward the sun) and strength, per time of day
 private:
  int tod_ = 1;
  GLuint sprog = 0, bprog = 0, svao = 0, skyTex = 0, sunTex[2] = {};
  GLuint tprog = 0, mprog = 0;
  GLint tu[24] = {}, mu[24] = {};
};
