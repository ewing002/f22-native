// F-22 Lightning II – native rebuild (Win32 + OpenGL 3.3, no external libraries).
//
// Data: RESOURCE.RES (terrain, models, missions) and the original program F22.EXE, decrypted in memory
// (sim::Exe::loadGameExe; a ready F22_decrypted.bin is still accepted), which supplies the flight
// model, the weapons and the AI tables. The game rules are in sim.cpp.
//
// Usage:  f22native.exe [MISSION] [--res RESOURCE.RES] [--autotest]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "audio.h"
#include "fx.h"
#include "gl.h"
#include <mmsystem.h>
#include "hud.h"
#include "kdv.h"
#include "aivm.h"
#include "aiunit.h"
#include "menu.h"
#include "res.h"
#include "sim.h"
#include "ui.h"
#include "vmath.h"
#include "world.h"
#include "imgio.h"

using sim::D3;

namespace {

float gFogDist = 260000.0f;     // view distance (setting: terrain texturing distance)
const float FOVY = 2 * std::atan(120.0f / 256.0f);   // the original focal length: 256 pixels / 240 rows
constexpr float EYE_FWD = 10.8f, EYE_UP = 2.1f;   // pilot eye in the cockpit model (m)

Res gRes;
sim::Exe gExe;
Models* gModels = nullptr;
Terrain gTerrain;
Renderer gR;
Effects gFx;
Hud gHud;
sim::World W;
std::vector<Model*> gUnitModel;
Model *mF22 = nullptr, *mCockpit = nullptr, *mMissile[6] = {}, *mBullet = nullptr, *mEx = nullptr, *mFlare = nullptr, *mChaff = nullptr;
std::string gMission = "C04M01";
bool gPaused = false, gCockpit = false, gInvert = false;
V3 gCamPos, gCamF{0, 0, -1}, gCamUp{0, 1, 0};
bool gCamInit = false;
HWND gWnd;
std::set<int> gInject;
bool gAutotest = false;
// application state: menu / flight; the menu draws the original 640x480 screens
enum AppState { APP_MENU, APP_FLIGHT };
bool gDumpTexMode = false;                           // --dumptex: dump the original textures to the textures folder
bool gForceHd = false;                                // --hd: HD textures (regardless of the saved setting)
bool gAiVm = false;                                  // --aivm: air AI driven by the original scripts (experimental)
bool gAiFlight = false;                              // --aiflight
bool gAiVmTest = false;                            // --aivmtest
std::string gVideoTest;                            // --videotest FILE: play one video with screenshots
bool gVideoActive = false, gVideoSkip = false, gNoIntro = false;   // KDV playback: any key / click skips it
AppState gApp = APP_MENU;
Menu gMenu;
Screen* gScreen = nullptr;
bool gViewTest = false, gGearTest = false, gFxTest = false, gWingTest = false, gShadowTest = false, gIlsTest = false, gCreateTest = false, gEjectTest = false;
bool gMenuTest = false, gDirect = false, gFlowTest = false, gTopdown = false, gSoundTest = false;
int gMenuClicks = 0, gTodOverride = 0;
float gTopH = 1500;
int gFlowFrame = 0;
int gMouseX = -1, gMouseY = -1;
bool gMouseDown = false, gWantStart = false, gWantEnd = false, gAnnounced = false;
int gW = 1280, gH = 720;
bool gRunning = true;

const std::set<std::string> COCKPIT_HIDE = {"F22HUD"};
const std::set<std::string> AIR_GEAR = {"WHEEL1", "WHEEL2", "WHEEL3", "WHEEL4", "WHEEL5", "WHEEL6", "WHEEL7", "WHEEL8",
                                        "WHEEL9", "WHEEL10", "MWEEL1", "MWEEL2", "MWEEL3", "MWEEL4", "MWEEL5", "MWEEL6",
                                        "MWEEL7", "MWEEL8", "MWEEL9", "MWEEL10"};

bool keyDown(int vk) {
  if (gAutotest) return gInject.count(vk) != 0;
  return GetForegroundWindow() == gWnd && (GetAsyncKeyState(vk) & 0x8000);
}
V3 toV(D3 d) { return {float(d.x), float(d.y), float(d.z)}; }
// world unit -> metre, the "image copy" nearest to the camera (the world wraps around)
V3 rpos(D3 p) {
  D3 cam{gCamPos.x / sim::WU_M, gCamPos.y / sim::WU_M, gCamPos.z / sim::WU_M};
  D3 d = sim::wdelta(p, cam);
  return gCamPos + V3{float(d.x * sim::WU_M), float(d.y * sim::WU_M), float(d.z * sim::WU_M)};
}
// The simulation steps at 40 Hz while the picture is refreshed more often: we interpolate
// between the last two steps, otherwise the aircraft would judder relative to the smoothly moving camera.
struct Snap { D3 pos; double h, p, r; };
std::vector<Snap> gPrev;
D3 gPrevF{0, 0, 1}, gPrevUp{0, 1, 0};
double gAlpha = 1;
void snapshot() {
  gPrev.resize(W.units.size());
  for (size_t i = 0; i < W.units.size(); i++) gPrev[i] = {W.units[i].pos, W.units[i].hdg, W.units[i].pitch, W.units[i].roll};
  gPrevF = W.f; gPrevUp = W.up;
}
double lerpA(double a, double b, double t) {
  double d = std::fmod(b - a + 3 * sim::PI, 2 * sim::PI) - sim::PI;
  return a + d * t;
}
Snap interp(size_t i) {
  const sim::Unit& u = W.units[i];
  if (i >= gPrev.size()) return {u.pos, u.hdg, u.pitch, u.roll};
  const Snap& s = gPrev[i];
  return {s.pos + sim::wdelta(u.pos, s.pos) * gAlpha, lerpA(s.h, u.hdg, gAlpha), lerpA(s.p, u.pitch, gAlpha),
          lerpA(s.r, u.roll, gAlpha)};
}
size_t playerIdx() { return size_t(W.player - W.units.data()); }
D3 playerPos() { return interp(playerIdx()).pos; }
D3 playerF() { return sim::norm(gPrevF + (W.f - gPrevF) * gAlpha); }
D3 playerUp() { return sim::norm(gPrevUp + (W.up - gPrevUp) * gAlpha); }

M3 attitude(double h, double p, double r) {
  D3 f, up, right;
  sim::basis(h, p, r, f, up, right);
  return modelR(toV(f), toV(up));
}

// ---------------------------------------------------------------- loading
const sim::CreateSpec* gCreateSpec = nullptr;                  // Create Mission: the template's settings (nullptr: ordinary mission)
bool loadAll(std::string& err) {
  if (gForceHd) gMenu.settings.hdTex = true;
  gHdTex = gMenu.settings.hdTex;
  W.extraUnits = !gCreateSpec && !gDirect && gMenu.extraUnits;   // [0x22c4d]: cleared after loading (0x53ddc)
  gMenu.extraUnits = false;
  if (!W.init(gRes, gExe, gMission, err, gCreateSpec)) return false;
  if (!gTerrain.load(gRes, W.theater, gMission, err)) return false;
  W.setTerrain(&gTerrain.depthCPU);
  gR.setTimeOfDay(gTodOverride ? gTodOverride : W.tod);
  if (gAiVm) W.enableAiVm(&gExe.d);
  W.restart();
  if (std::getenv("F22_TAXIDUMP"))                      // for debugging: taxi route of the player and the wingmen
    if (FILE* f = std::fopen("taxidump.txt", "w")) {
      sim::Unit& P = *W.player;
      std::fprintf(f, "%s player pos=(%.0f %.0f) hdg=%.1f taxi=%d group=%d\n", gMission.c_str(), P.pos.x, P.pos.z, P.hdg * 57.3, P.taxi, P.group);
      for (auto& u : W.units) if (u.cls == sim::C_AIR && !u.isPlayer && (u.leader == &P || u.group == P.group)) {
        std::fprintf(f, "  %s wing=%d st=%d pos=(%.0f %.0f) hdg=%.1f taxi=%d:", u.model.c_str(), u.wing, int(u.st), u.pos.x, u.pos.z, u.hdg * 57.3, u.taxi);
        if (u.taxi >= 0 && u.taxi < int(W.taxis().size())) for (auto& n : W.taxis()[u.taxi]) std::fprintf(f, " (%.0f %.0f a%d p%d)", n.x, n.z, n.action, n.param);
        std::fprintf(f, "\n");
      }
      if (P.taxi >= 0 && P.taxi < int(W.taxis().size())) { std::fprintf(f, "  player taxi:"); for (auto& n : W.taxis()[P.taxi]) std::fprintf(f, " (%.0f %.0f a%d p%d)", n.x, n.z, n.action, n.param); std::fprintf(f, "\n"); }
      std::fclose(f);
    }
  gUnitModel.clear();
  for (auto& u : W.units) gUnitModel.push_back(gModels->get(u.model));
  mF22 = gModels->get("F22");
  mCockpit = gModels->get("F22", 0);
  const char* mn[6] = {"BULLET", "SIDEWIND", "AMRAAM", "JDAM", "AMRAAM", "SIDEWIND"};
  for (int i = 0; i < 6; i++) mMissile[i] = gModels->get(mn[i]);
  mBullet = gModels->get("BULLET");
  mEx = gModels->get("EX1");
  mFlare = gModels->get("FLARE");
  mChaff = gModels->get("CHAFF");
  gFx.init({gModels->get("DEBRIS"), gModels->get("STRAIL"), gModels->get("FSMOKE"), gModels->get("FLAMES"), gModels->get("EX3"), gModels->get("POTHOLE")});
  gFx.reset();
  return true;
}
void applyRadii() {
  for (size_t i = 0; i < W.units.size(); i++) if (gUnitModel[i]) W.units[i].radius = gUnitModel[i]->radiusWU;
}

// ---------------------------------------------------------------- joystick (winmm)
// The original (0x28304, 0x614ec..0x614f8): X / Y axis = stick (±127), 3rd axis = throttle (if present), 4th = rudder
// (if present); buttons: fire, weapon, target, countermeasures, gear, flaps, brake. Dead zone 8 %.
struct Joy {
  bool present = false, hasZ = false, hasR = false, hasPov = false;
  int id = 0, nbtn = 0;
  float x = 0, y = 0, z = 0, r = 0;
  DWORD btn = 0, prevBtn = 0;
  int lastThr = -1;
  float raw(DWORD v, DWORD lo, DWORD hi) { return hi > lo ? (float(v) - lo) / float(hi - lo) : 0.5f; }
  void init() {
    JOYCAPSW c;
    for (int i = 0; i < 16; i++)
      if (joyGetDevCapsW(i, &c, sizeof c) == JOYERR_NOERROR) {
        JOYINFOEX j = {sizeof j, JOY_RETURNALL};
        if (joyGetPosEx(i, &j) != JOYERR_NOERROR) continue;
        present = true; id = i; hasZ = (c.wCaps & JOYCAPS_HASZ) != 0; hasR = (c.wCaps & JOYCAPS_HASR) != 0; hasPov = (c.wCaps & JOYCAPS_HASPOV) != 0;
        nbtn = int(c.wNumButtons);
        lo[0] = c.wXmin; hi[0] = c.wXmax; lo[1] = c.wYmin; hi[1] = c.wYmax; lo[2] = c.wZmin; hi[2] = c.wZmax; lo[3] = c.wRmin; hi[3] = c.wRmax;
        std::snprintf(name, sizeof name, "%ls", c.szPname);
        break;
      }
  }
  DWORD lo[4] = {}, hi[4] = {};
  char name[64] = {};
  static float dz(float v) { v = v * 2 - 1; float a = std::fabs(v); if (a < 0.08f) return 0; return (v < 0 ? -1.0f : 1.0f) * std::min(1.0f, (a - 0.08f) / 0.92f); }
  void poll() {
    if (!present) return;
    JOYINFOEX j = {sizeof j, JOY_RETURNALL};
    if (joyGetPosEx(id, &j) != JOYERR_NOERROR) { present = false; return; }
    x = dz(raw(j.dwXpos, lo[0], hi[0])); y = dz(raw(j.dwYpos, lo[1], hi[1]));
    z = hasZ ? raw(j.dwZpos, lo[2], hi[2]) : 0.5f; r = hasR ? dz(raw(j.dwRpos, lo[3], hi[3])) : 0;
    prevBtn = btn; btn = j.dwButtons;
  }
  bool pressed(int b) const { return (btn & (1u << b)) && !(prevBtn & (1u << b)); }
  bool down(int b) const { return (btn & (1u << b)) != 0; }
};
Joy gJoy;
bool gJoyTest = false;
void applySettings();


// ---------------------------------------------------------------- game controller (XInput: Xbox and compatible)
// XInput is loaded at run time from the system DLL (no external library).
struct XiGamepad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XiState { DWORD packet; XiGamepad pad; };
struct Pad {
  typedef DWORD(WINAPI* GetStateFn)(DWORD, XiState*);
  GetStateFn getState = nullptr;
  bool present = false;
  int idx = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
  WORD btn = 0, prev = 0;
  enum { UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8, START = 0x10, BACK = 0x20, LS = 0x40, RS = 0x80, LB = 0x100, RB = 0x200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000 };
  void init() {
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput1_3.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    if (m) getState = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
    poll();
  }
  static float stick(SHORT v) {                         // dead zone per axis 24 %
    float f = float(v) / 32767.0f, a = std::fabs(f);
    if (a < 0.24f) return 0;
    return (f < 0 ? -1.0f : 1.0f) * std::min(1.0f, (a - 0.24f) / 0.76f);
  }
  void poll() {
    if (!getState) return;
    XiState st = {};
    present = false;
    for (int k = 0; k < 4 && !present; k++) {
      int i = (idx + k) & 3;
      if (getState(DWORD(i), &st) == 0) { present = true; idx = i; }
    }
    if (!present) { btn = prev = 0; return; }
    lx = stick(st.pad.lx); ly = stick(st.pad.ly); rx = stick(st.pad.rx); ry = stick(st.pad.ry);
    lt = st.pad.lt / 255.0f; rt = st.pad.rt / 255.0f;
    prev = btn; btn = st.pad.buttons;
  }
  bool pressed(WORD b) const { return (btn & b) && !(prev & b); }
  bool down(WORD b) const { return (btn & b) != 0; }
};
Pad gPad;
int gPadRepeat = 0;

// ---------------------------------------------------------------- input -> original control record
sim::Controls readControls() {
  sim::Controls c;
  // keyboard (0x28667): ±127, "bang-bang"; grey arrows = stick, Del / PgDn = rudder.
  // With Ctrl or Alt held the arrows rotate the view, not the stick.
  bool mod = keyDown(VK_CONTROL) || keyDown(VK_MENU);
  int pitch = mod ? 0 : keyDown(VK_DOWN) - keyDown(VK_UP);
  c.stickY = pitch * 127 * (gInvert ? -1 : 1);
  c.stickX = mod ? 0 : (keyDown(VK_RIGHT) - keyDown(VK_LEFT)) * 127;
  c.rudder = (keyDown(VK_NEXT) - keyDown(VK_DELETE)) * 127;
  if (W.player && W.player->onGround && c.rudder == 0) c.rudder = c.stickX;   // taxiing: A/D also steers (convenience)
  c.fire = keyDown(VK_SPACE);
  c.brake = keyDown('B');
  if (gJoy.present && gMenu.settings.joy && !gAutotest && GetForegroundWindow() == gWnd) {
    // the stick: the axis replaces the key (the larger deflection wins)
    int jy = int(std::lround(gJoy.y * 127)) * (gInvert ? -1 : 1), jx = int(std::lround(gJoy.x * 127)), jr = int(std::lround(gJoy.r * 127));
    if (std::abs(jy) > std::abs(c.stickY)) c.stickY = jy;        // y: down (+) = pull, like the down arrow
    if (std::abs(jx) > std::abs(c.stickX)) c.stickX = jx;
    if (std::abs(jr) > std::abs(c.rudder)) c.rudder = jr;
    c.fire = c.fire || gJoy.down(0);
    c.brake = c.brake || gJoy.down(6);
  }
  if (gPad.present && gMenu.settings.joy && !gAutotest && GetForegroundWindow() == gWnd) {
    // left stick: stick (forward = nose down), right stick X: rudder, RT: fire, LT: brake
    int py = int(std::lround(-gPad.ly * 127)) * (gInvert ? -1 : 1), px = int(std::lround(gPad.lx * 127)), pr = int(std::lround(gPad.rx * 127));
    if (std::abs(py) > std::abs(c.stickY)) c.stickY = py;
    if (std::abs(px) > std::abs(c.stickX)) c.stickX = px;
    if (std::abs(pr) > std::abs(c.rudder)) c.rudder = pr;
    c.fire = c.fire || gPad.rt > 0.4f;
    c.brake = c.brake || gPad.lt > 0.4f;
  }
  return c;
}

// ---------------------------------------------------------------- views
// The original view table (0x2998c: key -> view record; 0x547d2 switch; 0x2a2c9 camera). Pressing the same
// key again steps to the next sub-view of the record. If the view's subject (wingman, missile, target)
// does not exist, the key has no effect; if it disappears midway, the cockpit view (F2) returns (0x2a305).
enum ViewMode { VM_VIRTUAL, VM_COCKPIT, VM_FULL, VM_EXTERNAL, VM_FLYBY, VM_PADLOCK, VM_MFD };
struct ViewSlot { int subj, n; ViewMode m[2]; const char* name[2]; int sub; };   // subj: 0 player, 1 wingman, 2 missile, 3 target
ViewSlot gSlots[14] = {
    {0, 1, {VM_VIRTUAL, VM_VIRTUAL}, {"PLAYER VIRTUAL", ""}, 0},                         // F1: 0x2a071
    {0, 2, {VM_COCKPIT, VM_FULL}, {"PLAYER COCKPIT", "PLAYER FULL VIEW"}, 0},          // F2: 0x2a061 (0xd01 / 0xd81)
    {0, 1, {VM_EXTERNAL, VM_EXTERNAL}, {"PLAYER EXTERNAL", ""}, 0},                     // F3: 0x29c5b
    {0, 1, {VM_FLYBY, VM_FLYBY}, {"PLAYER FLYBY", ""}, 0},                              // F4: 0x2a11d
    {0, 1, {VM_PADLOCK, VM_PADLOCK}, {"PADLOCK VIEW", ""}, 0},                          // F5: 0x2a083
    {1, 2, {VM_EXTERNAL, VM_FLYBY}, {"WINGMAN EXTERNAL", "WINGMAN FLYBY"}, 0},          // F6
    {2, 2, {VM_FLYBY, VM_EXTERNAL}, {"MISSILE FLYBY", "MISSILE EXTERNAL"}, 0},          // F7: 0x2a11d / 0x29e32
    {3, 2, {VM_EXTERNAL, VM_FLYBY}, {"TARGET EXTERNAL", "TARGET FLYBY"}, 0},            // F8
    // numeric keypad: full-screen MFD pages (0x2a196, flag 0x1108; press again to go back)
    {0, 1, {VM_MFD, VM_MFD}, {"HUD REPEATER", ""}, 0},                                  // 7
    {0, 1, {VM_MFD, VM_MFD}, {"ARTIFICIAL HORIZON", ""}, 0},                            // 9
    {0, 1, {VM_MFD, VM_MFD}, {"DEFENSE DISPLAY", ""}, 0},                               // 4
    {0, 1, {VM_MFD, VM_MFD}, {"NAVIGATION DISPLAY", ""}, 0},                            // 5
    {0, 1, {VM_MFD, VM_MFD}, {"ATTACK DISPLAY", ""}, 0},                                // 6
    {0, 1, {VM_MFD, VM_MFD}, {"STORES DISPLAY", ""}, 0}};                               // 2
int gPrevSlot = 1;
int gSlot = 1;
std::string gViewMsg;
double gViewMsgUntil = 0, gClock = 0;           // the name is shown for 1200 ticks (120 Hz: 10 s), 0x548a7
// camera angles in original angle units: 0x29a04 (sideways), 0x29a08 (up), their speeds 0x29a10 / 0x29a14;
// distance 0x29a30 (wu)
double gOrbYaw = 0, gOrbPitch = 0, gOrbYawV = 0, gOrbPitchV = 0, gOrbDist = 10;
bool gFlybySet = false;
D3 gFlybyPos;
bool gGoals = false;                            // M: mission goals ([0x531cb])
int gTimeScale = 1;                             // F9..F12: 1, 2, 4, 6x (0x531cf)

ViewMode curMode() { return gSlots[gSlot].m[gSlots[gSlot].sub]; }
bool cockpitMode(ViewMode m) { return m == VM_VIRTUAL || m == VM_COCKPIT || m == VM_FULL || m == VM_PADLOCK || m == VM_MFD; }

struct Subj { D3 pos; double h = 0, p = 0, r = 0, radius = 2; bool missile = false; };
bool subject(int s, Subj& o) {
  auto unit = [&](const sim::Unit* u) {
    if (!u || u->dead) return false;
    Snap sn = interp(size_t(u - W.units.data()));
    o.pos = sn.pos; o.h = sn.h; o.p = sn.p; o.r = sn.r; o.radius = u->radius;
    return true;
  };
  switch (s) {
    case 0:
      if (W.chute.on) { o.pos = W.chute.pos; o.h = W.chute.hdg; o.p = o.r = 0; o.radius = 2; return true; }   // after ejection: the pilot
      return unit(W.player);
    case 1:
      for (auto& u : W.units) if (u.leader == W.player && !u.dead && u.cls == sim::C_AIR) return unit(&u);
      return false;
    case 2:
      for (auto it = W.missiles.rbegin(); it != W.missiles.rend(); ++it)
        if (it->owner == W.player && !it->dead) { o.pos = it->pos; o.h = it->hdg; o.p = it->pitch; o.r = 0; o.radius = 1; o.missile = true; return true; }
      return false;
    case 3: return unit(W.lockTgt);
  }
  return false;
}

void selectView(int slot, bool announce = true) {
  Subj s;
  if (!subject(gSlots[slot].subj, s)) return;
  if (slot == gSlot && slot >= 8) { selectView(gPrevSlot < 8 ? gPrevSlot : 1, announce); return; }   // 0x1000: back
  if (slot != gSlot) gPrevSlot = gSlot;
  if (slot == gSlot && gSlots[slot].n > 1) gSlots[slot].sub = (gSlots[slot].sub + 1) % gSlots[slot].n;
  gSlot = slot;
  ViewMode m = curMode();
  if (m == VM_VIRTUAL || m == VM_PADLOCK) gOrbYaw = gOrbPitch = 0;      // 0x54c40
  else { gOrbYaw = -0x280000; gOrbPitch = 0x140000; }
  gOrbYawV = gOrbPitchV = 0;
  gFlybySet = false; gCamInit = false;
  gCockpit = cockpitMode(m);
  if (announce) { gViewMsg = gSlots[slot].name[gSlots[slot].sub]; gViewMsgUntil = gClock + 10; }
}

// per-step view control (0x537dc, 0x53bb2): Ctrl/Alt + grey arrows rotate (flag 0x10),
// S / X zoom in / out (external view, 0x800), faster with Ctrl/Alt
void viewStep() {
  ViewMode m = curMode();
  bool mod = keyDown(VK_CONTROL) || keyDown(VK_MENU);
  if (mod && (m == VM_VIRTUAL || m == VM_EXTERNAL || m == VM_PADLOCK)) {
    if (keyDown(VK_LEFT)) gOrbYawV -= 0xC00;
    if (keyDown(VK_RIGHT)) gOrbYawV += 0xC00;
    if (keyDown(VK_UP)) gOrbPitchV += 0xC00;
    if (keyDown(VK_DOWN)) gOrbPitchV -= 0xC00;
  }
  gOrbYaw += gOrbYawV; gOrbYawV -= std::trunc(gOrbYawV / 16);
  gOrbPitch += gOrbPitchV; gOrbPitchV -= std::trunc(gOrbPitchV / 16);
  gOrbPitch = std::clamp(gOrbPitch, -double(0x640000), double(0x640000));
  if (m == VM_EXTERNAL) {
    if (keyDown('S')) gOrbDist -= (0x1000 + (mod ? 0x20000 : 0)) / 65536.0;
    if (keyDown('X')) gOrbDist += (0x1000 + (mod ? 0x20000 : 0)) / 65536.0;
  }
}

void updateCamera(float dt) {
  gClock += dt;
  if (W.chute.on && (gSlot < 2 || cockpitMode(curMode()))) { gSlot = 2; gCockpit = false; }   // ejection: external view on the pilot
  if (W.crashed && !W.chute.on) {                      // crash: the camera stays in place (death cam)
    if (gCamInit) return;
    gSlot = 2; gCockpit = false;
  }
  Subj s;
  if (!subject(gSlots[gSlot].subj, s)) { selectView(1); if (!subject(0, s)) return; }
  ViewMode m = curMode();
  V3 pos = rpos(s.pos);
  auto groundClamp = [&](V3& c, float clearWU) {      // 0x29c23: at least 0.5 wu above the terrain (10 wu for a missile)
    float g = gTerrain.heightAt(std::fmod(c.x + 10 * MAP_M, MAP_M), std::fmod(c.z + 10 * MAP_M, MAP_M)) + clearWU * float(sim::WU_M);
    if (c.y < g) c.y = g;
  };
  auto lookAt = [&](V3 from, V3 to) {                 // 0x29b93: direction towards the object, no roll
    gCamPos = from;
    gCamF = norm(to - from);
    V3 u = V3{0, 1, 0} - gCamF * gCamF.y;
    gCamUp = len(u) > 1e-4f ? norm(u) : V3{0, 0, 1};
  };
  if (cockpitMode(m)) {
    V3 f = toV(playerF()), up = toV(playerUp()), rt = cross(up, f);
    gCamPos = pos + f * EYE_FWD + up * EYE_UP;
    V3 cf = f, cu = up;
    if (m == VM_VIRTUAL || m == VM_PADLOCK) {        // head rotation relative to the aircraft axes (0x29e4c)
      float a = float(gOrbYaw * sim::ANG), b = float(gOrbPitch * sim::ANG);
      V3 h = f * std::cos(a) + rt * std::sin(a);
      cf = h * std::cos(b) + up * std::sin(b);
      cu = norm(up * std::cos(b) - h * std::sin(b));
    }
    if (m == VM_PADLOCK && W.lockTgt && !W.lockTgt->dead) {   // padlock: the head follows the target (0x29f81)
      V3 want = norm(rpos(interp(size_t(W.lockTgt - W.units.data())).pos) - gCamPos);
      if (!gCamInit) gCamF = want;
      gCamF = norm(gCamF + (want - gCamF) * std::min(1.0f, dt * 6));
      V3 u = up - gCamF * dot(up, gCamF);
      gCamUp = len(u) > 1e-4f ? norm(u) : cu;
    } else { gCamF = cf; gCamUp = cu; }
    gCamInit = true;
    return;
  }
  if (m == VM_EXTERNAL) {                              // 0x29c5b: orbiting camera around the object
    gOrbDist = std::clamp(std::max(gOrbDist, s.radius + 0.5), 0.0, 104.0);   // 0x29bf5
    D3 d = sim::dirOf(s.h - gOrbYaw * sim::ANG, -gOrbPitch * sim::ANG);
    V3 c = rpos(s.pos - d * gOrbDist);
    groundClamp(c, s.missile ? 10.0f : 0.5f);
    lookAt(c, pos);
  } else {                                             // VM_FLYBY (0x2a11d / 0x2a09f): stationary camera in front of the object
    if (gFlybySet && sim::len(sim::wdelta(s.pos, gFlybyPos)) > 375) gFlybySet = false;
    if (!gFlybySet) {
      const double o = 0x30000 * sim::ANG;
      gFlybyPos = s.pos + sim::dirOf(s.h - o, s.p - o) * 300.0;
      gFlybySet = true;
    }
    V3 c = rpos(gFlybyPos);
    groundClamp(c, 0.5f);
    lookAt(c, pos);
  }
  gCamInit = true;
}

// ---------------------------------------------------------------- drawing
M4 gVP;
bool project(V3 world, int Wd, int Ht, int& sx, int& sy) {
  V3 p = world - gCamPos;
  const float* m = gVP.m;
  float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
  float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
  if (cw <= 0) return false;
  sx = int((cx / cw * 0.5f + 0.5f) * Wd);
  sy = int((0.5f - cy / cw * 0.5f) * Ht);
  return true;
}
V3 fx(V3 v) { return {-v.x, v.y, v.z}; }   // X mirroring (vmath.h: flipX): the camera directions are in mirrored space too
M3 billboard(V3 p) { V3 d = gCamPos - p; return yawMat(std::atan2(d.x, d.z)); }

void render(int Wd, int Ht) {
  glViewport(0, 0, Wd, Ht);
  glClearColor(gR.sky[0], gR.sky[1], gR.sky[2], 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  float aspect = float(Wd) / std::max(1, Ht);
  gVP = mul(perspective(FOVY, aspect, 2.5f, 420000.0f), mul(viewRot(fx(gCamF), fx(gCamUp)), flipX()));
  gR.drawSky(gVP, gCamPos, cross(gCamUp, gCamF), gCamUp, gFogDist);   // clouds, sun / moon (the terrain occludes them)
  // terrain: the neighbouring copies of the wrapping world, if within view distance
  for (int ox = -1; ox <= 1; ox++)
    for (int oz = -1; oz <= 1; oz++) {
      float x0 = ox * MAP_M, z0 = oz * MAP_M;
      float dx = std::max({x0 - gCamPos.x, 0.0f, gCamPos.x - (x0 + MAP_M)});
      float dz = std::max({z0 - gCamPos.z, 0.0f, gCamPos.z - (z0 + MAP_M)});
      if (std::hypot(dx, dz) > 300000) continue;
      gR.drawTerrain(gTerrain, gVP, gCamPos - V3{x0, 0, z0}, gFogDist);
    }

  float now = W.now;
  // the parts of the player's aircraft (per the original 0x2ad27, by LOD sub-record index):
  // 1-2 ailerons, 3-4 flaps, 5 elevator, 8 afterburner flame, 9 nose gear, 10-11 main gear,
  // 12 air brake, 13-15 open weapon bay and its doors, 17 closed weapon bay
  static std::vector<PartPose> pose(18);
  {
    const double A = sim::ANG;
    for (auto& p : pose) p = PartPose{};
    double R = W.stickX * 16384.0 * A;
    pose[1].b = float(-R); pose[1].c = float(-R / 4);
    pose[2].b = float(R); pose[2].c = float(-R / 4);
    double fb = W.flapPos * double(0x3C0000) / 64 * A, fc = W.flapPos * double(0x140000) / 64 * A;
    pose[3].b = float(fb); pose[3].c = float(fc);
    pose[4].b = float(fb); pose[4].c = float(-fc);
    pose[5].b = float(-W.stickY * 8192.0 * A);
    pose[8].hidden = (W.thrAct >> 8) <= 100;
    double g = W.gearPos * double(0x800000) / 64 * A;      // 0..90°
    pose[9].b = float(g); pose[10].c = float(g); pose[11].c = float(-g);
    pose[9].hidden = pose[10].hidden = pose[11].hidden = W.gearPos >= 64;
    pose[12].b = float(W.brakePos * -double(0x460000) / 32 * A);
    pose[12].hidden = W.brakePos == 0;
    double bay = W.bayPos * double(0x800000) / 32 * A;
    pose[14].c = float(-bay); pose[15].c = float(bay);
    pose[13].hidden = pose[14].hidden = pose[15].hidden = W.bayPos == 0;
    pose[17].hidden = W.bayPos > 0;
  }
  std::vector<DrawItem> items;
  for (size_t i = 0; i < W.units.size(); i++) {
    sim::Unit& u = W.units[i];
    if (u.dead || !gUnitModel[i] || u.isPlayer) continue;
    Snap s = interp(i);
    DrawItem it{gUnitModel[i], rpos(s.pos), attitude(s.h, s.p, s.r)};
    if (u.cls == sim::C_AIR && !u.onGround) it.hide = &AIR_GEAR;
    it.decal = u.cls == sim::C_RUNWAY;
    if (u.cls != sim::C_RUNWAY && (u.cls != sim::C_AIR || u.onGround)) {   // on the ground: the model's lowest point (the wheel sole) on the visible ground
      float lowest = 0;
      for (auto& [name, y] : gUnitModel[i]->partMinY) lowest = std::min(lowest, y);
      float gh = gTerrain.heightAt(std::fmod(it.pos.x + 10 * MAP_M, MAP_M), std::fmod(it.pos.z + 10 * MAP_M, MAP_M));
      it.pos.y = std::max(it.pos.y, gh - lowest);
    }
    if (u.cls != sim::C_RUNWAY && it.pos.y - gTerrain.heightAt(std::fmod(it.pos.x + 10 * MAP_M, MAP_M), std::fmod(it.pos.z + 10 * MAP_M, MAP_M)) < 4000)
      it.shadowY = gTerrain.heightAt(std::fmod(it.pos.x + 10 * MAP_M, MAP_M), std::fmod(it.pos.z + 10 * MAP_M, MAP_M));
    items.push_back(it);
  }
  if (!W.crashed && !gCockpit) {
    DrawItem it{mF22, rpos(playerPos()), modelR(toV(playerF()), toV(playerUp()))};
    it.pose = &pose;
    if (W.player->onGround) it.pos.y += 0.35f;   // the wheel soles are 3.89 m below the origin in the model, the aircraft origin is 3.58 m above the ground (0xBC00)
    it.shadowY = gTerrain.heightAt(std::fmod(it.pos.x + 10 * MAP_M, MAP_M), std::fmod(it.pos.z + 10 * MAP_M, MAP_M));
    items.push_back(it);
  }
  for (auto& m : W.missiles) items.push_back({mMissile[m.w], rpos(m.pos), attitude(m.hdg, m.pitch, 0), 1});
  if (W.chute.on) {                                    // ejected pilot (0x2296c / 0x22974)
    static Model* mPar[2] = {gModels->get("PAR_H00A"), gModels->get("PAR_H00B")};
    Model* pm = mPar[W.now - W.chute.t0 > 2.0f ? 1 : 0];
    if (pm) items.push_back({pm, rpos(W.chute.pos), attitude(W.chute.hdg, 0, 0), 1});
  }
  for (auto& b : W.bullets) {
    D3 d = sim::norm(b.vel);
    items.push_back({mBullet, rpos(b.pos), attitude(std::atan2(d.x, d.z), std::asin(std::clamp(d.y, -1.0, 1.0)), 0), 1});
  }
  for (auto& d : W.decoys) { V3 p = rpos(d.pos); items.push_back({d.kind ? mChaff : mFlare, p, billboard(p), 1.0f, d.born}); }
  for (auto& b : W.blasts) { V3 p = rpos(b.pos); items.push_back({mEx, p, billboard(p), b.scale, b.t0}); }
  gFx.draw(W, gUnitModel, items, [](D3 p) { return rpos(p); }, [](V3 p) { return billboard(p); },
           [](double h, double p, double r) { return attitude(h, p, r); }, &AIR_GEAR);

  // shadows (flat projection of aircraft and ground units; the cockpit has none for the own aircraft)
  gR.beginModels(gVP, gCamPos, gFogDist);
  for (auto& it : items) gR.drawModel(it, gCamPos, 0, now);
  for (auto& it : items) if (it.shadowY < 1e8f) gR.drawShadow(it, gCamPos);
  glEnable(GL_BLEND); glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR); glDepthMask(GL_FALSE);
  for (auto& it : items) gR.drawModel(it, gCamPos, 1, now);
  glDisable(GL_BLEND); glDepthMask(GL_TRUE);

  if (!W.crashed && gCockpit && curMode() != VM_FULL && mCockpit) {   // cockpit: separate depth buffer, near clip plane
    glClear(GL_DEPTH_BUFFER_BIT);
    M4 cvp = mul(perspective(FOVY, aspect, 0.1f, 160.0f), mul(viewRot(fx(gCamF), fx(gCamUp)), flipX()));
    gR.beginModels(cvp, gCamPos, 1e9f);
    DrawItem it{mCockpit, rpos(playerPos()), modelR(toV(playerF()), toV(playerUp()))};
    it.hide = &COCKPIT_HIDE;
    gR.drawModel(it, gCamPos, 0, now);
    glEnable(GL_BLEND); glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR); glDepthMask(GL_FALSE);
    gR.drawModel(it, gCamPos, 1, now);
    glDisable(GL_BLEND); glDepthMask(GL_TRUE);
  }
}

// ---------------------------------------------------------------- HUD (per the original routine 0x369ce)
// All sizes are in a 320x240 base unit, multiplied by S = image height/240 (S=2 in the original 640x480).
// The 320-wide layout is centred horizontally; the focal length is 256*S, as in the original,
// and the vertical field of view of the 3D image follows it (FOVY), so the roll scale covers the real horizon.
Font gSmall640, gSmall320;                     // SMALL640/SMALL320.BMP (the original HUD fonts)
Font gSmall;                                   // the one chosen for the current resolution
std::string hudAscii(const std::string& u8) {  // without accents, upper case (the font has nothing else)
  static const char* from[] = {"á", "é", "í", "ó", "ö", "ő", "ú", "ü", "ű", "Á", "É", "Í", "Ó", "Ö", "Ő", "Ú", "Ü", "Ű"};
  static const char to[] = "AEIOOOUUUAEIOOOUUU";
  std::string r;
  for (size_t i = 0; i < u8.size();) {
    bool hit = false;
    for (int k = 0; k < 18; k++) {
      size_t n = std::strlen(from[k]);
      if (u8.compare(i, n, from[k]) == 0) { r += to[k]; i += n; hit = true; break; }
    }
    if (hit) continue;
    char c = u8[i++];
    if (static_cast<unsigned char>(c) >= 0x80) continue;
    if (c == '(') c = '[';                       // the HUD font only has square brackets
    if (c == ')') c = ']';
    r += char(std::toupper(static_cast<unsigned char>(c)));
  }
  return r;
}
std::string exeStr(uint32_t a) {
  std::string t;
  while (a < gExe.d.size() && gExe.d[a]) t += char(gExe.d[a++]);
  return t;
}
std::string fillHash(std::string t, long v) {  // 0x56741: "###" right-aligned, padded with spaces
  size_t p = t.find('#');
  if (p == std::string::npos) return t;
  size_t q = p;
  while (q < t.size() && t[q] == '#') q++;
  std::string num = std::to_string(v);
  size_t n = q - p;
  if (num.size() < n) num = std::string(n - num.size(), ' ') + num;
  return t.substr(0, p) + num + t.substr(q);
}
std::string strip3(std::string t) {            // strips at most 3 leading spaces
  for (int i = 0; i < 3 && !t.empty() && t[0] == ' '; i++) t.erase(0, 1);
  return t;
}

// ---------------------------------------------------------------- G effect: tunnel vision, blackout, red-out
float gBlack = 0, gRed = 0;                 // 0..1 accumulated effect
void gEffectUpdate(float dt) {
  float g = W.crashed || gPaused || W.player->onGround ? 1.0f : float(W.gDisp);
  float over = std::max(0.0f, g - 5.0f), under = std::max(0.0f, -0.5f - g);
  gBlack += (over > 0 ? over * 0.35f : -0.45f) * dt;       // builds up above 5 g, then fades quickly
  gRed += (under > 0 ? under * 0.6f : -0.7f) * dt;
  if (const char* t = std::getenv("F22_GTEST")) { gBlack = float(std::atof(t)); gRed = gBlack < 0 ? -gBlack : 0; gBlack = std::max(0.0f, gBlack); }   // for verification only
  gBlack = std::clamp(gBlack, 0.0f, 1.0f); gRed = std::clamp(gRed, 0.0f, 1.0f);
}
void drawGEffect() {
  if (gBlack < 0.01f && gRed < 0.01f) return;
  static GLuint prog = 0, vao = 0;
  if (!prog) {
    prog = makeProgram(R"(#version 330 core
      out vec2 vP;
      void main() { vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); vP = p * 2.0 - 1.0; gl_Position = vec4(vP, 0.0, 1.0); })",
                       R"(#version 330 core
      in vec2 vP; uniform float uBlack, uRed; out vec4 o;
      void main() {
        float r = length(vP);                                      // 0 in the centre, ~1.4 in the corner
        float tun = smoothstep(1.25 - 1.25 * uBlack, 1.45 - 1.2 * uBlack, r);   // tunnel closing in from the edge
        float k = clamp(tun + uBlack * uBlack * 0.9, 0.0, 1.0);
        vec4 c = vec4(0.0, 0.0, 0.0, k);
        float rr = clamp(uRed * 0.75 + uRed * r * 0.25, 0.0, 0.92);
        c = mix(c, vec4(0.75, 0.02, 0.02, rr), rr > k ? 1.0 : 0.0);
        o = c;
      })");
    glGenVertexArrays(1, &vao);
  }
  glUseProgram(prog);
  glUniform1f(glGetUniformLocation(prog, "uBlack"), gBlack);
  glUniform1f(glGetUniformLocation(prog, "uRed"), gRed);
  glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindVertexArray(vao); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glBindVertexArray(0);
  glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

void drawHud(int Wd, int Ht) {
  gHud.begin(Wd, Ht);
  const float S = Ht / 240.0f, F = 256 * S, OX = Wd / 2.0f - 160 * S;
  const int TH = std::max(1, int(std::lround(S / 2)));
  // font: SMALL640 in the original 640x480 (S=2), SMALL320 in 320x240 (S=1); we pick the one that
  // is closer to the desired size at an integer magnification
  int f640 = std::max(1, int(std::lround(S / 2))), f320 = std::max(1, int(std::lround(S)));
  bool use640 = std::fabs(f640 * 2 - S) <= std::fabs(f320 - S);
  const int FS = use640 ? f640 : f320;
  gSmall = use640 ? gSmall640 : gSmall320;
  static const uint32_t RED = 0xFF0000, CYAN = 0x00FFFF, HUDC = 0x00BF00;   // 0x55ae4, 0x55aeb, HUDPAL[2]
  const float TXH = gSmall.h * FS / S;                       // font height in base units
  auto X = [&](float x) { return OX + x * S; };
  auto Y = [&](float y) { return y * S; };
  auto L = [&](float x0, float y0, float x1, float y1, uint32_t c) { gHud.pline(X(x0), Y(y0), X(x1), Y(y1), c, TH); };
  auto T = [&](float x, float y, const std::string& t, uint32_t c) { gHud.ptext(gSmall, int(X(x)), int(Y(y)), t, c, FS); };
  auto TW = [&](const std::string& t) { return gHud.ptextW(gSmall, t, FS) / S; };   // in base units
  auto TC = [&](float cx, float y, const std::string& t, uint32_t c) { T(cx - TW(t) / 2, y, t, c); };
  auto TR = [&](float rx, float y, const std::string& t, uint32_t c) { T(rx - TW(t), y, t, c); };
  auto BOX = [&](float x0, float y0, float x1, float y1) {   // readout box: the tape is erased beneath it
    gHud.pclear(int(X(x0)), int(Y(y0)), int((x1 - x0) * S) + 1, int((y1 - y0) * S) + 1);
    L(x0, y0, x1, y0, HUDC); L(x1, y0, x1, y1, HUDC); L(x1, y1, x0, y1, HUDC); L(x0, y1, x0, y0, HUDC);
  };
  char b[160];
  sim::Unit& P = *W.player;
  bool blink = (W.stepNo & 31) >= 16;

  auto TS = [&](float x, float y, const std::string& t, uint32_t c) {   // shadowed label (black, +1 pixel)
    gHud.ptext(gSmall, int(X(x)) + FS, int(Y(y)) + FS, t, 0x000000, FS);
    T(x, y, t, c);
  };
  if (gViewMsgUntil > gClock) TS(220, 10, gViewMsg, RED);   // view name (0x548a7)
  if (W.apMode && !W.crashed) TS(20, 220, W.apMode == 1 ? "AUTO LEVELING" : "AUTO PILOT", RED);   // 0x3526f
  if (!W.crashed && (curMode() == VM_COCKPIT || curMode() == VM_FULL)) {
    // image space attached to the aircraft: screen-right = cross(up, f) (left-handed world, X-mirrored image)
    D3 f = playerF(), up = playerUp(), sr = sim::cross(up, f);
    auto proj = [&](D3 d, float& sx, float& sy) {
      double bz = sim::dot(d, f);
      if (bz <= 1e-6) return false;
      sx = float(Wd / 2.0 + F * sim::dot(d, sr) / bz);
      sy = float(Ht / 2.0 - F * sim::dot(d, up) / bz);
      return true;
    };
    double hdgDeg = std::fmod(P.hdg * 180 / sim::PI + 720, 360.0), pitchDeg = P.pitch * 180 / sim::PI;

    // roll scale (0x33a03): every 10°, base-20..base+30, clip x 60..260, y 40..194
    gHud.setClip(int(X(60)), int(Y(40)), int(X(260)), int(Y(194)));
    int base = int(std::floor(pitchDeg / 10)) * 10;
    D3 lat = sim::norm(sim::cross(D3{0, 1, 0}, sim::dirOf(P.hdg, 0)));   // horizontal screen-right
    for (int a = base - 20; a <= base + 30; a += 10) {
      D3 c = sim::dirOf(P.hdg, a * sim::PI / 180) * 42.0;
      float lx, ly, rx, ry, cx0, cy0;
      if (!proj(c - lat * 5.0, lx, ly) || !proj(c + lat * 5.0, rx, ry) || !proj(c, cx0, cy0)) continue;
      int fa = a;                                           // label folded at ±90°
      while (fa > 180) fa -= 360;
      while (fa < -180) fa += 360;
      if (fa > 90) fa = 180 - fa;
      if (fa < -90) fa = -180 - fa;
      float ux = rx - lx, uy = ry - ly, ul = std::hypot(ux, uy);
      if (ul < 1) continue;
      ux /= ul; uy /= ul;
      float al = 16 * S, nx = -uy, ny = ux, hx, hy;
      // the end tick points towards the horizon
      D3 toward = sim::dirOf(P.hdg, (fa >= 0 ? a - 1 : a + 1) * sim::PI / 180) * 42.0;
      if (proj(toward, hx, hy) && (hx - cx0) * nx + (hy - cy0) * ny < 0) { nx = -nx; ny = -ny; }
      if (fa == 0) {                                        // horizon: 2 x 20 pixels, no tick
        gHud.pline(lx - ux * al / 4, ly - uy * al / 4, lx + ux * al, ly + uy * al, HUDC, TH);
        gHud.pline(rx + ux * al / 4, ry + uy * al / 4, rx - ux * al, ry - uy * al, HUDC, TH);
        continue;
      }
      bool dot = fa < 0;                                    // negative degrees: dotted
      gHud.pline(lx, ly, lx + ux * al, ly + uy * al, HUDC, TH, dot);
      gHud.pline(rx, ry, rx - ux * al, ry - uy * al, HUDC, TH, dot);
      gHud.pline(lx, ly, lx + nx * 3 * S, ly + ny * 3 * S, HUDC, TH);
      gHud.pline(rx, ry, rx + nx * 3 * S, ry + ny * 3 * S, HUDC, TH);
      std::snprintf(b, sizeof b, "%3d", fa);              // "###", not clipped
      float tw = float(gHud.ptextW(gSmall, b, FS)), th = gSmall.h * FS / 2.0f;
      gHud.ptext(gSmall, int(lx - ux * 10 * S - tw / 2), int(ly - uy * 10 * S - th), b, HUDC, FS);
      gHud.ptext(gSmall, int(rx + ux * 10 * S - tw / 2), int(ry + uy * 10 * S - th), b, HUDC, FS);
    }
    gHud.noClip();

    // heading tape (0x34227): baseline y=31, x 80..240, 2.844 pixels/degree
    L(80, 31, 240, 31, HUDC); L(160, 31, 160, 34, HUDC);
    for (int t = int(std::floor((hdgDeg - 30) / 10)) * 10; t <= hdgDeg + 30; t += 10) {
      float x = 160 + float(t - hdgDeg) * 2.844f;
      if (x < 80 || x > 240) continue;
      bool major = ((t % 20) + 20) % 20 == 0;
      L(x, major ? 28 : 29, x, 31, HUDC);
      if (major) TC(x, 22, strip3(fillHash("###", ((t % 360) + 360) % 360)), HUDC);
    }
    BOX(152, 20, 168, 28);
    {
      int h = int(std::lround(hdgDeg)) % 360;
      std::string ht = h == 0 ? "N" : h == 90 ? "E" : h == 180 ? "S" : h == 270 ? "W" : strip3(fillHash("###", h));
      TC(160, 24 - TXH / 2, ht, HUDC);
    }
    // waypoint direction indicator (0x33f04)
    const sim::Waypoint* wp = W.playerWaypoint();
    if (wp) {
      D3 d = sim::wdelta(wp->pos, P.pos);
      double brg = std::atan2(d.x, d.z) * 180 / sim::PI;
      double rel = std::remainder(brg - hdgDeg, 360.0);
      float x = 160 + float(rel) * 2.844f;
      if (x >= 80 && x <= 240) { L(x, 32, x + 4, 36, HUDC); L(x, 32, x - 4, 36, HUDC); }
      else if (x < 80) { L(78, 32, 75, 35, HUDC); L(78, 38, 75, 35, HUDC); }
      else { L(242, 32, 245, 35, HUDC); L(242, 38, 245, 35, HUDC); }
    }

    // speed tape (0x3469f): spine x=54, y 80..160; 5 pixels per 10 knots, label every 50 (knots/10)
    double K = sim::len(W.v) * 378.95;
    L(54, 80, 54, 160, HUDC); L(54, 120, 57, 120, HUDC);
    for (int v = int(K / 10) * 10 - 80; v <= K + 80; v += 10) {
      if (v < 0) continue;
      float y = 120 - float(v - K) * 0.5f;
      if (y < 80 || y > 160) continue;
      L(54, y, 51, y, HUDC);
      if (v % 50 == 0) TR(49, y - TXH / 2, std::to_string(v / 10), HUDC);
    }
    BOX(27, 116, 51, 124);
    if (K < 610) std::snprintf(b, sizeof b, "%d", int(K)); else std::snprintf(b, sizeof b, "M%.2f", K / 610);
    TC(39, 120 - TXH / 2, b, HUDC);

    // altitude tape (0x34d0c): spine x=266; 5 pixels per 100 feet, label every 500, in thousands of feet
    double ft = P.pos.y * 16, agl = (P.pos.y - W.groundWU(P.pos.x, P.pos.z)) * 16;
    L(266, 80, 266, 160, HUDC); L(266, 120, 263, 120, HUDC);
    for (int a = int(ft / 100) * 100 - 800; a <= ft + 800; a += 100) {
      if (a < 0) continue;
      float y = 120 - float(a - ft) * 0.05f;
      if (y < 80 || y > 160) continue;
      L(266, y, 269, y, HUDC);
      if (a % 500 == 0) { std::snprintf(b, sizeof b, "%.1f", a / 1000.0); T(272, y - TXH / 2, b, HUDC); }
    }
    {
      float yg = 120 + float(agl) * 0.05f;                    // ground mark (radar altitude)
      if (yg >= 80 && yg <= 160) { L(266, yg, 264, yg + 2, HUDC); L(266, yg, 264, yg - 2, HUDC); }
    }
    BOX(269, 116, 291, 124);
    TC(280, 120 - TXH / 2, std::to_string(int(ft)), HUDC);

    // ILS (not from the original): localizer (vertical) and glide slope (horizontal) indicators, with gear down
    if (!W.crashed && W.gearPos < 64) {
      auto I = W.ils();
      if (I.active) {
        float cx = Wd / 2.0f, cy = Ht / 2.0f, k = 14 * S;                 // 1° = 14 units
        float nx = cx + float(std::clamp(I.loc, -2.5, 2.5)) * k * 1.6f, ny = cy + float(std::clamp(I.gs, -2.5, 2.5)) * k * 1.6f;
        gHud.pline(nx, cy - 45 * S, nx, cy + 45 * S, HUDC, TH);
        gHud.pline(cx - 45 * S, ny, cx + 45 * S, ny, HUDC, TH);
        for (int i = -2; i <= 2; i++) if (i) {                              // scale: every 1.5°
          gHud.pline(cx + i * 22.4f * S, cy - 3 * S, cx + i * 22.4f * S, cy + 3 * S, HUDC, TH);
          gHud.pline(cx - 3 * S, cy + i * 22.4f * S, cx + 3 * S, cy + i * 22.4f * S, HUDC, TH);
        }
        char ib[40]; std::snprintf(ib, sizeof ib, "ILS %04.1f", I.nm);
        T(112, 152, ib, HUDC);
      }
    }
    // aiming: gun pipper (0x31b1c) or missile circle + diamond (0x33038)
    float ccx = Wd / 2.0f, ccy = Ht / 2.0f;
    if (W.weapon == 0) {
      double t = 47;
      // the bullet path curves against the own angular rate (on average t/2 steps of rotation) – without a target (APPROXIMATION)
      double ap = -(t / 2) * W.pitchR * sim::ANG, ay = -(t / 2) * W.yawR * sim::ANG;
      D3 lead;
      if (W.gunLead(lead)) {                              // with a target the original calculation (0x31bc0): the intercept point in the aircraft's axes
        sim::D3 rt = sim::cross(playerUp(), playerF());
        double bx = sim::dot(lead, D3{rt.x, rt.y, rt.z}), by = sim::dot(lead, D3{playerUp().x, playerUp().y, playerUp().z}), bz = sim::dot(lead, D3{playerF().x, playerF().y, playerF().z});
        if (bz > 1e-6) { ay = std::atan2(bx, bz); ap = std::atan2(by, bz); }
      }
      float px = ccx + float(F * std::tan(std::clamp(ay, -0.6, 0.6))), py = ccy - float(F * std::tan(std::clamp(ap, -0.6, 0.6)));
      gHud.pcircle(px, py, 6 * S, HUDC, TH);
      gHud.pset(int(px), int(py), HUDC);
      gHud.pline(px - 7 * S, py, px - 6 * S, py, HUDC, TH); gHud.pline(px + 7.5f * S, py, px + 6 * S, py, HUDC, TH);
      gHud.pline(px, py - 6 * S, px, py - 7 * S, HUDC, TH); gHud.pline(px, py + 6 * S, px, py + 7.5f * S, HUDC, TH);
    } else if (W.weapon == 1 || W.weapon == 2) {
      gHud.pcircle(ccx, ccy, (W.weapon == 1 ? 28 : 20) * S, HUDC, TH);
      if (W.lockState >= 1) {
        uint32_t c = W.lockState == 2 ? RED : HUDC;
        float dx = ccx + float(F * std::tan(W.curAz)), dy = ccy - float(F * std::tan(W.curEl)), d = 8 * S;
        gHud.pline(dx - d, dy, dx, dy - d, c, TH); gHud.pline(dx + d, dy, dx, dy - d, c, TH);
        gHud.pline(dx + d, dy, dx, dy + d, c, TH); gHud.pline(dx - d, dy, dx, dy + d, c, TH);
      }
    }
    // target box (0x329df) or off-screen target indicator (0x3275c)
    if (W.lockTgt && !W.lockTgt->dead) {
      uint32_t c = W.lockState == 2 ? RED : HUDC;
      D3 d = sim::wdelta(W.lockTgt->pos, P.pos);
      float tx, ty;
      bool vis = proj(d, tx, ty) && tx >= 0 && tx < Wd && ty >= 0 && ty < Ht;
      if (vis) {
        float h = 6 * S;
        gHud.pline(tx - h, ty - h, tx + h, ty - h, c, TH); gHud.pline(tx + h, ty - h, tx + h, ty + h, c, TH);
        gHud.pline(tx + h, ty + h, tx - h, ty + h, c, TH); gHud.pline(tx - h, ty + h, tx - h, ty - h, c, TH);
      } else {
        float ex = float(sim::dot(d, sr)), ey = float(-sim::dot(d, up));
        float m = std::max(std::fabs(ex) / (Wd / 2.0f - 5 * S), std::fabs(ey) / (Ht / 2.0f - 7 * S));
        if (m > 1e-6f) gHud.pcircle(ccx + ex / m, ccy + ey / m, 4 * S, HUDC, TH);
      }
    }
    gHud.pcircle(ccx, ccy, 3 * S, HUDC, TH);                  // sight circle (0x32ed4)

    // left-hand data (0x353fb), x=50
    int thr = W.thrCmd;
    if (thr >= 110) T(50, 172, "THR: BURN", HUDC); else T(50, 172, "THR: " + strip3(fillHash("###", thr)) + "%", HUDC);
    std::snprintf(b, sizeof b, "%c%04.1fG", W.gDisp < 0 ? '-' : '+', std::fabs(W.gDisp)); T(50, 180, b, HUDC);
    T(50, 188, fillHash(exeStr(0x3244d), long(W.fuel)), HUDC);
    std::snprintf(b, sizeof b, "AIRFRAME: %d%%", P.hp > 0 ? std::max(1, P.hp * 100 / P.maxhp) : 0); T(50, 196, b, HUDC);
    bool inIR = false, inRad = false;
    for (auto& m : W.missiles) if (m.target == &P && !m.dead) (W.wdef[m.w].seeker == 1 ? inIR : inRad) = true;
    if (inRad) T(50, 204, "CHAFF", RED);
    if (inIR) T(76, 204, "FLARE", RED);
    // waypoint data (0x357db)
    if (wp) {
      T(50, 46, fillHash(exeStr(0x32465), W.playerWp + 1), HUDC);
      std::string desc = "FOLLOW ROUTE";
      if (wp->action == 3)
        for (auto& u : W.units) if (u.id == wp->param) { desc = "ATTACK " + upper(u.model); break; }
      if (wp->action == 2 || wp->action == 4) desc = "ATTACK FLT GROUP";
      T(50, 54, desc, HUDC);
      D3 d = sim::wdelta(wp->pos, P.pos);
      std::snprintf(b, sizeof b, "RANGE: %05.2f", std::hypot(d.x, d.z) / 380); T(50, 62, b, HUDC);
    }
    // right-hand target data (0x3537a, 0x35937), right-aligned at x=274
    if (W.lockTgt && !W.lockTgt->dead) {
      sim::Unit& t = *W.lockTgt;
      bool air = t.cls == sim::C_AIR && !t.onGround;
      TR(274, 172, exeStr(uint32_t(gExe.i32(0x320aa + (air ? 2 : 5) * 4))), HUDC);
      TR(274, 180, upper(t.model), HUDC);
      D3 d = sim::wdelta(t.pos, P.pos);
      double r = sim::len(d), rn = sim::len(sim::wdelta(t.pos + t.fwd() * t.speed, P.pos + W.v));
      std::snprintf(b, sizeof b, "VC: %5d", int((r - rn) * 378.95)); TR(274, 188, b, HUDC);
      std::snprintf(b, sizeof b, "RANGE: %05.2f", r / 380); TR(274, 196, b, HUDC);
      std::snprintf(b, sizeof b, "AIRFRAME: %d%%", std::max(1, t.hp * 100 / std::max(1, t.maxhp))); TR(274, 204, b, HUDC);
    }
    // weapon (0x35c0a) and status words (0x350ad)
    TC(160, 196, fillHash(exeStr(uint32_t(gExe.i32(0x32092 + W.weapon * 4))), W.ammo[W.weapon]), HUDC);
    if (!W.gearWantUp) TC(121, 204, "GEAR", HUDC);
    if (W.flapsWant) TC(147, 204, "FLAPS", HUDC);
    if (W.brakePos) TC(173, 204, "BRAKE", HUDC);
    if (W.radarOn) TC(199, 204, "RADAR", HUDC);
    // stall warning (0x35d29)
    if (W.stallWarn && blink) TC(160, 80, W.stallWarn == 2 ? "--STALL--" : "STALL WARNING", RED);
  }
  // mission caption (0x36616)
  if (W.goalDone[3] > 0 || W.crashed) T(10, 6, exeStr(0x22316), RED);
  else if (W.primaryDone()) T(10, 6, exeStr(0x222ae), CYAN);
  // mission goals (M, [0x531cb] -> 0x2431e): title by mission state, then PRIMARY / SECONDARY / BONUS GOALS
  // and IMMEDIATE MISSION FAILURES; per line the text of the goal triggers (reply 3) (0x23e2d / 0x240bc)
  if (gGoals && !W.crashed) {
    static const char* AIRN[24] = {"ALPHA", "BETA", "DELTA", "EPSILON", "GAMMA", "OMEGA", "THETA", "STORM", "SKULL", "ZOMBIE", "VAMPIRE", "GHOST",
                                   "DRAGON", "VIPER", "COBRA", "MULE", "BANSHEE", "LAMBDA", "OMICRON", "SIGMA", "PSI", "KAPPA", "ZETA", "VICTORY"};
    static const char* GRN[35] = {"BRAVO", "BARSTOW", "OAK", "SHEPHERD", "RILEY", "SAGEBRUSH", "MALONE", "CACTUS", "UTAH", "LOGAN", "PRESTON", "CLAYTON",
                                  "TURNER", "MADISON", "TALBOT", "TYLER", "DREW", "RYAN", "HEMINGWAY", "DRACO", "HYDRA", "SUTTON", "HARKER", "ORCHID",
                                  "BIRCH", "MAPLE", "REDWOOD", "PINE", "SPEAKEASY", "SAUGUS", "LOADER", "CYLLA", "KHARYBDIS", "TRITON", "NORMAN"};
    static const char* COND[6] = {" MUST BE DESTROYED", " MUST GET AIRBORN", " MUST REACH ITS DESTINATION", " MUST HAVE BEEN SIGHTED", "25% OF ", "50% OF "};
    auto subjName = [&](const sim::Trigger& t) -> std::string {
      if (t.kind == 0) return std::string("FLIGHT GROUP ") + (t.subject >= 0 && t.subject < 24 ? AIRN[t.subject] : "?");
      if (t.kind == 2) return std::string(t.b == 1 || t.b == 2 ? COND[3 + t.b] : "") + "GROUND TARGET GROUP " + (t.subject >= 0 && t.subject < 35 ? GRN[t.subject] : "?");
      if (t.kind == 3) return "F22 GROUP STORM";
      for (auto& u : W.units)                             // 0x305d4: model + group name (air / ground table) + number
        if (u.id == t.subject) {
          bool air = u.cls == sim::C_AIR;
          int g = u.group;
          std::string gn = air ? (g >= 0 && g < 24 ? AIRN[g] : "?") : (g >= 0 && g < 35 ? GRN[g] : "?");
          return u.model + " " + gn + " " + std::to_string(u.slot + 1);   // APPROXIMATION: the source of the number (record +2) is not verified
        }
      return "?";
    };
    float lh = TXH + 2, y = 12;
    int st = W.crashed ? 1 : W.primaryDone() ? 2 : 0;
    static const char* TITLE[3] = {"MISSION GOALS", "MISSION FAILED", "MISSION COMPLETED"};
    TC(160, y, TITLE[st], HUDC);
    y += 2 * lh;
    static const char* HDR[4] = {"PRIMARY GOALS", "SECONDARY GOALS", "BONUS GOALS", "IMMEDIATE MISSION FAILURES"};
    for (int cat = 0; cat < 4; cat++) {
      T(10, y, HDR[cat], CYAN);
      y += 12;
      for (auto& t : W.triggers()) {
        if (t.resp != 3 || t.a != cat) continue;
        std::string line = subjName(t);
        uint32_t c = HUDC;
        if (cat < 3) {
          line += t.kind == 3 ? COND[2] : (t.cond >= 0 && t.cond < 4 ? COND[t.cond] : "");
          if (t.fired) { line += " - COMPLETED -"; c = 0xFFFF00; }   // the colour of the completed line ([0x55ae7]) is estimated
        } else {
          line += " IS DESTROYED";
          if (t.fired) { line += " - OCCURED -"; c = RED; }
        }
        T(10, y, line, c);
        y += lh;
      }
      y += lh;
    }
  }
  // messages (0x365eb): at most 3 lines, y = 213 + 8i, cyan
  std::vector<std::string> ms;
  for (auto& m : W.msgs) if (m.until > W.now) ms.push_back(hudAscii(m.text));
  if (gPaused) ms.push_back("SZUNET (P)");
  size_t first = ms.size() > 3 ? ms.size() - 3 : 0;
  for (size_t i = first; i < ms.size(); i++) TC(160, 213 + 8.0f * float(i - first), ms[i], CYAN);
  gHud.present();
}

// ---------------------------------------------------------------- MFD pages (0x1cba0 table, full-screen views)
// The views of the numeric keypad (0x2998c, flag 0x1108) each approximate one display: the frame is
// F22TOPS/F22TOPLT/F22SCRS/F22SCRN/F22SCRLT/F22SCRBT.PCX (0x1cbc0), its inner rectangle (0x1cac0)
// falls on the 0x1ca60 rectangle (320x240 base unit). The page content is in this coordinate system.
const int MFD_RECT[6][4] = {{89, 68, 232, 166}, {89, 68, 230, 168}, {70, 28, 250, 212}, {64, 24, 256, 216}, {70, 28, 250, 212}, {64, 24, 256, 216}};
const int MFD_INNER[6][4] = {{20, 20, 108, 103}, {20, 20, 108, 103}, {18, 17, 108, 110}, {18, 17, 108, 110}, {18, 17, 108, 110}, {18, 17, 108, 110}};
const char* MFD_BEZEL[6] = {"F22TOPS.PCX", "F22TOPLT.PCX", "F22SCRS.PCX", "F22SCRN.PCX", "F22SCRLT.PCX", "F22SCRBT.PCX"};
Pic gBezel[6];
bool gBezelLoaded = false;
int gMfdRange = 1;                               // map scale: 10 / 20 / 40 / 80 nautical miles (S / X, 0x1ebb9)
const int MFD_NM[4] = {10, 20, 40, 80};

// the frame onto the 640x480 canvas: the inner rectangle should fall on the display rectangle
void drawBezel(int page) {
  if (!gBezelLoaded) { for (int i = 0; i < 6; i++) gBezel[i].load(gRes, MFD_BEZEL[i]); gBezelLoaded = true; }
  Screen& s = *gScreen;
  s.clear(0xFF141414u);
  const Pic& p = gBezel[page];
  if (!p.w) return;
  const int* r = MFD_RECT[page];
  const int* in = MFD_INNER[page];
  double k = 2.0 * (r[2] - r[0]) / double(in[2] - in[0]);   // 640 pixels / texture pixel
  double ky = 2.0 * (r[3] - r[1]) / double(in[3] - in[1]);
  double ox = 2.0 * r[0] - in[0] * k, oy = 2.0 * r[1] - in[1] * ky;
  for (int y = 0; y < Screen::H; y++) {
    int ty = int(std::floor((y - oy) / ky));
    if (ty < 0 || ty >= p.h) continue;
    for (int x = 0; x < Screen::W; x++) {
      int tx = int(std::floor((x - ox) / k));
      if (tx < 0 || tx >= p.w) continue;
      s.px[y * Screen::W + x] = p.rgba(p.px[ty * p.w + tx]);
    }
  }
}

void drawMfd(Hud& H, int Wd, int Ht, int page, bool tex = false) {
  H.begin(Wd, Ht);
  // tex: we draw into the cockpit's 128x128 display texture (0x1fb40): the page rectangle falls on the frame's inner rectangle, no text
  const int* R0 = MFD_RECT[page]; const int* IN0 = MFD_INNER[page];
  const float S = tex ? float(IN0[2] - IN0[0]) / (R0[2] - R0[0]) : Ht / 240.0f;
  const float SY = tex ? float(IN0[3] - IN0[1]) / (R0[3] - R0[1]) : S;
  const float OX = tex ? IN0[0] - R0[0] * S : Wd / 2.0f - 160 * S, OY = tex ? IN0[1] - R0[1] * SY : 0.0f;
  const int TH = tex ? 1 : std::max(1, int(std::lround(S / 2)));
  int f640 = std::max(1, int(std::lround(S / 2))), f320 = std::max(1, int(std::lround(S)));
  bool use640 = std::fabs(f640 * 2 - S) <= std::fabs(f320 - S);
  const int FS = use640 ? f640 : f320;
  gSmall = use640 ? gSmall640 : gSmall320;
  static const uint32_t RED = 0xFF0000, CYAN = 0x00FFFF, GREEN = 0x00BF00, WHITE = 0xFFFFFF, YELLOW = 0xFFFF00;
  auto X = [&](float x) { return OX + x * S; };
  auto Y = [&](float y) { return OY + y * SY; };
  auto L = [&](float x0, float y0, float x1, float y1, uint32_t c, bool dot = false) { H.pline(X(x0), Y(y0), X(x1), Y(y1), c, TH, dot); };
  auto T = [&](float x, float y, const std::string& t, uint32_t c) { if (tex) return; H.ptext(gSmall, int(X(x)), int(Y(y)), t, c, FS); };
  auto TW = [&](const std::string& t) { return H.ptextW(gSmall, t, FS) / S; };
  auto TC = [&](float cx, float y, const std::string& t, uint32_t c) { T(cx - TW(t) / 2, y, t, c); };
  auto BOX = [&](float x0, float y0, float x1, float y1, uint32_t c) { L(x0, y0, x1, y0, c); L(x1, y0, x1, y1, c); L(x1, y1, x0, y1, c); L(x0, y1, x0, y0, c); };
  auto CIRC = [&](float x, float y, float r, uint32_t c) { H.pcircle(X(x), Y(y), r * S, c, TH); };
  char b[96];
  sim::Unit& P = *W.player;
  const int* R = MFD_RECT[page];
  float cx = (R[0] + R[2]) / 2.0f, cy = (R[1] + R[3]) / 2.0f;
  H.setClip(int(X(R[0])), int(Y(R[1])), int(X(R[2])), int(Y(R[3])));
  double hdgDeg = std::fmod(P.hdg * 180 / sim::PI + 720, 360.0);

  // heading-up map (0x1dbb9 / 0x20ae5): world -> display
  D3 fh = sim::dirOf(P.hdg, 0), rh = sim::cross(D3{0, 1, 0}, fh);
  auto scope = [&](float ocx, float ocy, float Rpx, uint32_t ringC) {
    double nm = MFD_NM[gMfdRange];
    float k = float(Rpx / (nm * 380));
    // compass: 12 marks every 30°, rotated by the aircraft heading (0x20b73)
    static const char* LAB[12] = {"N", "3", "6", "E", "12", "15", "S", "21", "24", "W", "30", "33"};
    for (int i = 0; i < 12; i++) {
      double a = (i * 30 - hdgDeg) * sim::PI / 180;
      float sx = float(std::sin(a)), sy = float(-std::cos(a));
      L(ocx + sx * Rpx, ocy + sy * Rpx, ocx + sx * (Rpx - 6), ocy + sy * (Rpx - 6), ringC);
      TC(ocx + sx * (Rpx - 12), ocy + sy * (Rpx - 12) - 2, LAB[i], ringC);
    }
    CIRC(ocx, ocy, Rpx, ringC);
    H.pcircle(X(ocx), Y(ocy), Rpx / 2 * S, ringC, TH);
    std::snprintf(b, sizeof b, "%d", int(nm)); T(ocx + Rpx * 0.71f + 2, ocy - Rpx * 0.71f - 6, b, ringC);
    std::snprintf(b, sizeof b, "%d", int(nm / 2)); T(ocx + Rpx * 0.35f + 2, ocy - Rpx * 0.35f - 6, b, ringC);
    // own aircraft (0x1cc60)
    L(ocx, ocy - 4, ocx - 3, ocy + 3, WHITE); L(ocx, ocy - 4, ocx + 3, ocy + 3, WHITE); L(ocx - 3, ocy + 3, ocx + 3, ocy + 3, WHITE);
    return k;
  };
  auto toScope = [&](D3 p, float ocx, float ocy, float k, float& sx, float& sy) {
    D3 d = sim::wdelta(p, P.pos);
    sx = ocx + float(sim::dot(d, rh)) * k;
    sy = ocy - float(sim::dot(d, fh)) * k;
  };
  auto mark = [&](float x, float y, bool air, uint32_t c) {
    if (air) { L(x - 2, y, x, y - 2, c); L(x, y - 2, x + 2, y, c); L(x + 2, y, x, y + 2, c); L(x, y + 2, x - 2, y, c); }
    else BOX(x - 2, y - 2, x + 2, y + 2, c);
  };
  // target data (0x21373: air 0x20f2b red, ground 0x2117f cyan), line spacing 6
  auto targetInfo = [&](float x, float y) {
    if (!W.lockTgt || W.lockTgt->dead) return;
    sim::Unit& t = *W.lockTgt;
    bool air = t.cls == sim::C_AIR && !t.onGround;
    uint32_t c = air ? RED : CYAN;
    D3 d = sim::wdelta(t.pos, P.pos);
    double r = sim::len(d);
    T(x, y, upper(t.model), c);
    if (air) {
      long alt = long(t.pos.y * 16);
      std::snprintf(b, sizeof b, "ALT %ld.%ld", alt / 1000, (alt % 1000) / 100); T(x, y + 6, b, c);
      std::snprintf(b, sizeof b, "TURN TO %ld", long(std::fmod(std::atan2(d.x, d.z) * 180 / sim::PI + 360, 360.0))); T(x, y + 12, b, c);
      std::snprintf(b, sizeof b, "DIST %ld", long(r / 380)); T(x, y + 18, b, c);
      double kt = t.speed * 378.95;
      std::snprintf(b, sizeof b, "MACH %ld.%ld", long(kt / 610), long(kt * 10 / 610) % 10); T(x, y + 24, b, c);
      double rn = sim::len(sim::wdelta(t.pos + t.fwd() * t.speed, P.pos + W.v));
      std::snprintf(b, sizeof b, "VC %ld", long((r - rn) * 378.95)); T(x, y + 30, b, c);
    } else {
      std::snprintf(b, sizeof b, "%ld.%ld", long(r / 380), long(r * 10 / 380) % 10); T(x, y + 6, b, c);
    }
  };

  switch (page) {
    case 0: {                                             // HUD repeater: message log (0x1ebde), 8 lines
      std::vector<std::string> ms;
      for (auto& m : W.msgs) ms.push_back(hudAscii(m.text));
      if (tex) {                                          // cockpit (addition): with the SMALL320 font, wrapped, the latest lines
        const int x0 = IN0[0] + 2, x1 = IN0[2] - 2, lh = gSmall320.h + 1;
        std::vector<std::pair<std::string, bool>> lines;
        for (size_t i = 0; i < ms.size(); i++) {
          std::string cur;
          std::string word;
          auto flush = [&]() { if (!cur.empty()) lines.push_back({cur, i + 1 == ms.size()}); cur.clear(); };
          std::string t = ms[i] + " ";
          for (char ch : t) {
            if (ch != ' ') { word += ch; continue; }
            std::string cand = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && H.ptextW(gSmall320, cand, 1) > x1 - x0) { flush(); cur = word; } else cur = cand;
            word.clear();
          }
          flush();
        }
        int maxL = (IN0[3] - IN0[1] - 2) / lh;
        size_t first = lines.size() > size_t(maxL) ? lines.size() - maxL : 0;
        for (size_t i = first; i < lines.size(); i++)
          H.ptext(gSmall320, x0, IN0[1] + 2 + int(i - first) * lh, lines[i].first, lines[i].second ? GREEN : YELLOW, 1);
        break;
      }
      size_t first = ms.size() > 8 ? ms.size() - 8 : 0;
      for (size_t i = first; i < ms.size(); i++) T(96, 76 + 10.0f * float(i - first), ms[i], i + 1 == ms.size() ? GREEN : YELLOW);
      break;
    }
    case 1: break;                                        // artificial horizon: below (adi)
    case 2: {                                             // defensive display (0x1ecb5): threats
      float k = scope(cx, cy, 80, WHITE);
      for (auto& u : W.units) {
        if (u.dead || u.side != 4 || u.cls == sim::C_RUNWAY) continue;
        bool air = u.cls == sim::C_AIR && !u.onGround;
        bool threat = air || u.cls == sim::C_GROUND;
        if (!threat) continue;
        float x, y; toScope(u.pos, cx, cy, k, x, y);
        mark(x, y, air, (u.target == &P) ? YELLOW : RED);
      }
      for (auto& m : W.missiles)
        if (!m.dead && m.target == &P) {
          float x, y; toScope(m.pos, cx, cy, k, x, y);
          if ((W.stepNo & 8) == 0) { L(x - 3, y - 3, x + 3, y + 3, YELLOW); L(x - 3, y + 3, x + 3, y - 3, YELLOW); }
        }
      BOX(cx - 82, cy + 72, cx - 58, cy + 86, WHITE); BOX(cx - 52, cy + 72, cx - 28, cy + 86, WHITE);
      TC(91, 194, "CHAFF", WHITE); TC(121, 194, "FLARE", WHITE);
      TC(91, 200, std::to_string(W.chaff), WHITE); TC(121, 200, std::to_string(W.flares), WHITE);
      break;
    }
    case 3: {                                             // navigation display (0x1eee3): route, map
      float ncy = R[3] - (R[3] - R[1]) / 4.0f;
      float k = scope(cx, ncy, 130, WHITE);
      if (P.route >= 0 && P.route < int(W.routes().size())) {
        auto& rt = W.routes()[P.route];
        for (size_t i = 0; i < rt.size(); i++) {
          float x, y; toScope(rt[i].pos, cx, ncy, k, x, y);
          if (i + 1 < rt.size()) { float x2, y2; toScope(rt[i + 1].pos, cx, ncy, k, x2, y2); L(x, y, x2, y2, CYAN); }
          uint32_t c = int(i) == W.playerWp ? WHITE : CYAN;
          CIRC(x, y, 2, c);
          T(x + 3, y - 3, std::to_string(i + 1), c);
        }
      }
      for (auto& u : W.units) {
        if (u.dead || u.isPlayer || u.cls == sim::C_RUNWAY) continue;
        float x, y; toScope(u.pos, cx, ncy, k, x, y);
        mark(x, y, u.cls == sim::C_AIR && !u.onGround, u.side == 4 ? RED : GREEN);
      }
      targetInfo(64, 178);
      break;
    }
    case 4: {                                             // attack display (0x1ef50): radar, launch zone
      float k = scope(cx, cy, 80, WHITE);
      for (auto& u : W.units) {
        if (u.dead || u.side != 4 || u.cls == sim::C_RUNWAY) continue;
        bool air = u.cls == sim::C_AIR && !u.onGround;
        if (air && !W.radarOn) continue;
        float x, y; toScope(u.pos, cx, cy, k, x, y);
        mark(x, y, air, air ? RED : CYAN);
        if (&u == W.lockTgt) BOX(x - 5, y - 5, x + 5, y + 5, W.lockState == 2 ? RED : WHITE);
      }
      // launch zone (0x2065b): AIM-9X at most 0x8E8 wu, AIM-120 0x23A0 wu; the target's distance with an arrow
      int w = W.weapon == 1 || W.weapon == 2 ? W.weapon : 0;
      if (w) {
        double maxR = w == 1 ? 0x8E8 : 0x23A0;
        float x0 = cx + 80, y0 = cy + 32, H = 50;
        L(x0, y0 - H, x0, y0, WHITE); L(x0 - 3, y0 - H, x0 + 3, y0 - H, WHITE); L(x0 - 3, y0, x0 + 3, y0, WHITE);
        std::snprintf(b, sizeof b, "%d", int(maxR / 380)); T(x0 + 4, y0 - H - 3, b, WHITE);
        if (W.lockTgt && !W.lockTgt->dead) {
          double r = sim::len(sim::wdelta(W.lockTgt->pos, P.pos));
          float yy = y0 - float(std::min(1.0, r / maxR)) * H;
          L(x0 - 6, yy - 2, x0 - 2, yy, RED); L(x0 - 6, yy + 2, x0 - 2, yy, RED);
        }
      }
      targetInfo(70, 178);
      break;
    }
    case 5: {                                             // weapons (0x1f02c)
      TC(160, 62, "THRUST", YELLOW);
      int thr = W.thrAct >> 8;
      for (int s = -1; s <= 1; s += 2) {                  // engine indicators (0x1fdf4), r = 12
        float gx = 160 + s * 32.0f, gy = 120 - 56;
        CIRC(gx, gy + 14, 12, WHITE);
        double a = (-135 + 270 * std::min(thr, 110) / 110.0) * sim::PI / 180;
        L(gx, gy + 14, gx + float(std::sin(a)) * 10, gy + 14 - float(std::cos(a)) * 10, GREEN);
        TC(gx, gy + 28, thr > 100 ? "BURN" : std::to_string(thr), WHITE);
      }
      // throttle lever (0x20011)
      BOX(150, 80, 170, 84, WHITE);
      L(150 + std::min(W.thrCmd, 110) * 20 / 110.0f, 79, 150 + std::min(W.thrCmd, 110) * 20 / 110.0f, 85, GREEN);
      // aircraft outline
      L(160, 104, 152, 130, WHITE); L(160, 104, 168, 130, WHITE); L(152, 130, 120, 150, WHITE); L(168, 130, 200, 150, WHITE);
      L(120, 150, 150, 152, WHITE); L(200, 150, 170, 152, WHITE); L(150, 152, 146, 168, WHITE); L(170, 152, 174, 168, WHITE);
      L(146, 168, 174, 168, WHITE);
      TC(160, 118, fillHash("GUN ###", W.ammo[0]), WHITE);
      // weapon bays (0x20489)
      static const char* WN[4] = {"", "AIM-9X", "AIM-120", "JDAM"};
      for (int i = 1; i <= 3; i++) {
        float x = 104 + (i - 1) * 56.0f;
        BOX(x - 18, 176, x + 18, 196, W.weapon == i ? WHITE : CYAN);
        TC(x, 179, WN[i], W.weapon == i ? WHITE : CYAN);
        TC(x, 187, std::to_string(W.ammo[i]), W.weapon == i ? WHITE : CYAN);
      }
      T(72, 140, "CHAFF " + std::to_string(W.chaff), WHITE);
      T(208, 140, "FLARE " + std::to_string(W.flares), WHITE);
      if (W.weapon && W.ammo[W.weapon] > 0 && W.bayPos > 0) {   // "<WEAPON> -- READY" (0x1f02c)
        std::string t = std::string(WN[W.weapon]) + " -- READY";
        float tw = TW(t);
        H.pclear(int(X(160 - tw / 2 - 2)), int(Y(96)), int((tw + 4) * S), int(9 * S));
        BOX(160 - tw / 2 - 2, 96, 160 + tw / 2 + 2, 105, WHITE);
        TC(160, 98, t, WHITE);
      }
      break;
    }
  }
  H.noClip();

  // artificial horizon (0x213ac): sky cyan / ground black, white pitch ladder and aircraft symbol, speed / altitude / heading
  auto adi = [&](float x0, float y0, float x1, float y1, bool full) {
    float acx = (x0 + x1) / 2, acy = (y0 + y1) / 2, sc = (y1 - y0) / 100.0f;
    float ppd = 1.6f * sc;                                // pixels / degree
    double roll = P.roll, pitch = P.pitch * 180 / sim::PI;
    float ca = float(std::cos(roll)), sa = float(std::sin(roll));
    H.setClip(int(X(x0)), int(Y(y0)), int(X(x1)), int(Y(y1)));
    for (int py = int(Y(y0)); py <= int(Y(y1)); py++)  // row fill according to the horizon, pixel by pixel
      for (int px = int(X(x0)); px <= int(X(x1)); px++) {
        float yy = (py - OY) / SY;
        float lx = (px - OX) / S - acx, ly = yy - acy;
        float vy = -lx * sa + ly * ca;                    // vertical relative to the horizon
        bool sky = vy < float(pitch) * ppd;
        if (std::fabs(pitch) > 88) sky = pitch > 0;
        if (sky) H.pset(px, py, 0x00A0C0);
      }
    for (int a = -90; a <= 90; a += 10) {               // pitch ladder
      float off = float(a - pitch) * ppd;
      if (std::fabs(off) > (y1 - y0)) continue;
      float w = a == 0 ? 30 * sc : 12 * sc;
      float lx0 = -w, lx1 = w, ly = -off;
      auto rx = [&](float x, float y) { return acx + x * ca - y * sa; };
      auto ry = [&](float x, float y) { return acy + x * sa + y * ca; };
      L(rx(lx0, ly), ry(lx0, ly), rx(lx1, ly), ry(lx1, ly), WHITE, a < 0);
      if (a != 0 && full) { std::snprintf(b, sizeof b, "%d", std::abs(a)); T(rx(lx1 + 2, ly) , ry(lx1 + 2, ly) - 2, b, WHITE); }
    }
    // aircraft symbol (0x1ce2e)
    L(acx - 16 * sc, acy, acx - 5 * sc, acy, WHITE); L(acx + 5 * sc, acy, acx + 16 * sc, acy, WHITE);
    L(acx - 5 * sc, acy, acx, acy + 4 * sc, WHITE); L(acx + 5 * sc, acy, acx, acy + 4 * sc, WHITE);
    H.noClip();
    if (full && !tex) {                                   // boxes: speed, altitude, heading (cyan)
      auto box = [&](float a0, float b0, float a1, float b1, const std::string& t) {
        H.pclear(int(X(acx + a0)), int(Y(acy + b0)), int((a1 - a0) * S) + 1, int((b1 - b0) * S) + 1);
        BOX(acx + a0, acy + b0, acx + a1, acy + b1, WHITE);
        TC(acx + (a0 + a1) / 2, acy + (b0 + b1) / 2 - 2, t, CYAN);
      };
      box(-68, -4, -44, 4, std::to_string(int(sim::len(W.v) * 378.95)));
      box(44, -4, 68, 4, std::to_string(int(P.pos.y * 16)));
      box(-10, -48, 10, -40, std::to_string(int(hdgDeg)));
    } else BOX(x0, y0, x1, y1, WHITE);
  };
  if (page == 1) adi(float(R[0]), float(R[1]), float(R[2]), float(R[3]), true);
  else if (!tex) {                                        // 0x21e1a: small artificial horizon at top right, AGL and autopilot
    adi(256, 2, 319, 64, false);
    std::snprintf(b, sizeof b, "%d AGL", int((P.pos.y - W.groundWU(P.pos.x, P.pos.z)) * 16)); T(258, 64 - 12, b, WHITE);
    if (W.apMode) T(258, 64 - 6, W.apMode == 1 ? "AUTOLEVEL" : "AUTOPILOT", RED);
  }
  if (!tex && gViewMsgUntil > gClock) { H.ptext(gSmall, int(X(220)) + FS, int(Y(10)) + FS, gViewMsg, 0, FS); T(220, 10, gViewMsg, RED); }
  std::vector<std::string> ms;
  for (auto& m : W.msgs) if (m.until > W.now) ms.push_back(hudAscii(m.text));
  if (page != 0 && !tex) {
    size_t first = ms.size() > 2 ? ms.size() - 2 : 0;
    for (size_t i = first; i < ms.size(); i++) TC(160, 222 + 8.0f * float(i - first), ms[i], CYAN);
  }
  if (!tex) H.present();
}


// cockpit displays (0x1fb40): the 5 pages (horizon, defensive, navigation, attack, weapons) go onto the cockpit model's
// F22TOPLT / F22SCRS / F22SCRN / F22SCRLT / F22SCRBT texture, drawn onto the frame
Hud gMfdHud;
GLuint gMfdTex[6] = {};
bool gMfdInit = false;
void updateCockpitMfds() {
  if (!mCockpit) return;
  if (!gMfdInit) {
    gMfdHud.init();
    if (!gBezelLoaded) { for (int i = 0; i < 6; i++) gBezel[i].load(gRes, MFD_BEZEL[i]); gBezelLoaded = true; }
    glGenTextures(6, gMfdTex);
    for (int i = 0; i < 6; i++) {                        // 0: top display (F22TOPS) = HUD repeater (addition; the original 0x1fb40 only has 1..5)
      glBindTexture(GL_TEXTURE_2D, gMfdTex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      mCockpit->overrideTexture(MFD_BEZEL[i], gMfdTex[i]);
    }
    gMfdInit = true;
  }
  static uint32_t buf[128 * 128];
  for (int page = 0; page < 6; page++) {
    const Pic& p = gBezel[page];
    if (p.w != 128 || p.h != 128) continue;
    gMfdHud.begin(128, 128);
    drawMfd(gMfdHud, 128, 128, page, true);
    const uint32_t* h = gMfdHud.bits();
    const int* in = MFD_INNER[page];
    for (int y = 0; y < 128; y++)
      for (int x = 0; x < 128; x++) {
        uint32_t c = p.rgba(p.px[y * 128 + x]);
        uint32_t v = h[y * 128 + x] & 0xFFFFFF;
        if (v && x >= in[0] && x <= in[2] && y >= in[1] && y <= in[3]) c = 0xFF000000u | ((v >> 16) & 255) | (v & 0xFF00) | ((v & 255) << 16);
        buf[y * 128 + x] = c;
      }
    glBindTexture(GL_TEXTURE_2D, gMfdTex[page]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  }
}

// ---------------------------------------------------------------- sound and music (the logic of the original 0x6604e / 0xea60)
Audio gAudio;
void applySettings() {                                  // the menu settings applied to sound, sky and view distance
  const auto& st = gMenu.settings;
  gAudio.sfxVol = st.sfx / 100.0f;
  gAudio.musicVol = st.music / 100.0f;
  gR.skyHigh = st.skyHigh;
  gFogDist = 260000.0f * (0.35f + 0.65f * st.texDist / 100.0f);
}
bool gAudioOn = false;
// sound id -> WAV (the original's loader row, 0x22edd: lea edx,[0x65c60+4*id])
const char* SND_NAME[40] = {nullptr, "FLYBY.WAV", "EXPLODE.WAV", "DAMN.WAV", "YEEHAW.WAV", "OCV040.WAV", "MISSLE.WAV",
                            "ENG2.WAV", "OCV046.WAV", "GEAR.WAV", "SCREECH.WAV", "GUNS.WAV", "BANG.WAV", "BANG2.WAV",
                            "BEEP.WAV", "TONE2.WAV", "OCV036.WAV", "WNG005.WAV", "WNG002.WAV", "WNG003.WAV", "WNG009.WAV",
                            "WNG013.WAV", "AAA.WAV", "FLARE.WAV", "BRAKE.WAV", "FLAPS.WAV", "BAY.WAV", nullptr, nullptr,
                            "EJECT.WAV"};
int gSndCount[64] = {};
int sndSample(int id, const std::string& wav = "") {
  if (!wav.empty()) return gAudio.load(wav);
  if (id <= 0 || id >= 40 || !SND_NAME[id]) return -1;
  return gAudio.load(SND_NAME[id]);
}
// music: events of 0xea60; state (0xe768): -1 crashed, 0 on ground, 2 flying, 3..5 combat (ACT3), 7 heavy combat (ACT5)
int gMusState = 0;
int gMusAvg = 0, gMus778 = 0, gMus780 = 0, gMus784 = 0;   // [0xe788] [0xe778] [0xe780] [0xe784]
std::vector<std::string> gMusLog;                // --soundtest: the music state machine's transitions
float gMusHold = 0, gMusCheck = 0;
double gEngPitch = 0;                                  // 0x2c950 (16.16)
int gLockSnd = 0, gWarnSnd = 0;
std::vector<float> gFlybyT;
std::vector<char> gDeadPrev;
bool gWasAir = false, gWasGround = true, gWonMus = false, gCrashMus = false;

void musicResume() {                                   // 0xef21: the repeating music of the state
  static const char* L[9] = {"DIELOOP.MID", "TAXILOOP.MID", "SOARLOW.MID", "SOARLOOP.MID", "ACT3_LP1.MID", "ACT3_LP2.MID",
                             "ACT3_LP3.MID", "ACT3_LP3.MID", "ACT5LOOP.MID"};
  gAudio.musicQueue(L[std::clamp(gMusState + 1, 0, 8)], true);
}
void musicEvent(int ev) {                              // 0xea60
  if (!gAudioOn) return;
  switch (ev) {
    case 0: gAudio.musicPlay("QINTRO.MID", false); gAudio.musicQueue("TAXILOOP.MID", true); gMusState = 0; break;
    case 1: gAudio.musicPlay("TAKEOFF.MID", false); gAudio.musicQueue("SOARINTR.MID", false); gAudio.musicQueue("SOARLOOP.MID", true); gMusState = 2; break;
    case 2: gAudio.musicPlay("FR_DIE.MID", false); musicResume(); break;
    case 3: gAudio.musicPlay("ENEMYDIE.MID", false); musicResume(); break;
    case 4: gAudio.musicPlay("F-22WIN.MID", false); gAudio.musicQueue("SOARLOOP.MID", true); gMusState = 2; break;
    case 5: gAudio.musicPlay("CRSHBURN.MID", false); gAudio.musicQueue("DIELOOP.MID", true); gMusState = -1; break;
    case 6: gAudio.musicPlay("TAXILOOP.MID", true); gMusState = 0; break;
  }
  gMusHold = 0;
}
void menuMusic() {
  if (!gAudioOn) return;
  gAudio.stopAll();
  gAudio.musicPlay("MENUINTR.MID", false);             // 0x388ef: intro, then repeating menu music
  gAudio.musicQueue("MENULOOP.MID", true);
}
void missionAudioStart() {
  gEngPitch = 0; gLockSnd = gWarnSnd = 0;
  gFlybyT.assign(W.units.size(), -100.0f);
  gDeadPrev.assign(W.units.size(), 0);
  for (size_t i = 0; i < W.units.size(); i++) gDeadPrev[i] = W.units[i].dead;
  gWasAir = !W.player->onGround; gWasGround = W.player->onGround; gWonMus = gCrashMus = false;
  gMusAvg = gMus778 = gMus780 = gMus784 = 0; gMusCheck = 0;
  if (!gAudioOn) return;
  gAudio.stopAll();
  int e = sndSample(sim::SND_ENGINE);
  gAudio.chPlay(0, e, 0, 0, 1, true);                  // 0x54e22: engine on channel 0, looping
  if (gWasAir) musicEvent(1); else musicEvent(0);
}
// sound processing after one simulation step (40 Hz)
void audioStep() {
  if (!gAudioOn) { W.sfx.clear(); return; }
  // engine (0x2ccc9): pitch from thrust, smoothed by 1/64
  {
    int p = std::min(W.thrAct >> 8, 101) - 59;
    double tgt = p >= 42 ? p * 1500.0 : (p ? p + 21 : 0) * 550.0;
    gEngPitch += std::trunc((tgt - gEngPitch) / 64);
  }
}
float panOf(V3 d) {
  V3 r = cross(gCamUp, gCamF);                          // screen-right (X-mirrored image)
  float l = len(d);
  return l < 1e-3f ? 0.0f : std::clamp(dot(d, r) / l * 0.8f, -0.8f, 0.8f);
}
void audioFrame() {
  if (!gAudioOn) { W.sfx.clear(); return; }
  sim::Unit& P = *W.player;
  // sound events of the steps
  for (auto& e : W.sfx) {
    gSndCount[e.id & 63]++;
    int s = sndSample(e.id, e.wav);
    if (s < 0) continue;
    float vol = e.vol / 255.0f, pan = 0;
    if (e.positional) {
      V3 d = rpos(e.pos) - gCamPos;
      float wu = len(d) / float(sim::WU_M);
      bool own = sim::len(sim::wdelta(e.pos, P.pos)) < 3;
      // own aircraft sound (0x2d7ec): in the cockpit the base volume, outside by distance (>>14); others: >>15
      float fall = own ? (gCockpit ? 1.0f : std::max(0.0f, 1 - wu / 64)) : std::max(0.0f, 1 - wu / 128);
      vol *= fall;
      if (!own || !gCockpit) pan = panOf(d);
      if (vol <= 0.01f) continue;
    }
    gAudio.play(s, vol, pan, float(e.pitch));
  }
  W.sfx.clear();
  bool alive = !W.crashed && !gPaused;
  // engine channel
  {
    float vol = 0;
    if (alive && gEngPitch > 0x800) {
      if (gCockpit) vol = 0x46 / 255.0f;
      else {
        float wu = len(rpos(playerPos()) - gCamPos) / float(sim::WU_M);
        vol = std::max(16.0f, 255 - wu) / 255.0f;
      }
    }
    gAudio.chSet(0, vol, gCockpit ? 0.0f : panOf(rpos(playerPos()) - gCamPos), float(std::max(0.05, gEngPitch / 65536.0)));
  }
  // lock tone (0x33084): tracking = BEEP at double pitch, locked = TONE2 continuously
  {
    int want = alive && (W.weapon == 1 || W.weapon == 2) ? W.lockState : 0;
    if (want != gLockSnd) {
      gLockSnd = want;
      if (want == 1) gAudio.chPlay(1, sndSample(sim::SND_BEEP), 0xAA / 255.0f, 0, 2.0f, true);
      else if (want == 2) gAudio.chPlay(1, sndSample(sim::SND_TONE), 0x28 / 255.0f * 3, 0, 28.0f, true);   // 0x3310a: edx = 0x1c0000 = 28 x the natural speed (192 Hz base -> ~5.4 kHz)
      else gAudio.chStop(1);
    }
  }
  // missile warning (0x35cbb / 0x35d14): radar = BEEP, infrared = BEEP at half pitch
  {
    int want = 0;
    if (alive)
      for (auto& m : W.missiles)
        if (!m.dead && m.target == &P) want = std::max(want, W.wdef[m.w].seeker == 1 ? 1 : 2);
    if (want != gWarnSnd) {
      gWarnSnd = want;
      if (want) gAudio.chPlay(2, sndSample(sim::SND_BEEP), 0x96 / 255.0f, 0, want == 2 ? 1.0f : 0.5f, true);
      else gAudio.chStop(2);
    }
  }
  // departing aircraft (0x2b8c8): within 40 wu, closing at at least 550 knots
  if (gFlybyT.size() == W.units.size())
    for (size_t i = 0; i < W.units.size(); i++) {
      sim::Unit& u = W.units[i];
      if (u.dead || u.isPlayer || u.cls != sim::C_AIR || u.onGround || W.now - gFlybyT[i] < 4) continue;
      V3 d = rpos(u.pos) - gCamPos;
      float wu = len(d) / float(sim::WU_M);
      if (wu > 40) continue;
      sim::D3 rel = u.fwd() * u.speed - W.v;
      double closing = -sim::dot(rel, sim::D3{d.x, d.y, d.z}) / std::max(1e-3f, len(d)) * 378.95;
      if (closing < 550) continue;
      gFlybyT[i] = W.now;
      gAudio.play(sndSample(sim::SND_FLYBY), 0xC8 / 255.0f, panOf(d), 1);
    }
  // music: events and combat intensity
  if (gDeadPrev.size() == W.units.size()) {
    for (size_t i = 0; i < W.units.size(); i++) {
      sim::Unit& u = W.units[i];
      if (u.dead && !gDeadPrev[i] && !u.isPlayer && u.cls != sim::C_RUNWAY && gMusState >= 0) {
        if (u.side == 4) musicEvent(3);
        else if (u.side == 8 && u.cls == sim::C_AIR) musicEvent(2);
      }
      gDeadPrev[i] = u.dead;
    }
  }
  if (W.crashed && !gCrashMus) { gCrashMus = true; musicEvent(5); }
  if (!W.crashed && W.primaryDone() && !gWonMus) { gWonMus = true; musicEvent(4); }
  if (!W.crashed && !P.onGround && !gWasAir && P.wasAirborne) { gWasAir = true; musicEvent(1); }
  if (!W.crashed && P.onGround && !gWasGround && gWasAir) musicEvent(6);
  gWasGround = P.onGround;
  // combat intensity (0xeb53): every 64 timer ticks (120 Hz). Average = (3·average + shots) / 4; counters:
  // ground units aiming at the player (e77c), air units and missiles aiming at the player (e770), air combat anywhere
  // (e76c); intensity = average/64 + (e770+2)/4 + (e76c ? 1 : 0). Songs (slots of 0x7b8f0): 9 SOARLOOP,
  // 10 SOARLOW, 11 ACT1, 12 ACT3INTR, 13–15 ACT3_LP1..3, 16 ACT5INTR, 17 ACT5LOOP; "play" (0x7c43f) replaces the
  // current one, "queue" (0x7c46c) appends after it.
  if (gMusState > 0 && W.now - gMusCheck >= 64.0 / 120.0) {
    gMusCheck = W.now;
    static const char* SONG[18] = {"", "", "", "", "", "", "", "", "", "SOARLOOP.MID", "SOARLOW.MID", "ACT1.MID", "ACT3INTR.MID",
                                   "ACT3_LP1.MID", "ACT3_LP2.MID", "ACT3_LP3.MID", "ACT5INTR.MID", "ACT5LOOP.MID"};
    auto mlog = [&](const char* w) { if (gSoundTest) { char t[96]; std::snprintf(t, sizeof t, "t=%.1f state=%d %s", W.now, gMusState, w); gMusLog.push_back(t); } };
    auto play = [&](int id) { gAudio.musicPlay(SONG[id], true); mlog(SONG[id]); };
    auto playQ = [&](int a, int b) { gAudio.musicPlay(SONG[a], false); gAudio.musicQueue(SONG[b], true); mlog(SONG[a]); };
    gMusAvg = (3 * gMusAvg + W.musShotsAtPlayer + W.musShotsFriendly) >> 2;
    W.musShotsAtPlayer = W.musShotsFriendly = 0;
    int airEng = 0, atPlayer = 0, ground = 0;
    auto onGround = [](const sim::Unit& u) { return u.cls != sim::C_AIR || u.onGround; };
    for (auto& u : W.units) {
      if (u.dead || !u.target || u.isPlayer) continue;
      if (onGround(u)) { if (u.target == &P) ground++; }
      else { if (u.target == &P) atPlayer++; if (!onGround(*u.target)) airEng++; }
    }
    for (auto& m : W.missiles) {                         // the missiles are on the list too (+0x45 = 8: "air")
      if (m.dead || !m.target) continue;
      if (m.target == &P) atPlayer++;
      if (!onGround(*m.target)) airEng++;
    }
    if (W.lockTgt && !W.lockTgt->dead && onGround(*W.lockTgt) && sim::len(sim::wdelta(W.lockTgt->pos, P.pos)) <= 0x1644) ground++;
    int inten = (gMusAvg >> 6) + ((atPlayer + 2) >> 2) + (airEng ? 1 : 0);
    if (gMusState == 6 && gAudio.musicCurrent() != "ACT1.MID") { /* 0xec66: waits during ACT1 */ }
    else {
      bool act5;
      if (!(ground >= inten && ground != 0)) act5 = gMusState == 7 && gMus780 < 30;
      else if (gMusState < 4) act5 = true;
      else if (gMusState != 7) act5 = gMus778 >= 30;
      else act5 = true;
      if (act5) {
        if (gMusState != 7) { playQ(16, 17); gMusState = 7; }
        gMus780++;
      } else {
        bool calm = gMusState == 1 || gMusState == 2 || gMusState == 7;
        if (inten == 0) {
          if (gMusState != 1 && gMusState != 2) { play(9); gMusState = 2; gMus778 = 0; }
          gMus784++;
        } else if (inten == 1) {
          if (gMusState != 3) { if (calm) playQ(12, 13); else play(13); gMusState = 3; }
          gMus784 = 0;
        } else if (inten == 2) {
          if (gMusState != 4) { play(14); gMusState = 4; }   // from calm the original immediately overwrites 12 with 14
          gMus778++; gMus784 = 0;
        } else {
          if (gMusState != 5) { if (calm) play(15); else if (gMusState == 3) playQ(14, 15); else play(15); gMusState = 5; }
          gMus778++; gMus784 = 0;
        }
        if (gMus778 > 60) { play(11); gMus778 = 0; gMusState = 6; }
        if (gMus784 > 60 && gMusState != 1) { play(10); gMusState = 1; }
        gMus780 = 0;
      }
    }
  }
}

// ---------------------------------------------------------------- KDV videos (the original player: F22.EXE 0x89490)
// The videos are in the video folder next to the program (HAIR.KDV …); a missing file is silently skipped.
// Shown directly at 640x480, doubled at 320x240; the sound is 22050 Hz 8-bit stereo.
// The frame is derived from the audio playback position, so the picture does not drift.
std::string exeDir();
void saveBMP(const std::string& path, int Wd, int Ht);
void menuMusic();
bool playVideo(HDC dc, const std::string& name) {
  Kdv k;
  std::string path = exeDir() + "\\video\\" + name;
  if (!k.open(path)) return true;
  gAudio.stopAll();
  int sl = gAudio.addPcm8("KDV_L", k.audioL.data(), k.audioL.size(), 22050), sr = gAudio.addPcm8("KDV_R", k.audioR.data(), k.audioR.size(), 22050);
  gVideoActive = true; gVideoSkip = false;
  std::vector<uint32_t> img(size_t(k.w) * k.h);
  auto t0 = std::chrono::steady_clock::now();
  bool audioOn = gAudioOn && !k.audioL.empty();
  if (audioOn) { gAudio.chPlay(6, sl, 1.0f, -1.0f, 1, false); gAudio.chPlay(7, sr, 1.0f, 1.0f, 1, false); }
  bool ok = true;
  int shown = -1;
  while (gRunning && !gVideoSkip) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) gRunning = false; TranslateMessage(&m); DispatchMessageW(&m); }
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double ap = audioOn ? gAudio.chPos(6) : -1;
    if (ap >= 0) t = ap;                                  // the audio is the authoritative clock
    int want = int(t * k.fps);
    if (want >= k.frames && k.index >= k.frames - 1) break;
    bool more = true;
    while (k.index < want && (more = k.next())) {}
    if (!more && k.index < want) break;
    if (k.index != shown && k.index >= 0) {
      shown = k.index;
      k.rgba(img.data());
      if (!gVideoTest.empty() && (shown == 20 || shown == 60 || shown == 100)) { gScreen->present(gW, gH); }
      gScreen->clear();
      for (int y = 0; y < Screen::H; y++)
        for (int x = 0; x < Screen::W; x++) {
          int sx = k.w == Screen::W ? x : x * k.w / Screen::W, sy = k.h == Screen::H ? y : y * k.h / Screen::H;
          gScreen->px[y * Screen::W + x] = img[size_t(sy) * k.w + sx];
        }
      gScreen->present(gW, gH);
      if (!gVideoTest.empty() && (shown == 20 || shown == 60 || shown == 100)) saveBMP(exeDir() + "\\video_" + std::to_string(shown) + ".bmp", gW, gH);
      SwapBuffers(dc);
    } else Sleep(2);
  }
  if (gVideoSkip) ok = false;
  gAudio.chStop(6); gAudio.chStop(7);
  gVideoActive = false;
  return ok;
}
// Videos one after another; any key skips the current one and the rest too.
void playVideos(HDC dc, const std::vector<std::string>& v, bool skipAll = true) {
  for (auto& n : v) if (!playVideo(dc, n) && skipAll) break;
  gVideoSkip = false;
  menuMusic();
}

// ---------------------------------------------------------------- window
// Alt+Enter: full-screen toggle (addition; not in the original). Borderless window stretched to the monitor size.
bool gFullscreen = false;
WINDOWPLACEMENT gPrevPlace = {sizeof(WINDOWPLACEMENT)};
void toggleFullscreen(HWND h) {
  DWORD style = GetWindowLongW(h, GWL_STYLE);
  if (!gFullscreen) {
    MONITORINFO mi = {sizeof mi};
    if (!GetWindowPlacement(h, &gPrevPlace) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
    SetWindowLongW(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW | WS_POPUP | WS_VISIBLE);
    SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    gFullscreen = true;
  } else {
    SetWindowLongW(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
    SetWindowPlacement(h, &gPrevPlace);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    gFullscreen = false;
  }
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE: gW = LOWORD(lp); gH = HIWORD(lp); return 0;
    case WM_CLOSE: gRunning = false; return 0;
    case WM_SETCURSOR:
      if (gApp == APP_MENU && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }   // own (original) mouse cursor
      break;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
      if (gVideoActive) { if (msg == WM_LBUTTONDOWN) gVideoSkip = true; return 0; }
      gMouseX = short(LOWORD(lp)); gMouseY = short(HIWORD(lp));
      if (msg == WM_LBUTTONDOWN) { gMouseDown = true; SetCapture(h); }
      if (msg == WM_LBUTTONUP) { gMouseDown = false; ReleaseCapture(); }
      return 0;
    case WM_CHAR: if (gVideoActive) return 0; if (gApp == APP_MENU) { gMenu.chr(int(wp)); return 0; } break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
      if (gVideoActive && !(msg == WM_SYSKEYDOWN && wp == VK_RETURN) && !(msg == WM_SYSKEYDOWN && wp == VK_F4)) { gVideoSkip = true; return 0; }
      if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && !(lp & (1 << 30))) { toggleFullscreen(h); gMenu.settings.fullscreen = gFullscreen; return 0; }   // Alt+Enter
      if (gApp == APP_MENU) {
        if (msg == WM_SYSKEYDOWN) break;
        Menu::Action a = gMenu.key(int(wp));
        if (a == Menu::QUIT) gRunning = false;
        if (a == Menu::START) gWantStart = true;
        return 0;
      }
      if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;   // Alt+F4: quit
      if (lp & (1 << 30)) return 0;
      // the original's key code (0x27c0a): scan code, upper byte 1 Shift, 2 Alt, 4 Ctrl
      int sc = (lp >> 16) & 0xFF;
      bool ext = (lp >> 24) & 1;
      bool shift = GetKeyState(VK_SHIFT) & 0x8000, ctrl = GetKeyState(VK_CONTROL) & 0x8000, alt = GetKeyState(VK_MENU) & 0x8000;
      if (wp == VK_ESCAPE || (ext && sc == 0x4F)) { if (gDirect) gRunning = false; else gWantEnd = true; return 0; }   // end of mission
      if (alt && sc == 0x11) { gOrbYaw = gOrbPitch = gOrbYawV = gOrbPitchV = 0; return 0; }   // Alt+W: view back to default (0x53626)
      if (ctrl && !alt) {                              // wingman commands (0x283e5: Ctrl+C/E/P/F/M)
        if (sc == 0x19) W.wingCommand(1);              // Ctrl+P: patrol route
        if (sc == 0x21) W.wingCommand(2);              // Ctrl+F: formation
        if (sc == 0x12) W.wingCommand(3);              // Ctrl+E: attack
        if (sc == 0x32) W.wingCommand(4);              // Ctrl+M: the player's target
        if (sc == 0x2E) W.wingCommand(5);              // Ctrl+C: cover
        if (sc == 0x24) W.eject();                     // Ctrl+J: eject (0x283e5 -> 0x17 -> 0xe310)
        return 0;
      }
      if (alt) return 0;
      if (sc >= 0x3B && sc <= 0x42) { selectView(sc - 0x3B); return 0; }        // F1..F8 (0x2998c)
      if (!ext && (sc == 0x33 || sc == 0x34)) {                                  // , . : HUD brightness (addition)
        gHud.brightness = std::clamp(gHud.brightness + (sc == 0x34 ? 0.2f : -0.2f), 0.2f, 1.0f);
        W.say("HUD BRIGHTNESS " + std::to_string(int(gHud.brightness * 100 + 0.5f)) + "%", 1.5f);
        return 0;
      }
      if (sc == 0x43 || sc == 0x44 || sc == 0x57 || sc == 0x58) {                // F9..F12: time acceleration (0x53f49)
        static const int TS[4] = {1, 2, 4, 6};
        gTimeScale = TS[sc == 0x43 ? 0 : sc == 0x44 ? 1 : sc == 0x57 ? 2 : 3];
        W.say(std::to_string(gTimeScale) + "X TIME COMPRESSION", 2);
        return 0;
      }
      if (ext) return 0;
      switch (sc) {                                    // 0x283e5 command table, 0x27b86 executors
        case 0x02: W.selectWeapon(2); break;           // 1: AMRAAM
        case 0x03: W.selectWeapon(1); break;           // 2: SIDEWINDER
        case 0x04: W.selectWeapon(0); break;           // 3: cannon
        case 0x05: W.selectWeapon(3); break;           // 4: JDAM
        case 0x06: W.throttleKey(0); break;            // 5..0, -: 0, 60, 70 ... 110 % (0x27519–0x275aa)
        case 0x07: W.throttleKey(60); break;
        case 0x08: W.throttleKey(70); break;
        case 0x09: W.throttleKey(80); break;
        case 0x0A: W.throttleKey(90); break;
        case 0x0B: W.throttleKey(100); break;
        case 0x0C: W.throttleKey(110); break;
        case 0x11: W.cycleWeapon(); break;             // W
        case 0x0F: W.cycleTargets(true, shift ? -1 : 1); break;   // Tab / Shift+Tab: air target
        case 0x28: W.nearestTarget(true); break;       // ': nearest air target
        case 0x1A: W.cycleTargets(false, -1); break;   // [ ]: ground target
        case 0x1B: W.cycleTargets(false, 1); break;
        case 0x1C:                                     // Enter: ground target; restart after a crash
          if (W.crashed) { W.restart(); gFx.reset(); applyRadii(); snapshot(); gCamInit = false; selectView(1, false); }
          else W.nearestTarget(false);
          break;
        case 0x22: W.toggleGear(); break;              // G
        case 0x21: W.toggleFlaps(); break;             // F
        case 0x2E: W.countermeasures(); break;         // C
        case 0x13: W.toggleRadar(); break;             // R
        case 0x1E: W.toggleAutopilot(2); break;        // A: AUTOPILOT
        case 0x26: W.toggleAutopilot(1); break;        // L: AUTOLEVEL
        case 0x23: W.homeWaypoint(); break;            // H: home
        case 0x31: W.nextWaypoint(); break;            // N: next waypoint
        case 0x19: gPaused = !gPaused; break;          // P
        case 0x32: gGoals = !gGoals; break;            // M: mission goals (0x5372e)
        case 0x47: selectView(8); break;               // numeric 7, 9, 4, 5, 6, 2: MFD pages
        case 0x49: selectView(9); break;
        case 0x4B: selectView(10); break;
        case 0x4C: selectView(11); break;
        case 0x4D: selectView(12); break;
        case 0x50: selectView(13); break;
        case 0x1F: if (curMode() == VM_MFD) gMfdRange = std::max(0, gMfdRange - 1); break;   // S / X: map scale (0x1ebb9)
        case 0x2D: if (curMode() == VM_MFD) gMfdRange = std::min(3, gMfdRange + 1); break;
        case 0x17: gInvert = !gInvert; W.say(gInvert ? "Inverted elevator control" : "Normal elevator control", 1.5f); break;   // I (own)
      }
      return 0;
    }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

std::string exeDir() {
  char p[MAX_PATH];
  GetModuleFileNameA(nullptr, p, MAX_PATH);
  std::string s(p);
  return s.substr(0, s.find_last_of("\\/"));
}
bool findRes(const std::string& given, std::string& out) {
  std::vector<std::string> c;
  if (!given.empty()) c.push_back(given);
  std::string d = exeDir();
  for (int i = 0; i < 4; i++) {
    c.push_back(d + "\\RESOURCE.RES");
    size_t k = d.find_last_of("\\/");
    if (k == std::string::npos) break;
    d = d.substr(0, k);
  }
  c.push_back("RESOURCE.RES");
  for (auto& p : c) if (gRes.open(p)) { out = p; return true; }
  return false;
}

void saveBMP(const std::string& path, int Wd, int Ht) {
  std::vector<uint8_t> px(size_t(Wd) * Ht * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, Wd, Ht, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  BITMAPFILEHEADER fh = {};
  BITMAPINFOHEADER ih = {};
  fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + DWORD(px.size());
  ih.biSize = sizeof ih; ih.biWidth = Wd; ih.biHeight = Ht; ih.biPlanes = 1; ih.biBitCount = 32;
  std::fwrite(&fh, sizeof fh, 1, f); std::fwrite(&ih, sizeof ih, 1, f); std::fwrite(px.data(), 1, px.size(), f);
  std::fclose(f);
}

// Self-test: take-off with the original flight model, gear retraction, cockpit, lock and fire, landing.
struct AutoTest {
  int step = 0, phase = 0;
  FILE* log = nullptr;
  std::string dir;
  bool shot = false, shot2 = false;
  std::string shotName;
  sim::Unit* target = nullptr;
  double h0 = 0;
  void L(const char* tag) {
    sim::Unit& P = *W.player;
    if (!log) return;
    std::fprintf(log, "%-14s t=%6.2f ground=%d crashed=%d kt=%6.1f ft=%7.0f agl=%6.0f pitch=%5.1f roll=%5.1f thr=%d/%d gear=%d L=%d kills=%d %s\n",
                 tag, W.now, P.onGround, W.crashed, sim::len(W.v) * 378.95, P.pos.y * 16,
                 (P.pos.y - W.groundWU(P.pos.x, P.pos.z)) * 16, P.pitch * 57.3, P.roll * 57.3, W.thrAct >> 8, W.thrCmd,
                 W.gearPos, W.lift, W.kills, W.crashWhy.c_str());
    for (auto& u : W.units)                               // the player's wingman (to check taxiing / take-off)
      if (!u.dead && u.cls == sim::C_AIR && u.leader == &P)
        std::fprintf(log, "    kisero: st=%d node=%d pos=(%.0f %.0f) y=%.0f hdg=%.0f speed=%.3f tav=%.0f\n", int(u.st), u.node, u.pos.x, u.pos.z, u.pos.y, u.hdg * 57.3, u.speed, sim::len(sim::wdelta(u.pos, P.pos)));
    if (W.aiVm()) {                                       // summary of the aircraft driven by the original scripts
      int cnt[10] = {}, air = 0; double dmin = 1e9, dmax = 0, aglMin = 1e9, aglMax = -1e9, spMax = 0; const sim::Unit* fast = nullptr;
      for (auto& u : W.units) if (u.cls == sim::C_AIR && !u.isPlayer && !u.dead) { cnt[u.st]++; air++; if (!u.onGround) { double ag = u.pos.y - W.groundWU(u.pos.x, u.pos.z); aglMin = std::min(aglMin, ag); aglMax = std::max(aglMax, ag); spMax = std::max(spMax, u.speed * 378.95); } double d = sim::len(sim::wdelta(u.pos, P.pos)); dmin = std::min(dmin, d); dmax = std::max(dmax, d); }
      std::fprintf(log, "    AI(%d gep): parked=%d taxi=%d takeoff=%d route=%d form=%d pursue=%d track=%d extend=%d evade=%d pullup=%d tav=%.0f..%.0f AGL=%.0f..%.0f wu vmax=%.0f kt esemeny=%d/%d\n", air, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7], cnt[8], cnt[9], dmin, dmax, aglMin, aglMax, spMax, W.evSeen, W.evFired);
    }
    std::fflush(log);
  }
  bool tick() {
    phase++;
    sim::Unit& P = *W.player;
    auto next = [&](const char* tag) { L(tag); step++; phase = 0; };
    if (W.crashed) { L("FAIL_crash"); return false; }
    switch (step) {
      case 0: if (phase == 1) selectView(2); if (phase > 20) { shot = true; shotName = "1_ground"; next("start"); } break;
      case 1: W.throttleKey(110); next("afterburner"); break;
      case 2: if (sim::len(W.v) * 378.95 > 185) { gInject = {VK_DOWN}; next("rotate"); } if (phase > 2400) { next("FAIL_noSpeed"); return false; } break;
      case 3: if (!P.onGround) next("liftoff"); if (phase > 800) { next("FAIL_noLiftoff"); return false; } break;
      case 4: if (phase > 60) { gInject.clear(); next("climb"); } break;
      case 5: if (phase > 120) { W.toggleGear(); W.toggleFlaps(); next("gear_up"); } break;
      case 6: if (phase > 400) { shot = true; shotName = "2_air"; W.throttleKey(100); next("air"); } break;
      case 7: if (phase > 10) { selectView(1); next("cockpit"); } break;
      case 8: if (phase > 40) { shot = true; shotName = "3_cockpit"; next("cockpit_shot"); } break;
      case 9:                                            // roll right: the picture must bank and turn right too
        if (phase == 10) { selectView(2); h0 = P.hdg; gInject = {VK_RIGHT}; }
        if (phase == 30) {
          gInject.clear();
          D3 sr = sim::cross(W.up, W.f);                 // the image's right direction = the original +x (cross(up, f))
          std::fprintf(log, "roll_test: roll=%.1f deg, heading change=%.1f deg, right wing (screen) y=%.3f -> %s\n",
                       P.roll * 57.3, std::remainder(P.hdg - h0, 2 * sim::PI) * 57.3, sr.y, sr.y < 0 ? "OK (jobb szarny lent)" : "HIBAS");
          next("chase");
        }
        break;
      case 10: {                                         // combat: over the nearest enemy radar, turns onto it
        double bd = 1e30;
        for (auto& u : W.units) if (!u.dead && u.side == 4 && u.cls == sim::C_GROUND) {
          double d = sim::len(sim::wdelta(u.pos, P.pos)); if (d < bd) { bd = d; target = &u; } }
        if (!target) { next("FAIL_noTarget"); return false; }
        P.pos = target->pos + D3{-700, 350, 0};
        W.f = sim::norm(sim::wdelta(target->pos, P.pos)); W.up = sim::norm(D3{0, 1, 0} - W.f * W.f.y);
        W.v = W.f * 1.0; W.yawR = W.pitchR = W.rollR = 0;
        W.weapon = 3; if (W.ammo[3] == 0) W.ammo[3] = 2;  // JDAM (against a ground target)
        W.lockTgt = target; W.lockState = 0;
        for (auto& u : W.units) u.lastLaunch = W.stepNo;   // for the duration of the test the SAMs do not fire
        gCamInit = false;
        next("teleport");
        break;
      }
      case 11:
        W.f = sim::norm(sim::wdelta(target->pos, P.pos)); W.up = sim::norm(D3{0, 1, 0} - W.f * W.f.y); W.v = W.f * sim::len(W.v);
        W.lockTgt = target;
        if (W.lockState == 2) { gInject = {VK_SPACE}; next("locked"); }
        if (phase > 1600) { next("FAIL_noLock"); return false; }
        break;
      case 12:
        if (phase == 4) gInject.clear();
        W.f = sim::norm(D3{W.f.x, std::max(W.f.y, 0.2), W.f.z}); W.up = sim::norm(D3{0, 1, 0} - W.f * W.f.y);
        if (!shot2 && phase == 50) { shot = true; shotName = "4_weapon"; shot2 = true; }
        if (target->dead || target->hp < target->maxhp) { L(target->dead ? "target_destroyed" : "target_damaged"); shot = true; shotName = "5_hit"; next("hit"); }
        if (phase > 1200) { next("FAIL_noHit"); return false; }
        break;
      case 13: if (phase > 20) {                         // landing: towards the runway, 150 knots, gentle descent
          sim::Unit* rw = nullptr;
          for (auto& u : W.units) if (u.cls == sim::C_RUNWAY) { rw = &u; break; }
          if (!rw) { next("FAIL_noRunway"); return false; }
          double gh = W.groundWU(rw->pos.x, rw->pos.z);
          P.pos = rw->pos; P.pos.y = gh + 2.5;
          W.gearWantUp = false; W.gearPos = 0; W.flapsWant = true; W.flapPos = 64;
          double pa = 6 * sim::PI / 180;
          D3 hf = sim::dirOf(rw->hdg, 0);
          W.f = sim::norm(hf * std::cos(pa) + D3{0, std::sin(pa), 0}); W.up = sim::norm(D3{0, 1, 0} - W.f * W.f.y);
          W.v = sim::norm(hf * 1.0 + D3{0, -0.04, 0}) * (150 / 378.95);
          W.yawR = W.pitchR = W.rollR = 0; W.throttleKey(0); W.thrAct = 0;
          for (auto& m : W.missiles) m.dead = true;
          gCamInit = false;
          next("approach");
        } break;
      case 14: if (P.onGround) { gInject = {'B'}; next("touchdown"); } if (phase > 400) { next("FAIL_noTouchdown"); return false; } break;
      case 15: if (sim::len(W.v) * 378.95 < 3 || phase > 2400) { gInject.clear(); next(sim::len(W.v) * 378.95 < 3 ? "stopped" : "FAIL_noStop"); } break;
      case 16:                                           // AI observation: 3 minutes of the mission, the player is invulnerable
        P.hp = P.maxhp;
        if (phase == 2400) {                             // after 60 s: 2000 wu in front of a flying enemy aircraft
          sim::Unit* e = nullptr;
          for (auto& u : W.units) if (u.cls == sim::C_AIR && u.side == 4 && !u.onGround && !u.dead) { e = &u; break; }
          if (e) {
            P.onGround = false; W.gearWantUp = true; W.gearPos = 64; W.flapsWant = false; W.flapPos = 0;
            P.pos = e->pos + e->fwd() * 2000; P.pos.y = e->pos.y;
            W.f = sim::norm(sim::wdelta(e->pos, P.pos)); W.f.y = 0; W.f = sim::norm(W.f); W.up = {0, 1, 0};
            W.v = W.f * 1.2; W.throttleKey(100); W.thrAct = 100 << 8;
            std::fprintf(log, "player placed 2000 wu ahead of %s (group %d)\n", e->model.c_str(), e->group);
          }
        }
        if (phase > 2400) {                              // the player flies straight, holding altitude
          W.f = sim::norm(D3{W.f.x, 0, W.f.z}); W.up = {0, 1, 0};
          gInject.clear();
        }
        if (phase % 400 == 0) {
          int st[10] = {}, dead = 0, air = 0;
          for (auto& u : W.units) if (u.cls == sim::C_AIR && !u.isPlayer) { st[u.st]++; dead += u.dead; air += !u.onGround; }
          std::fprintf(log, "AI t=%.0f states park=%d taxi=%d tko=%d route=%d form=%d pursue=%d track=%d extend=%d evade=%d pullup=%d airborne=%d dead=%d fired: enemy=%d friendly=%d player=%d\n",
                       W.now, st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9], air, dead, W.firedBy[4], W.firedBy[8], W.firedBy[2]);
          std::fprintf(log, "   evade maneuvers: %d %d %d %d %d %d %d %d\n", W.evadeCount[0], W.evadeCount[1], W.evadeCount[2],
                       W.evadeCount[3], W.evadeCount[4], W.evadeCount[5], W.evadeCount[6], W.evadeCount[7]);
          // formation: mean deviation from position; evasion: how many aircraft are manoeuvring right now
          double fe = 0; int fn = 0, inv = 0;
          for (auto& u : W.units) if (u.cls == sim::C_AIR && !u.dead && u.st == sim::S_FORM && u.leader && u.leader->st != sim::S_EVADE && u.leader->st != sim::S_PULLUP) {
            fe += u.slotErr; fn++;
          }
          for (auto& u : W.units) if (u.st == sim::S_EVADE && std::cos(u.roll) < 0) inv++;
          std::fprintf(log, "   formation: %d wingmen, mean slot error %.1f wu; inverted now %d; terrain kills %d\n", fn, fn ? fe / fn : 0.0, inv, W.terrainKills);
          std::fflush(log);
        }
        if (phase >= 3600 && phase % 600 == 300)                       // evasion test: every airborne enemy aircraft
          for (auto& u : W.units)
            if (u.cls == sim::C_AIR && !u.dead && !u.onGround && !u.isPlayer && u.side == 4 && u.st != sim::S_FORM) W.startEvade(u);
        if (phase > 7200) { next("ai_done"); return false; }
        break;
      // ---- shadow (--shadowtest): low-flying aircraft from an external view
      case 500: {
        P.hp = P.maxhp;
        if (phase == 2) for (auto& u : W.units) if (u.cls == sim::C_RUNWAY) std::fprintf(log, "runway: sim ground %.2f m, terrain mesh %.2f m, player y %.2f m\n", W.groundWU(u.pos.x, u.pos.z) * 4.8768, gTerrain.heightAt(float(u.pos.x * 4.8768), float(u.pos.z * 4.8768)), W.player->pos.y * 4.8768);
        if (phase == 2) { selectView(2); gOrbDist = 14; gOrbPitch = 0x180000; gOrbYaw = 0x1000000; }
        // the aircraft stands on the ground (the start position)
        if (phase == 30) { shot = true; shotName = "shadow_1"; }
        if (phase == 40) { next("shadow_done"); return false; }
        break;
      }
      // ---- ILS (--ilstest): the aircraft on the runway's 2.5 nautical mile final, slightly left and high
      case 600: {
        P.hp = P.maxhp;
        sim::Unit* rw = nullptr;
        for (auto& u : W.units) if (u.cls == sim::C_RUNWAY) { rw = &u; break; }
        if (!rw) { next("no_runway"); return false; }
        if (phase == 2) {
          D3 rf = sim::dirOf(rw->hdg, 0), rr = sim::cross(D3{0, 1, 0}, rf);
          P.pos = rw->pos - rf * 950 - rr * 12;
          P.pos.y = W.groundWU(rw->pos.x, rw->pos.z) + 950 * std::tan(3.0 * 0.017453292519943295) + 12;
          P.onGround = false; W.f = rf; W.up = {0, 1, 0}; W.v = rf * 2.0; W.gearPos = 0; W.gearWantUp = false;
          selectView(1);
        }
        if (phase >= 3 && phase < 6) { P.pos.y = W.groundWU(rw->pos.x, rw->pos.z) + 950 * std::tan(3.0 * 0.017453292519943295) + 12; }
        if (phase == 4) { auto I = W.ils(); std::fprintf(log, "ils: active=%d nm=%.2f loc=%.2f gs=%.2f hdg=%.0f\n", I.active, I.nm, I.loc, I.gs, I.hdg); shot = true; shotName = "ils_1"; }
        if (phase == 8) { next("ils_done"); return false; }
        break;
      }
      // ---- ejection (--ejecttest)
      case 800: {
        if (phase == 2) { P.onGround = false; P.pos.y = W.groundWU(P.pos.x, P.pos.z) + 400; W.v = W.f * 2.0; W.gearPos = 64; W.eject(); }
        if (phase == 3) std::fprintf(log, "eject: ejected=%d chute=%d vel=(%.2f,%.2f,%.2f)\n", W.ejected, int(W.chute.on), W.chute.vel.x, W.chute.vel.y, W.chute.vel.z);
        if (phase == 40) { shot = true; shotName = "eject_1"; }
        if (phase == 200) { shot = true; shotName = "eject_2"; }
        if (phase % 100 == 0) std::fprintf(log, "t=%.1f chute y=%.1f ground=%.1f vy=%.3f plane crashed=%d\n", W.now, W.chute.pos.y, W.groundWU(W.chute.pos.x, W.chute.pos.z), W.chute.vel.y, W.crashed);
        if (phase == 1200) { next("eject_done"); return false; }
        break;
      }
      // ---- Create Mission (--createtest): check of an assembled mission
      case 700: {
        P.hp = P.maxhp;
        if (phase == 2) {
          std::map<std::string, int> cnt;
          for (auto& u : W.units) cnt[u.model + " side" + std::to_string(u.side) + (u.isPlayer ? " PLAYER" : "")]++;
          for (auto& kv : cnt) std::fprintf(log, "  %-28s x%d\n", kv.first.c_str(), kv.second);
          std::fprintf(log, "goals primary=%d tod=%d ammo=%d/%d/%d player=(%.0f,%.0f,%.0f) onGround=%d\n", W.goalTotal[0], W.tod, W.ammo[1], W.ammo[2], W.ammo[3], P.pos.x, P.pos.y, P.pos.z, P.onGround);
          for (auto& u : W.units) if (u.cls == sim::C_AIR && !u.isPlayer) std::fprintf(log, "  air %s side%d grp%d wing%d at (%.0f,%.0f,%.0f) dist %.0f\n", u.model.c_str(), u.side, u.group, u.wing, u.pos.x, u.pos.y, u.pos.z, sim::len(sim::wdelta(u.pos, P.pos)));
          selectView(3);
        }
        if (phase == 20) {                                  // target selection: shoot list (Enter), ] to step, Tab
          P.pos.y += 600; P.onGround = false;
          W.selectWeapon(1); W.nearestTarget(false);
          std::fprintf(log, "shootlist:");
          for (auto* u : W.shootList) std::fprintf(log, " %s(%.0f)", u->model.c_str(), sim::len(sim::wdelta(u->pos, P.pos)));
          std::fprintf(log, " -> lock %s\n", W.lockTgt ? W.lockTgt->model.c_str() : "-");
          W.cycleTargets(false, 1); std::fprintf(log, "] -> %s\n", W.lockTgt ? W.lockTgt->model.c_str() : "-");
          W.selectWeapon(3); W.nearestTarget(false);
          std::fprintf(log, "jdam shootlist: %d -> %s\n", int(W.shootList.size()), W.lockTgt ? W.lockTgt->model.c_str() : "-");
          gGoals = true;
          { W.selectWeapon(0); W.lockTgt = nullptr; W.cycleTargets(true, 1);
            sim::D3 ld; bool ok = W.gunLead(ld);
            if (W.lockTgt) { sim::D3 d0 = sim::wdelta(W.lockTgt->pos, P.pos);
              std::fprintf(log, "gunLead: %d  |d0|=%.1f |lead|=%.1f  target speed %.3f wu/step\n", int(ok), sim::len(d0), ok ? sim::len(ld) : 0.0, W.lockTgt->speed); } }
          W.selectWeapon(1); W.lockTgt = nullptr; W.cycleTargets(true, 1); std::fprintf(log, "tab -> %s\n", W.lockTgt ? W.lockTgt->model.c_str() : "-");
        }
        if (phase == 30) { shot = true; shotName = "create_1"; }
        if (phase == 400) {
          int dead = 0, tot = 0;
          for (auto& u : W.units) if (u.side == 4) { tot++; dead += u.dead; }
          std::fprintf(log, "after 400 steps: enemies %d dead %d, crashed %d\n", tot, dead, W.crashed);
          next("create_done"); return false;
        }
        break;
      }
      // ---- wingman commands (--wingtest): the flight airborne, commands one after another
      case 400: {
        P.hp = P.maxhp;
        static const char* ST[10] = {"park", "taxi", "tko", "route", "form", "pursue", "track", "extend", "evade", "pullup"};
        auto dump = [&](const char* tag) {
          std::fprintf(log, "%s:", tag);
          for (auto& u : W.units) if (!u.dead && u.leader == &P)
            std::fprintf(log, " [%s %s order=%d tgt=%s dist=%.0f tmask=%d]", u.model.c_str(), ST[u.st], u.order, u.target ? u.target->model.c_str() : "-", sim::len(sim::wdelta(u.pos, P.pos)), u.tmask);
          if (target) for (auto& u : W.units) if (!u.dead && u.leader == &P)
            std::fprintf(log, " {MIG tav=%.0f, latja=%d, dy=%.0f}", sim::len(sim::wdelta(target->pos, u.pos)), W.los(u.pos, target->pos), target->pos.y - u.pos.y);
          std::fprintf(log, " | msg: %s\n", W.msgs.empty() ? "" : W.msgs.back().text.c_str());
        };
        if (phase == 2) {
          int n = 0;
          for (auto& u : W.units) if (u.leader == &P) n++;
          std::fprintf(log, "mission %s: wingmen %d\n", gMission.c_str(), n);
          for (auto& u : W.units) if (u.cls == sim::C_AIR && sim::len(sim::wdelta(u.pos, P.pos)) < 1900)
            std::fprintf(log, "  near: %s sub=%d side=%d group=%d wing=%d player=%d dist=%.0f leader=%s\n", u.model.c_str(), u.sub, u.side, u.group, u.wing, u.isPlayer, sim::len(sim::wdelta(u.pos, P.pos)), u.leader ? u.leader->model.c_str() : "-");
          if (!n) { next("no_wingmen"); return false; }
          P.onGround = false; P.pos.y = W.groundWU(P.pos.x, P.pos.z) + 500;
          W.f = sim::norm(D3{W.f.x, 0, W.f.z}); W.up = {0, 1, 0}; W.v = W.f * 1.2; W.throttleKey(100); W.thrAct = 100 << 8;
          W.gearWantUp = true; W.gearPos = 64;
          for (auto& u : W.units) if (u.leader == &P) {
            u.onGround = false; u.wasAirborne = true; u.pos = P.pos - W.f * 30.0 + sim::cross(W.up, W.f) * 20.0; u.hdg = P.hdg; u.pitch = 0;
            u.speed = 1.2; u.st = sim::S_FORM; u.active = true;
            W.aiReseed(u);
          }
          // the nearest enemy aircraft in front of the player, at 3000 wu
          for (auto& u : W.units) if (!u.dead && u.side == 4 && u.cls == sim::C_AIR) {
            u.onGround = false; u.wasAirborne = true; u.active = true; u.pos = P.pos + W.f * 3000.0; u.hdg = P.hdg; u.speed = 1.0; u.st = sim::S_ROUTE;
            W.aiReseed(u);
            target = &u; break;
          }
          W.lockTgt = target;
        }
        if (phase > 2) { W.f = sim::norm(D3{W.f.x, 0, W.f.z}); W.up = {0, 1, 0}; W.yawR = W.pitchR = W.rollR = 0; W.v = W.f * 1.2; }
        if (phase == 40) { dump("start"); std::fprintf(log, "Ctrl+M ok=%d\n", W.wingCommand(4)); }
        if (phase == 80) dump("after_ctrlM");
        if (phase == 120) { std::fprintf(log, "Ctrl+F ok=%d\n", W.wingCommand(2)); }
        if (phase == 160) dump("after_ctrlF");
        if (phase == 200) { std::fprintf(log, "Ctrl+P ok=%d\n", W.wingCommand(1)); }
        if (phase == 400) dump("after_ctrlP");
        if (phase == 440) { std::fprintf(log, "Ctrl+E ok=%d\n", W.wingCommand(3)); }
        if (phase == 480) dump("after_ctrlE");
        if (phase == 520) { std::fprintf(log, "Ctrl+C ok=%d\n", W.wingCommand(5)); }
        if (phase == 560) { dump("after_ctrlC"); next("wing_done"); return false; }
        break;
      }
      // ---- effects (--fxtest): destruction of a ground target and an aircraft, missile smoke trail
      case 300: {
        P.hp = P.maxhp;
        static sim::Unit* g = nullptr; static sim::Unit* a = nullptr;
        static D3 eye, look;
        auto hold = [&]() {                               // the aircraft is fixed, facing the observed point
          P.onGround = false; P.pos = eye;
          W.f = sim::norm(sim::wdelta(look, eye)); W.up = sim::norm(D3{0, 1, 0} - W.f * W.f.y);
          W.v = W.f * 1.0; W.yawR = W.pitchR = W.rollR = 0; W.gearWantUp = true; W.gearPos = 64;
        };
        if (phase == 2) {
          for (auto& u : W.units) if (!u.dead && u.side == 4 && u.cls == sim::C_GROUND) { g = &u; break; }
          for (auto& u : W.units) if (!u.dead && u.side == 4 && u.cls == sim::C_AIR && !u.onGround) { a = &u; break; }
          if (!g) { next("FAIL_noGround"); return false; }
          for (auto& u : W.units) u.lastLaunch = W.stepNo;
          eye = g->pos + D3{-22, 6, -4}; look = g->pos + D3{0, 4, 0};
          W.testKill(*g);
          selectView(1); if (curMode() != VM_FULL) selectView(1);
        }
        if (phase >= 2 && phase < 610) hold();
        if (phase == 12 || phase == 40) { shot = true; shotName = phase == 12 ? "fx_1_burst" : "fx_1b_debris"; }
        if (phase == 250) { shot = true; shotName = "fx_2_burning"; }
        if (phase == 600) { shot = true; shotName = "fx_3_smoke"; }
        if (phase == 610 && a) { look = a->pos; eye = a->pos + a->fwd() * -150.0 + D3{0, 60, 0}; W.testKill(*a); }
        if (phase >= 610 && phase < 700 && a) hold();
        if (phase == 650) { shot = true; shotName = "fx_4_falling"; }
        if (phase == 690) { shot = true; shotName = "fx_4b_falling"; }
        if (phase == 700) { eye = eye + D3{0, 300, 0}; look = eye + W.f * 100.0; look.y = eye.y; hold(); W.selectWeapon(1); W.ammo[1] = 2; gInject = {VK_SPACE}; }
        if (phase > 700) { W.v = sim::norm(D3{W.f.x, 0, W.f.z}) * 1.0; W.f = sim::norm(D3{W.f.x, 0, W.f.z}); W.up = {0, 1, 0}; W.yawR = W.pitchR = W.rollR = 0; }
        if (phase == 704) gInject.clear();
        if (phase == 740) { selectView(2); gOrbDist = 40; }
        if (phase == 800) { shot = true; shotName = "fx_5_trail"; next("fx_done"); return false; }
        break;
      }
      // ---- gear animation (--geartest): from the side and from the front, halfway
      case 200: {
        P.hp = P.maxhp;
        if (phase == 2) {
          P.onGround = false; P.pos.y = W.groundWU(P.pos.x, P.pos.z) + 600;
          W.f = sim::norm(D3{W.f.x, 0, W.f.z}); W.up = {0, 1, 0}; W.v = W.f * 1.0; W.throttleKey(100); W.thrAct = 100 << 8;
          selectView(2);
        }
        static const int G[4] = {0, 24, 48, 62};
        int k = phase / 30;
        if (k >= 10) { next("gear_done"); return false; }
        if (k >= 8) {                                     // flaps out, and aileron right (from behind, from above)
          W.f = sim::norm(D3{W.f.x, 0, W.f.z}); W.up = {0, 1, 0}; W.yawR = W.pitchR = W.rollR = 0;
          W.flapPos = k == 8 ? 64 : 0; W.flapsWant = k == 8; W.gearPos = 64;
          gInject.clear(); if (k == 9) gInject = {VK_RIGHT};
          gOrbDist = 12; gOrbPitch = 0x300000; gOrbYaw = 0; gOrbYawV = gOrbPitchV = 0;
          if (phase % 30 == 25) { shot = true; shotName = "gear_" + std::to_string(k); }
          break;
        }
        W.f = sim::norm(D3{W.f.x, 0, W.f.z}); W.up = {0, 1, 0}; W.yawR = W.pitchR = W.rollR = 0;
        W.gearPos = G[k % 4]; W.gearWantUp = true;
        W.bayPos = k % 4 == 2 ? 20 : 0; W.brakePos = k % 4 == 3 ? 32 : 0;
        gOrbDist = 9; gOrbPitch = -0x20000; gOrbYaw = k < 4 ? 0x800000 : 0x1000000;   // from the side, then from the front
        gOrbYawV = gOrbPitchV = 0;
        if (phase % 30 == 25) { shot = true; shotName = "gear_" + std::to_string(k); }
        break;
      }
      // ---- view and autopilot test (--viewtest)
      case 100: if (phase == 2) {                         // into the air, 600 wu above the terrain, towards an enemy aircraft
          P.onGround = false; W.gearWantUp = true; W.gearPos = 64; W.flapsWant = false; W.flapPos = 0;
          P.pos.y = W.groundWU(P.pos.x, P.pos.z) + 600;
          W.f = sim::norm(D3{W.f.x, 0, W.f.z}); W.up = {0, 1, 0}; W.v = W.f * 1.2; W.throttleKey(100); W.thrAct = 100 << 8;
          for (auto& u : W.units) if (u.cls == sim::C_AIR && u.side == 4 && !u.dead) { target = &u; break; }
          W.lockTgt = target; W.selectWeapon(2);
          for (auto& u : W.units) u.lastLaunch = W.stepNo;
          next("airborne");
        } break;
      case 101: {                                         // F1..F8: a picture of every view
        P.hp = P.maxhp;
        int k = phase / 40;
        if (k == 6 && phase % 40 == 1) { W.lockTgt = target; gInject = {VK_SPACE}; }   // missile for F7
        if (phase % 40 == 1 && k < 14) {
          if (k == 7) gInject.clear();
          selectView(k);
          std::fprintf(log, "view F%d -> slot F%d mode %d '%s'\n", k + 1, gSlot + 1, int(curMode()), gViewMsg.c_str());
        }
        if (k == 6 && phase % 40 == 39) { selectView(6); std::fprintf(log, "view F7 again -> slot F%d mode %d '%s'\n", gSlot + 1, int(curMode()), gViewMsg.c_str()); }
        if (phase % 40 == 30 && k < 14) { shot = true; shotName = k < 8 ? "view_F" + std::to_string(k + 1) : "view_mfd" + std::to_string(k - 7); }
        if (k >= 8) W.lockTgt = target;
        if (k == 1 && phase % 40 == 35) { selectView(1); shot = true; shotName = "view_F2b"; }   // second F2: full view
        if (k == 2 && phase % 40 == 10) gOrbYawV = 0x6000;                                // the effect of Ctrl+arrow
        if (phase >= 560) { selectView(2); next("views_done"); }
        break;
      }
      case 102: {                                         // AUTOLEVEL: from 60° bank, -20° pitch
        P.hp = P.maxhp;
        if (phase == 1) {
          D3 rt;
          sim::basis(P.hdg, -20 * sim::PI / 180, 60 * sim::PI / 180, W.f, W.up, rt);
          W.yawR = W.pitchR = W.rollR = 0; W.toggleAutopilot(1);
        }
        if (phase % 80 == 0) std::fprintf(log, "autolevel t=%5.1f ap=%d pitch=%6.1f roll=%6.1f agl=%6.0f ft\n", phase / 40.0, W.apMode,
                                          P.pitch * 57.3, P.roll * 57.3, (P.pos.y - W.groundWU(P.pos.x, P.pos.z)) * 16);
        if (phase == 600) { W.apMode = 0; next("autolevel_done"); }
        break;
      }
      case 103: {                                         // AUTOPILOT: turns towards the waypoint, holds altitude
        P.hp = P.maxhp;
        const sim::Waypoint* wp = W.playerWaypoint();
        if (!wp) { next("FAIL_noWaypoint"); return false; }
        D3 d = sim::wdelta(wp->pos, P.pos);
        double err = std::remainder(std::atan2(d.x, d.z) - P.hdg, 2 * sim::PI) * 57.3;
        if (phase == 1) {                                 // starts rotated by 120°
          D3 rt;
          sim::basis(std::atan2(d.x, d.z) + 120 * sim::PI / 180, 0, 0, W.f, W.up, rt);
          W.v = W.f * 1.2; W.yawR = W.pitchR = W.rollR = 0;
          W.toggleAutopilot(2);
          std::fprintf(log, "waypoint %d alt %.0f ft, dist %.0f wu\n", W.playerWp + 1, wp->pos.y * 16, std::hypot(d.x, d.z));
        }
        if (phase % 120 == 0) std::fprintf(log, "autopilot t=%5.1f ap=%d hdgErr=%7.1f roll=%6.1f pitch=%6.1f ft=%6.0f safe=%6.0f thr=%d wp=%d\n", phase / 40.0, W.apMode,
                                           err, P.roll * 57.3, P.pitch * 57.3, P.pos.y * 16, 0.0, W.thrCmd, W.playerWp + 1);
        if (phase == 400) { shot = true; shotName = "view_autopilot"; }
        if (phase == 2400) {
          W.homeWaypoint(); W.homeWaypoint();
          std::fprintf(log, "H: waypoint %d ap=%d\n", W.playerWp + 1, W.apMode);
          gInject = {VK_LEFT};
        }
        if (phase == 2405) { std::fprintf(log, "stick input -> ap=%d (0 = off)\n", W.apMode); gInject.clear(); next("autopilot_done"); return false; }
        break;
      }
    }
    return true;
  }
};

MissionResult missionResult() {
  MissionResult r;
  for (auto& u : W.units) {
    if (u.side != 4 || !u.dead) continue;
    if (u.cls == sim::C_AIR) r.airKills++;
    else if (u.cls != sim::C_RUNWAY) r.groundKills++;
  }
  r.playerAir = W.playerAirKills; r.playerGround = W.playerGroundKills;
  r.missiles = W.firedBy[2];
  for (int k = 0; k < 4; k++) { r.goalTotal[k] = W.goalTotal[k]; r.goalDone[k] = W.goalDone[k]; }
  r.crashed = W.crashed != 0;
  r.success = W.primaryDone() && !W.crashed;
  r.failed = W.missionFailed();
  r.aborted = !r.success && !r.failed;
  r.time = W.now;
  r.ejected = W.ejected != 0; r.landed = W.tally.landed;
  r.bulletsFired = W.tally.bulletsFired; r.bulletsHit = W.tally.bulletsHit; r.missilesFired = W.tally.missilesFired;
  r.missilesHit = W.tally.missilesHit; r.jdamFired = W.tally.jdamFired; r.jdamKills = W.tally.jdamKills;
  r.score = W.missionScore(gMenu.campaignMission());
  return r;
}

// Menu self-test: an image of every screen into the build folder (menu_*.bmp)
bool runMenuTest(HDC dc) {
  std::string dir = exeDir();
  auto shot = [&](const char* name) {
    gMenu.draw(*gScreen);
    gScreen->present(gW, gH);
    saveBMP(dir + "\\menu_" + name + ".bmp", gW, gH);
    SwapBuffers(dc);
  };
  gMenu.mouse(-1, -1, false);
  shot("main");
  gMenu.key(VK_DOWN); gMenu.key(VK_RETURN); shot("quick");            // quick mission
  gMenu.mouse(100, 230, false); gMenu.mouse(100, 230, true); gMenu.frame(); gMenu.mouse(100, 230, false);
  shot("quick_sel3");
  gMenu.key(VK_ESCAPE); gMenu.go(Menu::NEWCAMP); shot("newcamp");
  {                                                    // callsign: the 3rd row, "MAVERICK", ACCEPT -> briefing
    auto click = [&](int x, int y) { gMenu.mouse(x, y, true); gMenu.frame(); gMenu.mouse(x, y, false); gMenu.frame(); };
    click(150, 214);
    for (const char* c = "maverick"; *c; c++) gMenu.chr(*c);
    shot("newcamp_name");
    gMenu.key(VK_BACK); gMenu.chr('K');
    click(460, 227);
    FILE* lg = std::fopen((dir + "\\menutest.txt").c_str(), "a");
    if (lg) { std::fprintf(lg, "newcamp: screen=%d (BRIEF=%d) mission=%s\n", int(gMenu.scr), int(Menu::BRIEF), gMenu.statusLine().c_str()); std::fclose(lg); }
    gMenu.go(Menu::CONTCAMP); shot("contcamp_pilot");
    gMenu.go(Menu::STATS); shot("stats_pilot");
  }
  gMenu.go(Menu::CONTCAMP); shot("contcamp");
  gMenu.go(Menu::QUICK);
  gMenu.mouse(380, 370, false); gMenu.mouse(380, 370, true); gMenu.frame(); gMenu.mouse(380, 370, false);   // ACCEPT
  shot("brief");
  gMenu.go(Menu::MAP); shot("map");
  gMenu.go(Menu::OPTIONS); gMenu.settings.sfx = 70; gMenu.settings.music = 40; gMenu.settings.texDist = 85; gMenu.settings.skyHigh = false; shot("options");
  gMenu.go(Menu::OPTIONS2); shot("options2");
  {                                                    // mouse test: clicking on the bar must not jump to another screen
    gMenu.go(Menu::OPTIONS);
    struct P { int x, y; const char* what; } T[] = {{400, 362, "sfx bar"}, {240 - 10, 362, "sfx minus"}, {591, 395, "music plus"}, {420, 194, "texdist bar"}, {450, 160, "sky high"}, {560, 160, "sky low"}};
    bool ok = true;
    for (auto& t : T) {
      gMenu.mouse(t.x, t.y, true); gMenu.frame(); gMenu.mouse(t.x, t.y, false); gMenu.frame();
      bool stay = gMenu.scr == Menu::OPTIONS;
      if (!stay) { ok = false; gMenu.go(Menu::OPTIONS); }
      FILE* lg = std::fopen((dir + "\\menutest.txt").c_str(), "a");
      if (lg) { std::fprintf(lg, "%s: screen %s, sfx %d music %d tex %d sky %d\n", t.what, stay ? "OPTIONS" : "ELUGROTT", gMenu.settings.sfx, gMenu.settings.music, gMenu.settings.texDist, int(gMenu.settings.skyHigh)); std::fclose(lg); }
    }
    if (!ok) return false;
  }
  gMenu.spec.enemy[0] = {3, 2, 3}; gMenu.spec.enemy[1] = {1, 3, 2}; gMenu.spec.ground = 2; gMenu.spec.samNum = 2; gMenu.spec.aaaNum = 4; gMenu.spec.aaaSkill = 2;
  gMenu.spec.friendly[0] = {3, 1, 2}; gMenu.spec.region = 2; gMenu.spec.tod = 3; gMenu.spec.aim9 = 4; gMenu.spec.jdam = 2;
  gMenu.go(Menu::CREATE_E); shot("create_e");
  gMenu.go(Menu::CREATE_F); shot("create_f");
  {                                                    // mouse test: fields and the START button
    auto click = [&](int x, int y) { gMenu.mouse(x, y, true); Menu::Action r = gMenu.frame(); gMenu.mouse(x, y, false); gMenu.frame(); return r; };
    FILE* lg = std::fopen((dir + "\\menutest.txt").c_str(), "a");
    gMenu.go(Menu::CREATE_E);
    click(274, 228); int t1 = gMenu.spec.enemy[2].type;            // wing 3 type: +
    click(150, 228); click(150, 228); int t2 = gMenu.spec.enemy[2].type;   // twice -: NONE, then wraps to SU-27 (4)
    click(274, 228);                                                // + again: NONE
    click(560, 328); int sn = gMenu.spec.samNum;                    // SAM count field: +1
    gMenu.go(Menu::CREATE_F);
    click(571 + 15, 163); int fn = gMenu.spec.friendly[0].num;      // friendly wing1 count: +
    click(535 + 7, 328); int ab = gMenu.spec.aim9;                  // AIM-9X 4th box
    Menu::Action act = click(520, 450);                               // START MISSION
    if (lg) { std::fprintf(lg, "create: wing3 type +%d, then %d; sam %d; friendly num %d; aim9 %d; start=%d mission=%s created=%d region=%d\n", t1, t2, sn, fn, ab, int(act), gMenu.mission.c_str(), int(gMenu.created), gMenu.spec.region); std::fclose(lg); }
    if (FILE* lg2 = std::fopen((dir + "\\menutest.txt").c_str(), "a")) {
      for (int c = 0; c < 5; c++) std::fprintf(lg2, "campaign %d end (score 0): %s\n", c + 1, gMenu.testCampaignEnd(c, 0).c_str());
      std::fprintf(lg2, "campaign 4 end (score 200000): %s\n", gMenu.testCampaignEnd(3, 200000).c_str());
      std::fclose(lg2);
    }
    gMenu.created = false;
    gMenu.go(Menu::MAIN);
  }
  gMenu.go(Menu::STATS); shot("stats");
  gMenu.setPilotForTest(2, false); gMenu.go(Menu::MEDALS); gMenu.mouse(260, 200, false); shot("medals");   // medals screen, the mouse on the 2nd medal
  MissionResult r; r.success = true; r.airKills = 3; r.playerAir = 2; r.groundKills = 1; r.missiles = 4; r.goalTotal[0] = 1; r.goalDone[0] = 1; r.time = 754;
  gMenu.missionEnded(r); shot("debrief");
  gMenu.drawLoading(*gScreen); gScreen->present(gW, gH); saveBMP(dir + "\\menu_loading.bmp", gW, gH);
  return true;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
  std::string resPath, exePath;
  bool exeTest = false;
  for (int i = 1; i < __argc; i++) {
    std::string a = __argv[i];
    if (a == "--res" && i + 1 < __argc) resPath = __argv[++i];
    else if (a == "--exe" && i + 1 < __argc) exePath = __argv[++i];
    else if (a == "--exetest") exeTest = true;
    else if (a == "--size" && i + 1 < __argc) {         // window size, e.g. --size 1920x1080 (the launcher passes it)
      int w = 0, h = 0;
      if (std::sscanf(__argv[++i], "%dx%d", &w, &h) == 2 && w >= 640 && h >= 400 && w <= 7680 && h <= 4320) { gW = w; gH = h; }
    }
    else if (a == "--autotest") { gAutotest = true; gDirect = true; }
    else if (a == "--topdown" && i + 1 < __argc) { gTopdown = true; gDirect = true; gMission = upper(__argv[++i]); if (i + 1 < __argc) gTopH = float(std::atof(__argv[++i])); }
    else if (a == "--menutest") gMenuTest = true;
    else if (a == "--flowtest") gFlowTest = true;
    else if (a == "--nointro") gNoIntro = true;
    else if (a == "--aivm") gAiVm = true;
    else if (a == "--hd") gForceHd = true;
    else if (a == "--dumptex") { gDumpTexMode = true; gDirect = true; gNoIntro = true; }
    else if (a == "--aiflight") { gAiFlight = true; gAutotest = true; gDirect = true; }
    else if (a == "--aivmtest") { gAiVmTest = true; gAutotest = true; gDirect = true; }
    else if (a == "--videotest" && i + 1 < __argc) { gVideoTest = __argv[++i]; gDirect = false; }
    else if (a == "--tod" && i + 1 < __argc) gTodOverride = std::atoi(__argv[++i]);
    else if (a == "--geartest") { gGearTest = true; gViewTest = true; gAutotest = true; gDirect = true; }
    else if (a == "--joytest") gJoyTest = true;
    else if (a == "--ilstest") { gIlsTest = true; gViewTest = true; gAutotest = true; gDirect = true; }
    else if (a == "--createtest") { gCreateTest = true; gViewTest = true; gAutotest = true; gDirect = true; gMission = "QMC01"; if (i + 1 < __argc && __argv[i + 1][0] != 0x2D) gMission = upper(__argv[++i]); }
    else if (a == "--ejecttest") { gEjectTest = true; gViewTest = true; gAutotest = true; gDirect = true; }
    else if (a == "--shadowtest") { gShadowTest = true; gViewTest = true; gAutotest = true; gDirect = true; }
    else if (a == "--wingtest") { gWingTest = true; gViewTest = true; gAutotest = true; gDirect = true; }
    else if (a == "--fxtest") { gFxTest = true; gViewTest = true; gAutotest = true; gDirect = true; }
    else if (a == "--viewtest") { gViewTest = true; gAutotest = true; gDirect = true; }
    else if (a == "--soundtest") { gSoundTest = true; gAutotest = true; gDirect = true; }
    else { gMission = upper(a); gDirect = true; }
  }
  gJoy.init();
  gPad.init();
  if (gJoyTest) {                                       // joystick test: detection and 5 s sampling into joytest.txt
    FILE* t = std::fopen((exeDir() + "\\joytest.txt").c_str(), "w");
    if (t) {
      std::fprintf(t, "XInput vezérlő: %s\n", gPad.present ? "van" : "nincs");
      if (!gJoy.present) std::fprintf(t, "nincs joystick\n");
      else {
        std::fprintf(t, "joystick: %s, gombok %d, Z %d, R %d, POV %d\n", gJoy.name, gJoy.nbtn, gJoy.hasZ, gJoy.hasR, gJoy.hasPov);
        for (int i = 0; i < 50; i++) {
          gJoy.poll();
          std::fprintf(t, "x %+.2f y %+.2f z %.2f r %+.2f gombok %08lx\n", gJoy.x, gJoy.y, gJoy.z, gJoy.r, gJoy.btn);
          std::fflush(t);
          Sleep(100);
        }
      }
      std::fclose(t);
    }
    return 0;
  }
  std::string found;
  if (!findRes(resPath, found)) {
    MessageBoxW(nullptr, L"RESOURCE.RES not found.\nPut the program in the game folder, or specify: --res <path>",
                L"F-22 Lightning II", MB_ICONERROR);
    return 1;
  }
  std::string base = found.substr(0, found.find_last_of("\\/") + 1);
  // The original program supplies the tables: F22.EXE is decrypted in memory (next to RESOURCE.RES, next to this
  // program, or --exe <path>); an F22_decrypted.bin made by tools\f22unpack.py is still accepted as a fallback.
  std::vector<std::string> exeCand;
  if (!exePath.empty()) exeCand.push_back(exePath);
  exeCand.push_back(base + "F22.EXE");
  exeCand.push_back(exeDir() + "\\F22.EXE");
  std::string exeUsed;
  for (auto& p : exeCand) if (gExe.loadGameExe(p)) { exeUsed = p; break; }
  if (exeUsed.empty()) {
    if (gExe.load(base + "extracted\\F22_decrypted.bin")) exeUsed = base + "extracted\\F22_decrypted.bin";
    else if (gExe.load(base + "F22_decrypted.bin")) exeUsed = base + "F22_decrypted.bin";
  }
  if (exeTest) {   // the in-memory decryption must give the same image as tools\f22unpack.py
    sim::Exe a, b;
    bool okA = false;
    for (auto& p : exeCand) if (a.loadGameExe(p)) { okA = true; break; }
    bool okB = b.load(base + "extracted\\F22_decrypted.bin") || b.load(base + "F22_decrypted.bin");
    FILE* t = std::fopen((exeDir() + "\\exetest.txt").c_str(), "w");
    int rc = 1;
    if (t) {
      std::fprintf(t, "F22.EXE decrypted: %s (%zu bytes)\n", okA ? "yes" : "NO", a.d.size());
      std::fprintf(t, "F22_decrypted.bin: %s (%zu bytes)\n", okB ? "yes" : "NO", b.d.size());
      if (okA && okB) {
        size_t n = 0, first = 0;
        for (size_t i = 0; i < std::min(a.d.size(), b.d.size()); i++) if (a.d[i] != b.d[i]) { if (!n) first = i; n++; }
        if (a.d.size() != b.d.size()) n++;
        std::fprintf(t, "differing bytes: %zu%s\n", n, n ? "" : " -> identical");
        if (n) std::fprintf(t, "first difference at 0x%zx\n", first);
        rc = n ? 1 : 0;
      } else if (okA) rc = 0;   // nothing to compare against, the decryption itself passed its checks
      std::fclose(t);
    }
    return rc;
  }
  if (exeUsed.empty()) {
    MessageBoxW(nullptr, L"F22.EXE (the original game program) not found.\n"
                         L"Put this program in the game folder (next to F22.EXE and RESOURCE.RES), or specify: --exe <path to F22.EXE>",
                L"F-22 Lightning II", MB_ICONERROR);
    return 1;
  }

  WNDCLASSW wc = {};
  wc.style = CS_OWNDC; wc.lpfnWndProc = wndProc; wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = L"F22Native";
  RegisterClassW(&wc);
  RECT r = {0, 0, gW, gH};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  gWnd = CreateWindowW(wc.lpszClassName, L"F-22 Lightning II – native rebuild", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                       CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  HDC dc = GetDC(gWnd);
  PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
  pfd.cDepthBits = 24; pfd.cStencilBits = 8;
  SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
  HGLRC tmp = wglCreateContext(dc);
  wglMakeCurrent(dc, tmp);
  auto createCtx = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(wglGetProcAddress("wglCreateContextAttribsARB"));
  const int attrs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                       WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
  HGLRC ctx = createCtx ? createCtx(dc, nullptr, attrs) : nullptr;
  if (!ctx) { MessageBoxW(gWnd, L"OpenGL 3.3 is not available.", L"F-22", MB_ICONERROR); return 1; }
  wglMakeCurrent(dc, ctx);
  wglDeleteContext(tmp);
  if (!loadGL()) { MessageBoxW(gWnd, L"Missing OpenGL functions.", L"F-22", MB_ICONERROR); return 1; }
  if (auto swap = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(wglGetProcAddress("wglSwapIntervalEXT"))) swap(1);

  gR.init();
  gR.initSky(gRes);
  gHud.init();
  gAudioOn = gSoundTest || !(gAutotest || gMenuTest || gFlowTest || gTopdown);
  if (gAudioOn && !gAudio.init(gRes)) gAudioOn = false;
  std::vector<int16_t> rec;
  if (gSoundTest) gAudio.record = &rec;
  gTexDir = exeDir() + "\\textures\\";
  Models models(gRes);
  gModels = &models;
  std::string err;
  gScreen = new Screen();
  if (!gMenu.init(gRes, gExe, exeDir(), err)) {
    std::wstring w(err.begin(), err.end());
    MessageBoxW(gWnd, w.c_str(), L"F-22", MB_ICONERROR);
    return 1;
  }
  if (!gSmall640.load(gRes, "SMALL640.BMP", gExe) || !gSmall320.load(gRes, "SMALL320.BMP", gExe)) { MessageBoxW(gWnd, L"Missing SMALL640/SMALL320.BMP", L"F-22", MB_ICONERROR); return 1; }
  if (gMenuTest) { gMenu.persist = false; return runMenuTest(dc) ? 0 : 1; }
  auto startMission = [&](const std::string& msn) {
    gMission = msn;
    if (!gDirect) {                                      // loading screen (LOADING.PCX)
      gMenu.drawLoading(*gScreen);
      gScreen->present(gW, gH);
      SwapBuffers(dc);
    }
    std::string e;
    if (!loadAll(e)) {
      std::wstring w(e.begin(), e.end());
      MessageBoxW(gWnd, w.c_str(), L"F-22", MB_ICONERROR);
      return false;
    }
    applyRadii();
    snapshot();
    gCamInit = false; gPaused = false; gAnnounced = false; gTimeScale = 1; gGoals = false;   // [0x531cb] = 0 (0x54d11)
    gSlot = 1; gSlots[1].sub = 0; gCockpit = true;     // start: cockpit (0x5479a)
    gApp = APP_FLIGHT;
    missionAudioStart();
    return true;
  };
  if (gDumpTexMode) {                                   // --dumptex: the textures of every model and terrain tile to PNG (if not already there)
    gDumpTex = true;
    int nm = 0;
    for (auto& n : gRes.names(".PAK")) { std::string b = n.substr(0, n.rfind('.')); if (models.get(b)) nm++; }
    int nt = 0;
    for (int th = 1; th <= 9; th++) { std::vector<uint8_t> rgba; if (!Terrain::buildTiles(gRes, th, rgba)) continue; nt++;
      for (int k = 0; k < 128; k++) { char fn[64]; std::snprintf(fn, sizeof fn, "terrain_T%d_%03d.png", th, k);
        if (FILE* t = std::fopen((gTexDir + fn).c_str(), "rb")) { std::fclose(t); continue; }
        savePng(gTexDir + fn, 128, 128, std::vector<uint8_t>(rgba.begin() + size_t(k) * 65536, rgba.begin() + size_t(k + 1) * 65536)); } }
    if (FILE* lg = std::fopen((exeDir() + "\\dumptex.txt").c_str(), "w")) { std::fprintf(lg, "%d modell, %d terep-csempekeszlet\n", nm, nt); std::fclose(lg); }
    return 0;
  }
  if (gAiFlight) {                                      // --aiflight: one aircraft entirely on the original scripts (taxiing, take-off, route)
    struct Mem { std::vector<uint8_t> b; Mem() : b(0x10000, 0) {} void w32(uint32_t o, int32_t v) { std::memcpy(&b[o], &v, 4); } void w16(uint32_t o, int v) { uint16_t x = uint16_t(v); std::memcpy(&b[o], &x, 2); } } mem;
    const int32_t W = 65536;
    // ground waypoints (0x8000, 12 bytes: x, z, type, angle): towards the runway, then the runway end (type 1: take-off, heading 0)
    mem.w32(0x8000, 0 * W); mem.w32(0x8004, 300 * W); mem.w16(0x8008, 0); mem.w16(0x800a, 0);
    mem.w32(0x800c, 0 * W); mem.w32(0x8010, 600 * W); mem.w16(0x8014, 1); mem.w16(0x8016, 0);
    // air waypoints (0x9000, 16 bytes: x, y, z, class, parameter): a rectangle, at the end a jump to the first
    const int32_t R[4][3] = {{0, 400, 4000}, {3000, 400, 4000}, {3000, 400, 0}, {0, 400, 0}};
    for (int i = 0; i < 4; i++) {                       // 16-byte records: class 0 = the next waypoint, the last 1 = jump to the first
      mem.w32(0x9000 + 16 * i, R[i][0] * W); mem.w32(0x9004 + 16 * i, R[i][1] * W); mem.w32(0x9008 + 16 * i, R[i][2] * W);
      mem.w16(0x900c + 16 * i, i == 3 ? 1 : 0); mem.w16(0x900e + 16 * i, 0);
    }
    sim::OUnit u; sim::OWorld wd; sim::OHost h; sim::AiVm vm(gExe.d);
    h.u = &u; h.img = &gExe.d; h.world = &wd; h.self = wd.add(&u);
    h.groundH = [](int32_t, int32_t) { return 0; };
    h.wpAt = [&](uint32_t p, int32_t& x, int32_t& y, int32_t& z) { std::memcpy(&x, &mem.b[p], 4); if (p >= 0x9000) { std::memcpy(&y, &mem.b[p + 4], 4); std::memcpy(&z, &mem.b[p + 8], 4); } else { y = 0; std::memcpy(&z, &mem.b[p + 4], 4); } };
    h.wpWord = [&](uint32_t p, uint32_t off) { uint16_t v; std::memcpy(&v, &mem.b[p + off], 2); return int32_t(v); };
    const uint8_t* pf = &gExe.d[0x24618 + 0x50 * 0];
    std::memcpy(h.perf, pf, 0x50);
    h.perfOf = [&](const sim::OUnit&) { return pf; };
    u.set(sim::of::X, 0); u.set(sim::of::Z, 0); u.set(sim::of::Y, 0);
    u.set(sim::of::ANG_A, 0); u.set8(sim::of::STATE, 1); u.set8(sim::of::SKILL, 2); u.set8(sim::of::TYPE, 2);
    u.set(sim::of::TAXI, 0x8000); u.set(sim::of::WP_BASE, 0x9000); u.set(sim::of::WP_PTR, 0x9000);
    u.set(sim::of::TURN_RATE, 0x1c0000); u.set(sim::of::TURN_A, 0x100000); u.set(sim::of::TURN_B, 0x180000);
    u.set(sim::of::CMD_ANG, int32_t(0x80000000)); u.set(sim::of::CMD_PITCH, int32_t(0x80000000)); u.set(sim::of::CMD_ROLL, int32_t(0x80000000));
    u.set(sim::of::PENDING, 0x374c8);
    h.rebuildDir();
    std::string out; sim::AiCtx cx;
    int lastState = -1;
    for (int tick = 0; tick < 12000; tick++) {
      u.set(sim::of::TIMER, tick);
      sim::loadCtx(u, cx);
      vm.run(cx, h);
      sim::storeCtx(u, cx);
      h.move();
      int st = u.get8(sim::of::STATE);
      if (tick % 600 == 0 || st != lastState) {
        char t[200]; std::snprintf(t, sizeof t, "t=%5d state=%d pc=%#x pos=(%7.1f %6.1f %7.1f) hdg=%6.1f speed=%d wp=%#x\n", tick, st, cx.pc, u.get(sim::of::X) / 65536.0, u.get(sim::of::Y) / 65536.0, u.get(sim::of::Z) / 65536.0, u.get(sim::of::ANG_A) * 360.0 / 33554432.0, u.get(sim::of::SPEED), u.get(sim::of::WP_PTR));
        out += t; lastState = st;
      }
    }
    if (FILE* lg = std::fopen((exeDir() + "\\aiflight.txt").c_str(), "w")) { std::fputs(out.c_str(), lg); std::fclose(lg); }
    return 0;
  }
  if (gAiVmTest) {                                      // --aivmtest: check of the AI interpreter with the original unit record
    struct Stub : sim::OHost {
      std::string log;
      bool other(int code, const uint8_t*, sim::AiCtx& c) override {
        log += std::string(sim::AiVm::name(code)) + "; ";
        c.flags |= sim::F_CF;                            // the helper routine "done" (carry) – the helper routines are not ported yet
        c.pc += 4;
        return true;
      }
    } st;
    sim::OUnit unit;
    st.u = &unit;
    st.groundH = [](int32_t, int32_t) { return 0; };
    st.img = &gExe.d;
    st.wpAt = [](uint32_t, int32_t& x, int32_t& y, int32_t& z) { x = 40 * 65536; y = 0; z = 90 * 65536; };   // the ground waypoint
    sim::AiVm vm(gExe.d);
    sim::AiCtx cx;
    unit.set(sim::of::PENDING, 0x37550);                 // the "climb after take-off" script
    std::string out;
    for (int tick = 0; tick < 8; tick++) {
      unit.set(sim::of::Y, unit.get(sim::of::Y) + 1800000);   // the aircraft climbs
      st.log.clear();
      sim::loadCtx(unit, cx);
      int n = vm.run(cx, st);
      sim::storeCtx(unit, cx);
      char t[160]; std::snprintf(t, sizeof t, "tick %d: %d utasítás, pc=%#x, Y=%d, state=%d, cmdPitch=%d, flags=%#x: ", tick, n, cx.pc, unit.get(sim::of::Y), unit.get8(sim::of::STATE), unit.get(sim::of::CMD_PITCH), cx.flags);
      out += t + st.log + "\n";
    }
    {                                                    // the ported math and helper routines
      char m[256];
      int32_t sn, cs; st.sinCos(0x400000, sn, cs);       // 45°
      std::snprintf(m, sizeof m, "sin/cos(45): %d %d\n", sn, cs); out += m;
      std::snprintf(m, sizeof m, "atan2: (0,1)=%#x (1,0)=%#x (0,-1)=%#x (-1,0)=%#x (1,1)=%#x\n", st.atan2o(0, 1), st.atan2o(1, 0), st.atan2o(0, -1), st.atan2o(-1, 0), st.atan2o(1, 1)); out += m;
      sim::OUnit w; st.u = &w;
      w.set(sim::of::X, 0); w.set(sim::of::Z, 0); w.set(sim::of::ANG_A, 0);
      int n2 = 0; bool done = false;
      for (; n2 < 400 && !done; n2++) done = st.steerTaxi(1);
      std::snprintf(m, sizeof m, "steerTaxi: %d lepes, irany=%#x, dir=(%d %d %d), sebesseg=%d\n", n2, w.get(sim::of::ANG_A), w.get(sim::of::DIRX), w.get(sim::of::DIRY), w.get(sim::of::DIRZ), w.get(sim::of::SPEED)); out += m;
      w.set(sim::of::ANG_B, 0x300000); w.set(sim::of::ANG_C, 0x1f00000); w.set(sim::of::TURN_A, 0x20000); w.set(sim::of::TURN_B, 0x30000);
      n2 = 0; done = false;
      for (; n2 < 400 && !done; n2++) done = st.levelOut();
      std::snprintf(m, sizeof m, "levelOut: %d lepes, B=%d C=%d\n", n2, w.get(sim::of::ANG_B), w.get(sim::of::ANG_C)); out += m;
      std::snprintf(m, sizeof m, "dist3(3,4,0 x65536)=%d dist3(0,0,0)=%d\n", st.dist3(3 * 65536, 4 * 65536, 0), st.dist3(0, 0, 0)); out += m;
      w.set(sim::of::SPEED, 20000);
      st.matchSpeed(40000, 100, 1000, 0, 500); st.matchSpeed(40000, 100, 1000, 0, 500);
      std::snprintf(m, sizeof m, "matchSpeed: %d\n", w.get(sim::of::SPEED)); out += m;
      {                                                  // unit list and targeting
        sim::OWorld wd; sim::OUnit me, foe, foe2;
        st.world = &wd; st.u = &me;
        st.self = wd.add(&me); uint32_t pf = wd.add(&foe), pf2 = wd.add(&foe2);
        foe.set(sim::of::X, 5000000); foe.set(sim::of::Z, 100000); foe.set(sim::of::TARGET, int32_t(st.self));
        foe2.set(sim::of::X, 900000); foe2.set(sim::of::TARGET, int32_t(st.self));
        std::snprintf(m, sizeof m, "cover: %s\n", st.coverUnit() == pf2 ? "foe2 (a legkozelebbi)" : (st.coverUnit() == pf ? "foe (HIBA)" : "nincs (HIBA)")); out += m;
        st.perf[0x34 / 4] = 20000; me.set(sim::of::SPEED, 20000); me.set(sim::of::TURN_RATE, 0x200000); me.set(sim::of::TURN_A, 0x200000);
        int32_t ye = 0, pe = 0;
        for (int i = 0; i < 400; i++) st.aimAt(3000000, 500000, 8000000, &ye, &pe);
        std::snprintf(m, sizeof m, "aimAt: yawErr=%d pitchErr=%d dir=(%d %d %d)\n", ye, pe, me.get(sim::of::DIRX), me.get(sim::of::DIRY), me.get(sim::of::DIRZ)); out += m;
        {                                                // firing and evasion script
          int shots = 0; bool lastMissile = false;
          st.spawnShot = [&](bool m) { shots++; lastMissile = m; };
          st.perf[0x38 / 4] = 0x0f0f0f0f; st.perf[0x3c / 4] = 0x0f0f0f0f;   // for every skill level the 4 lower evasions
          me.set8(sim::of::TYPE, 1); me.set8(sim::of::INCOMING, 1); me.set8(sim::of::SKILL, 3); me.set8(sim::of::STATE, 3); me.set(sim::of::TIMER, 128);
          bool r1 = st.fireCheck();
          std::snprintf(m, sizeof m, "fireCheck: %d, loves=%d rakéta=%d, allapot=%d, belepo=%#x\n", r1, shots, lastMissile, me.get8(sim::of::STATE), me.get(sim::of::PENDING)); out += m;
          me.set8(sim::of::STATE, 3); me.set(sim::of::TIMER, 1);
          std::snprintf(m, sizeof m, "fireCheck (nem 128): %d\n", st.fireCheck()); out += m;
        }
        {                                                // target finder and wing allocation
          sim::OUnit w1, w2, e1, e2;
          uint32_t pw1 = wd.add(&w1), pw2 = wd.add(&w2), pe1 = wd.add(&e1), pe2 = wd.add(&e2);
          (void)pw1; (void)pw2;
          for (sim::OUnit* x : {&e1, &e2}) { x->set8(sim::of::SIDE_FLAGS, 1); x->set8(sim::of::TYPE, 1); x->set(sim::of::STEALTH, 65536); }
          e1.set(sim::of::X, 20000000); e2.set(sim::of::X, 40000000);
          foe.set8(sim::of::SIDE_FLAGS, 0); foe2.set8(sim::of::SIDE_FLAGS, 0);
          for (sim::OUnit* x : {&w1, &w2}) { x->set8(sim::of::STATE, 4); x->set8(sim::of::GROUP_ID, 1); }
          me.set8(sim::of::F7, 1); me.set8(sim::of::F9, 0); me.set(sim::of::WING_NEXT, int32_t(pw1)); w1.set(sim::of::WING_NEXT, int32_t(pw2));
          st.assignWing();
          std::snprintf(m, sizeof m, "assignWing: w1->%s w2->%s belepo=%#x\n", uint32_t(w1.get(sim::of::TARGET)) == pe1 ? "e1" : "?", uint32_t(w2.get(sim::of::TARGET)) == pe2 ? "e2" : "?", w1.get(sim::of::PENDING)); out += m;
        }
        {                                                // pursuit: attack step on one target
          sim::OUnit a1, t1; sim::OWorld w2; sim::OHost* hp = &st;
          st.world = &w2; st.u = &a1;
          st.self = w2.add(&a1); uint32_t pt = w2.add(&t1);
          t1.set(sim::of::X, 3000000); t1.set(sim::of::Y, 100000); t1.set(sim::of::Z, 6000000); t1.set(sim::of::SPEED, 30000); t1.set8(sim::of::TYPE, 1);
          a1.set(sim::of::TARGET, int32_t(pt)); a1.set(sim::of::SPEED, 25000); a1.set(sim::of::TURN_RATE, 0x200000); a1.set(sim::of::TURN_A, 0x200000);
          a1.set(sim::of::MIN_RANGE, 0x7fffffff);
          st.perf[8 / 4] = 20000; st.perf[0x20 / 4] = 500; st.perf[0x34 / 4] = 20000;
          bool ok = false; int n3 = 0;
          for (; n3 < 400 && !ok; n3++) { a1.set(sim::of::TIMER, n3 * 4 + 1); ok = st.attackStep(); }
          std::snprintf(m, sizeof m, "attackStep: %d lepes, kesz=%d, sebesseg=%d, A=%d B=%d dir=(%d %d %d)\n", n3, ok, a1.get(sim::of::SPEED), a1.get(sim::of::ANG_A), a1.get(sim::of::ANG_B), a1.get(sim::of::DIRX), a1.get(sim::of::DIRY), a1.get(sim::of::DIRZ)); out += m;
          (void)hp; st.world = nullptr; st.u = &unit;
        }
        {                                                // formation flying and commanded turn
          sim::OUnit ld, wg; sim::OWorld w3;
          st.world = &w3; st.u = &wg;
          uint32_t pl = w3.add(&ld); st.self = w3.add(&wg);
          wg.set(sim::of::LEADER, int32_t(pl));
          ld.set(sim::of::X, 1000000); ld.set(sim::of::Z, 3000000); ld.set(sim::of::SPEED, 30000);
          st.slotVec = [](const sim::OUnit&, int32_t v[3]) { v[0] = -500000; v[1] = 0; v[2] = -400000; };
          st.perf[0] = 40000; st.perf[0x34 / 4] = 40000; wg.set(sim::of::SPEED, 20000);
          wg.set(sim::of::TURN_RATE, 0x2000000); wg.set(sim::of::TURN_A, 0x2000000); wg.set(sim::of::TURN_B, 0x30000);
          for (int i = 0; i < 6000; i++) { wg.set(sim::of::TIMER, i * 4 + 1); st.formationStep(); wg.set(sim::of::X, wg.get(sim::of::X) + int32_t((int64_t(wg.get(sim::of::DIRX)) * wg.get(sim::of::SPEED)) >> 16)); wg.set(sim::of::Z, wg.get(sim::of::Z) + int32_t((int64_t(wg.get(sim::of::DIRZ)) * wg.get(sim::of::SPEED)) >> 16)); ld.set(sim::of::Z, ld.get(sim::of::Z) + 30000); }
          std::snprintf(m, sizeof m, "formationStep: tav=%d sebesseg=%d (vezer 30000)\n", st.dist3to(ld), wg.get(sim::of::SPEED)); out += m;
          const int32_t NONE = int32_t(0x80000000);
          wg.set(sim::of::ANG_A, 0); wg.set(sim::of::ANG_B, 0); wg.set(sim::of::ANG_C, 0);
          wg.set(sim::of::CMD_ANG, 0x300000); wg.set(sim::of::CMD_PITCH, 0x100000); wg.set(sim::of::CMD_ROLL, NONE);
          wg.set(sim::of::SPEED, 40000); st.perf[0x34 / 4] = 100000;
          bool dn = false; int n4 = 0;
          for (; n4 < 400 && !dn; n4++) { wg.set(sim::of::TIMER, n4 * 4 + 1); dn = st.maneuver(); }
          std::snprintf(m, sizeof m, "maneuver: %d lepes, kesz=%d, A=%#x B=%#x\n", n4, dn, wg.get(sim::of::ANG_A), wg.get(sim::of::ANG_B)); out += m;
        }
        {                                                // firing and pursuit
          sim::OUnit me2, tg, gl, wgm; sim::OWorld w4;
          st.world = &w4; st.u = &me2;
          st.self = w4.add(&me2); uint32_t ptg = w4.add(&tg); uint32_t pgl = w4.add(&gl); uint32_t pwg = w4.add(&wgm);
          me2.set(sim::of::TARGET, int32_t(ptg)); tg.set(sim::of::X, 1000000); tg.set8(sim::of::TYPE, 1); tg.set8(sim::of::GROUP_IDX, 2);
          gl.set8(sim::of::F9, 1); gl.set(sim::of::WING_NEXT, int32_t(pwg)); wgm.set8(sim::of::STATE, 4);
          st.groupLeader = [&](int idx) { return idx == 2 ? pgl : 0u; };
          int kinds[4] = {-1, -1, -1, -1}; int nk = 0;
          st.weapon = [&](int k) { if (nk < 4) kinds[nk++] = k; };
          bool r1 = st.fireAtTarget();
          tg.set(sim::of::X, 100000000);
          bool r2 = st.fireAtTarget();
          me2.set(sim::of::TIMER, 64); me2.set(sim::of::Y, 50000000); tg.set(sim::of::Y, 50000000); st.perf[8 / 4] = 20000; st.perf[0x20 / 4] = 500;
          st.pursuitStep();
          std::snprintf(m, sizeof m, "fireAtTarget: kozel=%d (fegyver %d), tavol=%d (fegyver %d); ertesites: szarnyas->%s belepo=%#x\n", r1, kinds[0], r2, kinds[1], uint32_t(wgm.get(sim::of::TARGET)) == st.self ? "rank indul" : "HIBA", wgm.get(sim::of::PENDING)); out += m;
          {                                                // take-off, VMAX, TGT, FORM
          sim::OUnit g; sim::OWorld w5; st.world = &w5; st.u = &g; st.self = w5.add(&g);
          st.perf[0] = 90000; st.perf[1] = 30000; st.perf[2] = 60000; st.perf[0x20 / 4] = 2000;
          int ev = 0; bool lift = false; st.onEvent = [&](int, int) { ev++; }; st.onLiftoff = [&] { lift = true; };
          sim::AiCtx cx2; g.set8(sim::of::TYPE, 2); g.set(sim::of::SPEED, 0);
          int steps = 0;
          for (; steps < 100; steps++) { st.takeoffStep(cx2); if (cx2.flags & 1) break; }
          std::snprintf(m, sizeof m, "takeoff: %d lepes, sebesseg=%d, allapot=%d, tipus=%d, esemeny=%d, felszallt=%d\n", steps + 1, g.get(sim::of::SPEED), g.get8(sim::of::STATE), g.get8(sim::of::TYPE), ev, lift); out += m;
          uint8_t ins[4] = {0x29, 1, 0, 0}; g.set8(sim::of::SKILL, 2);
          st.op(0x29, ins, cx2);
          std::snprintf(m, sizeof m, "VMAX(1): cmdSpeed=%d\n", g.get(sim::of::CMD_SPEED)); out += m;
          {                                                // ACT, CLS
          sim::OUnit lead, wing, enemy; sim::OWorld w6; st.world = &w6; st.u = &wing;
          uint32_t pl2 = w6.add(&lead); st.self = w6.add(&wing); uint32_t pen = w6.add(&enemy);
          wing.set(sim::of::LEADER, int32_t(pl2)); lead.set(sim::of::TARGET, int32_t(pen));
          st.actOp(1);
          std::snprintf(m, sizeof m, "ACT 1 (a vezer celja): %s\n", uint32_t(wing.get(sim::of::TARGET)) == pen ? "ok" : "HIBA"); out += m;
          wing.set(sim::of::WP_PTR, 0x1000); wing.set(sim::of::WP_BASE, 0x800);
          st.wpWord = [](uint32_t, uint32_t off) { return off == 0xe ? 3 : 0; };
          st.classOp(1);
          std::snprintf(m, sizeof m, "CLS 1 (ugras): WP_PTR=%#x\n", wing.get(sim::of::WP_PTR)); out += m;
          {                                                // line of sight and movement
          sim::OUnit p1, p2; sim::OWorld w7; st.world = &w7; st.u = &p1; st.self = w7.add(&p1); w7.add(&p2);
          p1.set(sim::of::X, 0); p1.set(sim::of::Z, 0); p1.set(sim::of::Y, 5000000);
          p2.set(sim::of::X, 200000000); p2.set(sim::of::Z, 0); p2.set(sim::of::Y, 5000000);
          st.groundH = [](int32_t, int32_t) { return 0; };
          bool free1 = st.visible(p2);
          st.groundH = [](int32_t x, int32_t) { return (x > 90000000 && x < 110000000) ? 100000000 : 0; };   // a mountain between the two aircraft
          bool free2 = st.visible(p2);
          st.groundH = [](int32_t, int32_t) { return 0; };
          p1.set(sim::of::DIRZ, 65536); p1.set(sim::of::SPEED, 30000); st.move();
          std::snprintf(m, sizeof m, "visible: sik=%d hegy=%d; move: Z=%d\n", free1, free2, p1.get(sim::of::Z)); out += m;
          st.world = nullptr; st.u = &unit;
        }
        st.world = nullptr; st.u = &unit;
        }
        st.world = nullptr; st.u = &unit;
        }
        st.world = nullptr; st.u = &unit;
        }
        st.world = nullptr; st.u = &unit;
      }
      st.u = &unit;
    }
    {                                                    // group handling
      sim::OUnit L, A, B, Cc; sim::OWorld gw; sim::OGroups gr; gr.w = &gw; gr.img = &gExe.d;
      int ev = 0; gr.onEvent = [&](int, int, int) { ev++; };
      uint32_t pL = gw.add(&L), pA = gw.add(&A), pB = gw.add(&B), pC = gw.add(&Cc);
      gr.addLeader(2, pL); gr.addWing(2, 1, pA, 0x500); gr.addWing(2, 2, pB, 0x514); gr.addWing(2, 3, pC, 0x528);
      for (sim::OUnit* x : {&L, &A, &B, &Cc}) x->set8(sim::of::GROUP_IDX, 2);
      gr.buildChains();
      bool chain = uint32_t(L.get(sim::of::WING_NEXT)) == pA && uint32_t(A.get(sim::of::WING_NEXT)) == pB && uint32_t(B.get(sim::of::WING_NEXT)) == pC && uint32_t(Cc.get(sim::of::LEADER)) == pL;
      B.set8(sim::of::STATE, 4); A.set8(sim::of::STATE, 4);
      gr.wingDied(pB);
      bool unl = uint32_t(A.get(sim::of::WING_NEXT)) == pC;
      L.set8(sim::of::STATE, 4);
      gr.leaderDied(pL);
      bool prom = gr.leader[2] == pA && uint32_t(Cc.get(sim::of::LEADER)) == pA && A.get(sim::of::DEATH) == int32_t(sim::OGroups::H_LEADER);
      char m[200]; std::snprintf(m, sizeof m, "csoport: lanc=%d kilancolas=%d vezerhalal=%d (uj vezer belepo=%#x)\n", chain, unl, prom, A.get(sim::of::PENDING)); out += m; (void)ev;
    }
    sim::AiRng r1, r2;
    uint32_t q1 = r1.next(), q2 = r1.next(), q3 = r1.next();
    char t2[96]; std::snprintf(t2, sizeof t2, "rng: %u %u %u\n", q1, q2, q3);
    out += t2; (void)r2;
    if (FILE* lg = std::fopen((exeDir() + "\\aivmtest.txt").c_str(), "w")) { std::fputs(out.c_str(), lg); std::fclose(lg); }
    return 0;
  }
  if (!gVideoTest.empty()) {                            // --videotest: one video, with screenshots and timing
    auto t0 = std::chrono::steady_clock::now();
    bool full = playVideo(dc, gVideoTest);
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (FILE* lg = std::fopen((exeDir() + "\\videotest.txt").c_str(), "w")) { std::fprintf(lg, "%s: lejátszva %s, %.2f s\n", gVideoTest.c_str(), full ? "végig" : "megszakítva", el); std::fclose(lg); }
    return 0;
  }
  if (!gDirect && !gMenuTest && !gFlowTest && !gNoIntro && !gSoundTest && !gAutotest)   // 0x8a954: Dolby, Nova, F-22 intro
    playVideos(dc, {"HDOLBY.KDV", "NOVA_640.KDV", "HF22INTR.KDV"});
  static sim::CreateSpec testSpec;
  if (gCreateTest) {
    testSpec.enemy[0] = {3, 2, 3}; testSpec.enemy[1] = {4, 1, 2};
    testSpec.friendly[0] = {3, 2, 2};
    testSpec.samNum = 3; testSpec.samSkill = 1; testSpec.aaaNum = 4; testSpec.ground = 1; testSpec.tod = 3;
    testSpec.aim9 = 3; testSpec.amraam = 5; testSpec.jdam = 2;
    gCreateSpec = &testSpec;
  }
  if (gDirect && !startMission(gMission)) return 1;
  if (!gDirect) menuMusic();

  AutoTest at;
  if (gAutotest) { at.dir = exeDir(); at.log = std::fopen((at.dir + (gViewTest ? "\\viewtest.txt" : "\\autotest.txt")).c_str(), "w"); }
  if (gViewTest) at.step = gGearTest ? 200 : gFxTest ? 300 : gWingTest ? 400 : gShadowTest ? 500 : gIlsTest ? 600 : gCreateTest ? 700 : gEjectTest ? 800 : 100;
  auto last = std::chrono::steady_clock::now();
  double acc = 0;
  while (gRunning) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    auto nowT = std::chrono::steady_clock::now();
    double dt = std::min(0.25, std::chrono::duration<double>(nowT - last).count());
    last = nowT;
    if (gW <= 0 || gH <= 0) { Sleep(10); continue; }
    if (gFlowTest) {                                     // menu -> mission -> debriefing -> another mission
      gFlowFrame++;
      static const char* FL[2] = {"QCKMIS3", "C01M01"};
      static int k = 0;
      if (gApp == APP_MENU && gFlowFrame % 120 == 30) {
        if (k == 2) { saveBMP(exeDir() + "\\flow_debrief.bmp", gW, gH); break; }
        gMenu.mission = FL[k++]; gWantStart = true;
      }
      if (gApp == APP_FLIGHT && gFlowFrame % 120 == 100) { saveBMP(exeDir() + "\\flow_" + gMission + ".bmp", gW, gH); gWantEnd = true; }
    }
    applySettings();
    if (gApp == APP_MENU) {
      int mx = -1, my = -1;
      if (!gScreen->toScreen(gMouseX, gMouseY, gW, gH, mx, my)) mx = my = -1;
      gMenu.mouse(mx, my, gMouseDown);
      if (gMenu.settings.fullscreen != gFullscreen) toggleFullscreen(gWnd);   // the menu's toggle (OPTIONS 2)
      if (gMenu.settings.hdTex != gHdTex) {                                   // HD textures (OPTIONS 2): re-upload of the loaded textures
        gHdTex = gMenu.settings.hdTex;
        if (gModels) gModels->reloadTextures();
        if (gTerrain.tiles) gTerrain.uploadTiles();
      }
      Menu::Action a = gMenu.frame();
      if (gMenu.clicks != gMenuClicks) { gMenuClicks = gMenu.clicks; if (gAudioOn) gAudio.play(gAudio.load("CLICK.WAV"), 0xC8 / 255.0f); }   // 0xe0b3
      if (a == Menu::QUIT) break;
      if (!gMenu.videos.empty()) { auto v = gMenu.videos; gMenu.videos.clear(); playVideos(dc, v); last = std::chrono::steady_clock::now(); continue; }
      if (a == Menu::START) gWantStart = true;
      if (gWantStart) {
        gWantStart = false;
        gCreateSpec = gMenu.created ? &gMenu.spec : nullptr;
        if (!startMission(gMenu.mission)) break;
        last = std::chrono::steady_clock::now(); acc = 0;
        continue;
      }
      gMenu.draw(*gScreen);
      gScreen->present(gW, gH);
      SwapBuffers(dc);
      continue;
    }
    if (gApp == APP_FLIGHT && gJoy.present && !gAutotest) {   // joystick: buttons and throttle axis
      gJoy.poll();
      if (GetForegroundWindow() == gWnd && !gPaused && !W.crashed) {
        if (gJoy.pressed(1)) W.cycleWeapon();
        if (gJoy.pressed(2)) W.cycleTargets(true, 1);
        if (gJoy.pressed(3)) W.countermeasures();
        if (gJoy.pressed(4)) W.toggleGear();
        if (gJoy.pressed(5)) W.toggleFlaps();
        if (gJoy.pressed(7)) W.toggleRadar();
        if (gJoy.pressed(8)) selectView(gSlot == 1 ? 2 : 1);
        if (gJoy.hasZ) {                                // the 3rd axis is the throttle (0x614f8): only on change, so the keys work too
          float v = 1.0f - gJoy.z;                      // pushed forward = high thrust
          int pct = v < 0.04f ? 0 : v < 0.5f ? 60 + int(v / 0.5f * 40) : v < 0.96f ? 100 : 110;
          if (pct != gJoy.lastThr) { if (gJoy.lastThr >= 0) W.throttleKey(pct); gJoy.lastThr = pct; }
        }
      }
    }
    if (gApp == APP_FLIGHT && !gAutotest && gPad.getState) {   // game controller buttons
      gPad.poll();
      if (gPad.present && GetForegroundWindow() == gWnd && !W.crashed) {
        if (gPad.pressed(Pad::START)) gPaused = !gPaused;
        if (!gPaused) {
          if (gPad.pressed(Pad::A)) W.cycleWeapon();
          if (gPad.pressed(Pad::B)) W.countermeasures();
          if (gPad.pressed(Pad::X)) W.cycleTargets(true, 1);
          if (gPad.pressed(Pad::Y)) selectView(gSlot == 1 ? 2 : 1);
          if (gPad.pressed(Pad::LB)) W.toggleGear();
          if (gPad.pressed(Pad::RB)) W.toggleFlaps();
          if (gPad.pressed(Pad::LEFT)) W.cycleTargets(true, -1);
          if (gPad.pressed(Pad::RIGHT)) W.toggleRadar();
          if (gPad.pressed(Pad::RS)) W.nearestTarget(true);
          if (gPad.pressed(Pad::LS)) W.toggleAutopilot(1);
          // d-pad up / down: throttle by one step (R / Z); repeats while held
          bool ud = gPad.down(Pad::UP) || gPad.down(Pad::DOWN);
          if (gPad.pressed(Pad::UP)) { W.throttleStep(1); gPadRepeat = 0; }
          else if (gPad.pressed(Pad::DOWN)) { W.throttleStep(-1); gPadRepeat = 0; }
          else if (ud && ++gPadRepeat >= 20) { W.throttleStep(gPad.down(Pad::UP) ? 1 : -1); gPadRepeat = 0; }
          if (gPad.pressed(Pad::BACK)) gWantEnd = true;
        }
      }
    }
    if (gWantEnd) {                                      // end of mission -> debriefing
      gWantEnd = false;
      gMenu.missionEnded(missionResult());
      menuMusic();
      gApp = APP_MENU;
      continue;
    }
    if (!gAnnounced && (W.primaryDone() || W.goalDone[3] > 0)) {   // 0x222ae / 0x22316
      gAnnounced = true;
      uint32_t a = W.primaryDone() ? 0x222ae : 0x22316;
      std::string t;
      while (a < gExe.d.size() && gExe.d[a]) t += char(gExe.d[a++]);
      W.say(t, 10);
    }
    // fixed 40 Hz simulation steps (like the original main loop, 0xF039)
    if (gAutotest) {
      for (int k = 0; k < 4 && gRunning; k++) {          // accelerated: 4 steps per frame
        if (!at.tick()) gRunning = false;
        snapshot();
        W.step(readControls());
        gFx.step(W);
        viewStep();
        audioStep();
      }
    } else if (!gPaused) {
      acc += dt;
      int n = 0;
      acc += dt * (gTimeScale - 1);                     // F9..F12: in the original the game time advances this much per tick (0x61128)
      while (acc >= sim::STEP_S && n < 20 * gTimeScale) { snapshot(); W.step(readControls()); gFx.step(W); viewStep(); audioStep(); acc -= sim::STEP_S; n++; }
      if (n == 20 * gTimeScale) acc = 0;
    }
    gAlpha = (gAutotest || gPaused) ? 1.0 : std::clamp(acc / sim::STEP_S, 0.0, 1.0);
    if (gTopdown) {                                      // debugging: top-down view above the airfield
      sim::Unit* rw = nullptr;
      for (auto& u : W.units) if (u.cls == sim::C_RUNWAY) { rw = &u; break; }
      D3 c = rw ? rw->pos : W.player->pos;
      gCamPos = {float(c.x * sim::WU_M), gTerrain.heightAt(float(c.x * sim::WU_M), float(c.z * sim::WU_M)) + gTopH, float(c.z * sim::WU_M)};
      gCamF = {0, -1, 0}; gCamUp = {0, 0, 1}; gCamInit = true;
      if (gTopH < 0 && rw) {                              // negative altitude: low oblique view along the runway
        D3 fw = sim::dirOf(rw->hdg, 0), cp = rw->pos - fw * (gTopH < -20 ? 350.0 : 60.0);
        gCamPos = {float(cp.x * sim::WU_M), gTerrain.heightAt(float(cp.x * sim::WU_M), float(cp.z * sim::WU_M)) - gTopH, float(cp.z * sim::WU_M)};
        V3 tgt{float(rw->pos.x * sim::WU_M), gTerrain.heightAt(float(rw->pos.x * sim::WU_M), float(rw->pos.z * sim::WU_M)), float(rw->pos.z * sim::WU_M)};
        gCamF = norm(tgt - gCamPos);
        gCamUp = norm(V3{0, 1, 0} - gCamF * gCamF.y);
      }
      render(gW, gH);
      saveBMP(exeDir() + "\\topdown_" + gMission + ".bmp", gW, gH);
      break;
    }
    updateCamera(float(dt));
    audioFrame();
    if (curMode() == VM_MFD && !W.crashed) {
      drawBezel(gSlot - 8);
      gScreen->present(gW, gH);
      drawMfd(gHud, gW, gH, gSlot - 8);
    } else {
      if (gCockpit) updateCockpitMfds();
      render(gW, gH);
      if (gMenu.settings.gEffects) { gEffectUpdate(float(dt)); if (!gAutotest || std::getenv("F22_GTEST")) drawGEffect(); }
      else gBlack = gRed = 0;                                   // G effect off (OPTIONS 2)
      drawHud(gW, gH);
    }
    if (gAutotest && at.shot) { saveBMP(at.dir + "\\" + at.shotName + ".bmp", gW, gH); at.shot = false; }
    SwapBuffers(dc);
  }
  if (at.log) std::fclose(at.log);
  if (gSoundTest) {                                     // the mixer output to WAV (for verification)
    gAudio.record = nullptr;
    if (FILE* t = std::fopen((exeDir() + "\\soundtest.txt").c_str(), "w")) {
      for (int i = 0; i < 64; i++) if (gSndCount[i]) std::fprintf(t, "id %2d %-12s x%d\n", i, i < 40 && SND_NAME[i] ? SND_NAME[i] : "(radio)", gSndCount[i]);
      std::fprintf(t, "music: %s\n", gAudio.musicCurrent().c_str());
      for (auto& m : gMusLog) std::fprintf(t, "  %s\n", m.c_str());
      std::fclose(t);
    }
    Sleep(100);
    FILE* f = std::fopen((exeDir() + "\\soundtest.wav").c_str(), "wb");
    if (f) {
      uint32_t n = uint32_t(rec.size() * 2), v;
      std::fwrite("RIFF", 1, 4, f); v = 36 + n; std::fwrite(&v, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
      const uint16_t fmt[8] = {16, 0, 1, 2, 0xAC44, 0, 0xB110, 0x0002};
      std::fwrite(fmt, 2, 8, f); const uint16_t ba[2] = {4, 16}; std::fwrite(ba, 2, 2, f);
      std::fwrite("data", 1, 4, f); std::fwrite(&n, 4, 1, f); std::fwrite(rec.data(), 2, rec.size(), f);
      std::fclose(f);
    }
  }
  if (gAudioOn) gAudio.shutdown();
  return 0;
}
