#include "menu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gl.h"

namespace {

enum {
  A_QUICK = 1, A_NEW, A_CONT, A_MULTI, A_OPTIONS, A_CREDITS, A_EXIT,
  A_Q_ACCEPT = 10, A_Q_CREATE, A_Q_BACK, A_Q_PREV = 20, A_Q_NEXT,
  A_N_ACCEPT = 30, A_N_BACK,
  A_C_BACK = 40,
  A_B_START = 50, A_B_MAP, A_B_BACK, A_B_STATS, A_B_PREV, A_B_NEXT,
  A_M_BACK = 60, A_M_UP, A_M_DOWN, A_M_LEFT, A_M_RIGHT,
  A_O_BACK = 70, A_O_MORE,
  A_S_BACK = 80, A_S_MEDALS, A_MD_BACK,
  A_D_CONT = 90,
  A_O2_BACK = 95,
  A_SFX_M = 400, A_SFX_P, A_SFX_BAR, A_MUS_M, A_MUS_P, A_MUS_BAR, A_TEX_M, A_TEX_P, A_TEX_BAR, A_SKY_H, A_SKY_L,
  A_JOY_M, A_JOY_P, A_JOY_BAR, A_FS, A_GE_ON, A_GE_OFF, A_HD_ON, A_HD_OFF,                          // A_FS: windowed / fullscreen (addition)
  A_CR_EXIT = 900, A_CR_FRIENDLY, A_CR_ENEMY, A_CR_START,   // Create Mission: gombok
  A_CR_FIELD = 1000,                                         // + 2 * field + (0: -, 1: +)
  A_CR_WEAPON = 1200,                                        // + 5 * row + column
  A_ROW_Q = 100, A_ROW_N = 200, A_ROW_C = 300,
};

const char* BG[13] = {"MAIN.PCX", "Q_LOGO.PCX", "NC_LOGO.PCX", "CC_LOGO.PCX", "MLOGO.PCX",
                     "MISSMAP.PCX", "OPT_LOGO.PCX", "PLYRSTAT.PCX", "PLYRSTAT.PCX", "MOOPTS.PCX", "ENEMY.PCX", "FRIENDLY.PCX", "MED_TEST.PCX"};

std::string cstr(const sim::Exe& e, uint32_t a) {
  std::string s;
  while (a < e.d.size() && e.d[a]) s += char(e.d[a++]);
  return s;
}
std::string upperS(std::string s) {
  for (char& c : s) if (c >= 'a' && c <= 'z') c -= 32;
  return s;
}
// a number replaces "##" (the original 0x56741 formatter)
std::string fmtHash(std::string s, int v) {
  size_t p = s.find('#');
  if (p == std::string::npos) return s + std::to_string(v);
  size_t q = p;
  while (q < s.size() && s[q] == '#') q++;
  return s.substr(0, p) + std::to_string(v) + s.substr(q);
}

}  // namespace

Menu::Widget Menu::wid(uint32_t a, int kind, int id, bool disabled) {
  Widget w;
  for (int k = 0; k < 3; k++) {
    uint32_t src = uint32_t(exe_->i32(a + k * 12));
    w.sx[k] = int(src & 0xFFFF); w.sy[k] = int(src >> 16);
  }
  uint32_t sz = uint32_t(exe_->i32(a + 4)), pos = uint32_t(exe_->i32(a + 40));
  w.w = int(sz & 0xFFFF); w.h = int(sz >> 16);
  w.x = int(pos & 0xFFFF); w.y = int(pos >> 16);
  w.kind = kind; w.id = id; w.disabled = disabled;
  if (kind == 0) { w.hitX = w.x - 6; w.hitY = w.y - 3; w.hitW = 248; w.hitH = w.h + 6; }   // the whole button strip
  else { w.hitX = w.x; w.hitY = w.y; w.hitW = w.w; w.hitH = w.h; }
  return w;
}

bool Menu::init(const Res& res, const sim::Exe& exe, const std::string& saveDir, std::string& err) {
  res_ = &res; exe_ = &exe;
  savePath_ = saveDir + "\\f22native.sav";
  if (!mstrip_.load(res, "MSTRIP.PCX")) { err = "Missing menu image: MSTRIP.PCX"; return false; }
  for (int i = 0; i < 13; i++) if (!bg_[i].load(res, BG[i])) { err = std::string("Missing menu image: ") + BG[i]; return false; }
  if (!buttons_.load(res, "BUTTONS.PCX") || !cursor_.load(res, "CURSOR.PCX") || !loading_.load(res, "LOADING.PCX")) {
    err = "Missing menu graphics (BUTTONS/CURSOR/LOADING.PCX)"; return false;
  }
  const char* maps[4] = {"BLUEISLD.PCX", "BLUESNOW.PCX", "BLUEDSRT.PCX", "BLUEJNGL.PCX"};
  for (int i = 0; i < 4; i++) mapBg_[i].load(res, maps[i]);
  icons_[0].load(res, "ICONS_G.PCX"); icons_[1].load(res, "ICONS_R.PCX"); icons_[2].load(res, "ICONS_B.PCX");
  if (!fat_.load(res, "FATFNT.BMP", exe)) { err = "Missing font: FATFNT.BMP"; return false; }
  for (int i = 0; i < 4; i++) regions_[i] = cstr(exe, uint32_t(exe.i32(0x1184e + i * 4)));   // ~2GREEN ISLAND ...
  for (int i = 0; i < 5; i++) tods_[i] = cstr(exe, uint32_t(exe.i32(0x1185e + i * 4)));      // " - DAY" ...

  // missions: F22.MSN (quick missions, then campaigns), titles and descriptions from the ORF headers
  Blob m = res.get("F22.MSN");
  if (!m.ok()) { err = "Missing F22.MSN"; return false; }
  auto readMsn = [&](size_t o, int camp, int idx) {
    Msn r;
    char nm[16] = {};
    std::memcpy(nm, m.p + o + 1, 15);
    r.name = nm;
    if (r.name.size() > 4) r.name = r.name.substr(0, r.name.size() - 4);
    r.camp = camp; r.idx = idx;
    Blob orf = res.get(r.name + ".ORF");
    if (orf.ok() && orf.n > 0x288) {
      r.region = orf.p[0] & 15; r.tod = (orf.p[0] >> 4) - 1;
      for (int k = 0; k < 3; k++) r.ammo[k] = orf.p[1 + k];
      r.fuel = (orf.p[0x7D] + 1) * 5000;
      r.title = upperS(std::string(reinterpret_cast<const char*>(orf.p + 4), strnlen(reinterpret_cast<const char*>(orf.p + 4), 0x7C)));
      r.desc = std::string(reinterpret_cast<const char*>(orf.p + 0x84), strnlen(reinterpret_cast<const char*>(orf.p + 0x84), 0x204));
    }
    return r;
  };
  size_t o = 0;
  uint32_t nq = m.u32(o); o += 4;
  for (uint32_t i = 0; i < nq; i++, o += 16) quick_.push_back(readMsn(o, -1, int(i)));
  o += 4;
  uint32_t nc = m.u32(o); o += 4;
  for (uint32_t c = 0; c < nc && o < m.n; c++) {
    uint32_t k = m.u32(o); o += 4;
    camps_.emplace_back();
    for (uint32_t i = 0; i < k; i++, o += 16) camps_.back().push_back(readMsn(o, int(c), int(i)));
  }

  // widgets from the original tables
  for (int k = 0; k < 7; k++)
    w_[MAIN].push_back(wid(0x12190 + k * 0x44, 0, A_QUICK + k, k == 3));   // multiplayer: none
  w_[QUICK] = {wid(0x11460, 0, A_Q_ACCEPT), wid(0x114a4, 0, A_Q_CREATE), wid(0x114e8, 0, A_Q_BACK),
               wid(0x116c4, 2, A_Q_PREV), wid(0x11680, 2, A_Q_NEXT)};
  for (int k = 0; k < 5; k++) w_[QUICK].push_back(wid(0x1152c + k * 0x44, 1, A_ROW_Q + k));
  w_[NEWCAMP] = {wid(0x184b0, 0, A_N_ACCEPT), wid(0x184f4, 0, A_N_BACK)};
  for (int k = 0; k < 7; k++) w_[NEWCAMP].push_back(wid(0x18538 + k * 0x44, 1, A_ROW_N + k));
  w_[CONTCAMP] = {wid(0x180e0, 0, A_C_BACK)};
  for (int k = 0; k < 7; k++) w_[CONTCAMP].push_back(wid(0x18124 + k * 0x44, 0, A_ROW_C + k));
  w_[BRIEF] = {wid(0x14ac0, 0, A_B_START), wid(0x14b04, 0, A_B_MAP), wid(0x14b48, 0, A_B_BACK), wid(0x14b8c, 0, A_B_STATS),
               wid(0x14bd0, 2, A_B_PREV), wid(0x14c14, 2, A_B_NEXT)};
  w_[MAP] = {wid(0x1b3e0, 0, A_M_BACK)};
  {                                                          // the arrow pad is on the background image
    auto pad = [&](int x, int y, int id) { Widget w{}; w.kind = 3; w.id = id; w.hitX = x; w.hitY = y; w.hitW = 28; w.hitH = 28; return w; };
    w_[MAP].push_back(pad(558, 264, A_M_UP)); w_[MAP].push_back(pad(558, 322, A_M_DOWN));
    w_[MAP].push_back(pad(528, 294, A_M_LEFT)); w_[MAP].push_back(pad(588, 294, A_M_RIGHT));
  }
  w_[OPTIONS] = {wid(0x16300, 0, A_O_BACK), wid(0x16344, 0, A_O_MORE)};
  w_[OPTIONS2] = {wid(0x16300, 0, A_O2_BACK)};
  { Widget w{}; w.kind = 3; w.id = A_MD_BACK; w.hitX = 362; w.hitY = 440; w.hitW = 244; w.hitH = 31; w_[MEDALS].push_back(w); }   // MED_TEST.PCX: BACK
  {                                                          // Create Mission: the fields are on the background image (measured rectangles)
    auto box = [&](Scr sc, int x0, int y0, int x1, int y1, int id) { Widget w{}; w.kind = 3; w.id = id; w.hitX = x0; w.hitY = y0; w.hitW = x1 - x0 + 1; w.hitH = y1 - y0 + 1; w_[sc].push_back(w); };
    auto row = [&](Scr sc, int f, int x0, int x1, int y, bool arrows = true) {     // field: -, + (or the whole field)
      if (arrows) { box(sc, x0 - 28, y - 1, x0 - 2, y + 17, A_CR_FIELD + 2 * f); box(sc, x1 + 2, y - 1, x1 + 28, y + 17, A_CR_FIELD + 2 * f + 1); }
      else box(sc, x0, y, x1, y + 16, A_CR_FIELD + 2 * f + 1);
    };
    for (int k = 0; k < 4; k++) { int y = 152 + 34 * k; row(CREATE_E, k, 167, 259, y); row(CREATE_E, 4 + k, 347, 439, y); row(CREATE_E, 8 + k, 537, 551, y); }
    row(CREATE_E, 12, 347, 551, 287);
    row(CREATE_E, 13, 389, 464, 320, false); row(CREATE_E, 14, 504, 579, 320, false);
    row(CREATE_E, 15, 389, 464, 354, false); row(CREATE_E, 16, 504, 579, 354, false);
    for (int k = 0; k < 2; k++) { int y = 152 + 34 * k; row(CREATE_F, 20 + k, 213, 305, y); row(CREATE_F, 22 + k, 387, 479, y); row(CREATE_F, 24 + k, 557, 571, y); }
    row(CREATE_F, 26, 479, 571, 219); row(CREATE_F, 27, 479, 571, 253);
    for (int r = 0; r < 3; r++)
      for (int c = r == 2 ? 2 : 0; c < 5; c++) {
        int y = 320 + 33 * r + (r == 2 ? 1 : 0);
        box(CREATE_F, 397 + 46 * c, y, 411 + 46 * c, y + 16, A_CR_WEAPON + 5 * r + c);
      }
    for (Scr sc : {CREATE_E, CREATE_F}) {
      box(sc, 37, 435, 222, 468, A_CR_EXIT);
      if (sc == CREATE_E) box(sc, 234, 435, 420, 468, A_CR_FRIENDLY);
      else { box(sc, 234, 435, 420, 468, A_CR_ENEMY); box(sc, 430, 435, 615, 468, A_CR_START); }
    }
  }
  {                                                          // the settings fields are on the background image (positions of the drawn bars and buttons)
    auto box = [&](Scr sc, int x0, int y0, int x1, int y1, int id) { Widget w{}; w.kind = 3; w.id = id; w.hitX = x0; w.hitY = y0; w.hitW = x1 - x0 + 1; w.hitH = y1 - y0 + 1; w_[sc].push_back(w); };
    box(OPTIONS, 220, 352, 240, 372, A_SFX_M); box(OPTIONS, 581, 352, 601, 372, A_SFX_P); box(OPTIONS, 250, 354, 572, 370, A_SFX_BAR);
    box(OPTIONS, 220, 386, 240, 406, A_MUS_M); box(OPTIONS, 581, 386, 601, 406, A_MUS_P); box(OPTIONS, 250, 388, 572, 404, A_MUS_BAR);
    box(OPTIONS, 301, 184, 321, 204, A_TEX_M); box(OPTIONS, 581, 184, 601, 204, A_TEX_P); box(OPTIONS, 330, 186, 572, 202, A_TEX_BAR);
    box(OPTIONS, 409, 152, 483, 168, A_SKY_H); box(OPTIONS, 524, 152, 598, 168, A_SKY_L);
    box(OPTIONS2, 302, 152, 376, 168, A_FS);
    box(OPTIONS2, 410, 186, 484, 202, A_HD_ON); box(OPTIONS2, 525, 186, 599, 202, A_HD_OFF);
    box(OPTIONS2, 410, 253, 484, 269, A_GE_ON); box(OPTIONS2, 525, 253, 599, 269, A_GE_OFF);
    box(OPTIONS2, 396, 117, 416, 137, A_JOY_M); box(OPTIONS2, 583, 117, 603, 137, A_JOY_P); box(OPTIONS2, 425, 119, 573, 135, A_JOY_BAR);
  }
  w_[STATS] = {wid(0x157c4, 0, A_S_MEDALS), wid(0x15780, 0, A_S_BACK)};
  w_[DEBRIEF] = {wid(0x157c4, 0, A_S_MEDALS), wid(0x15780, 0, A_D_CONT)};
  loadProfile();
  return true;
}

std::vector<Menu::Widget>& Menu::widgets() { return w_[scr]; }

std::string Menu::testCampaignEnd(int camp, int score) {
  slots_[0].name = "TEST"; slots_[0].camp = camp; slots_[0].done = false; slots_[0].completed = camp; slots_[0].car.score = score;
  curSlot_ = 0; curCamp_ = camp; cur_ = &camps_[camp].back(); last_ = MissionResult{}; last_.success = true;
  videos.clear();
  activate(A_D_CONT);
  std::string r;
  for (auto& v : videos) r += v + " ";
  return r + "| camp=" + std::to_string(slots_[0].camp) + " done=" + std::to_string(slots_[0].done) + " completed=" + std::to_string(slots_[0].completed) + " scr=" + std::to_string(int(scr));
}

std::vector<Menu::Medal> Menu::medalList() const {
  // conditions (based on the player's cumulative statistics; the view from STATS via the Medals button)
  // names: table 0x15f40; caption: "FOR COMPLETION OF CAMPAIGN %d" (0x1614b)
  static const char* N[5] = {"AIR MEDAL", "FLYING CROSS", "SILVER STAR", "AIR FORCE CROSS", "PRESIDENT UNIT CITATION"};
  static const char* T[5] = {"FOR COMPLETION OF CAMPAIGN 1", "FOR COMPLETION OF CAMPAIGN 2", "FOR COMPLETION OF CAMPAIGN 3",
                             "FOR COMPLETION OF CAMPAIGN 4", "FOR COMPLETION OF CAMPAIGN 5"};
  std::vector<Medal> v;
  // the number of medals is the pilot's campaign ([0x18888]) + 1 if all done ([0x1888b]) (0x16289)
  int n = 0;
  if (curSlot_ >= 0 && slots_[curSlot_].active()) n = slots_[curSlot_].completed;
  for (int i = 0; i < 5; i++) v.push_back({N[i], T[i], i < n});
  return v;
}

void Menu::go(Scr s) {
  scr = s; hover_ = -1; focus_ = -1; click_ = false;
  if (s == NEWCAMP) { campSel_ = 0; nameEdit_ = slots_[0].name; }
  if (s == MAP) prepareMap();
}

void Menu::mouse(int mx, int my, bool down) {
  if (down && !down_) click_ = true;
  down_ = down; mx_ = mx; my_ = my;
  hover_ = -1;
  auto& ws = widgets();
  for (int i = 0; i < int(ws.size()); i++) {
    const Widget& w = ws[i];
    if (mx >= w.hitX && mx < w.hitX + w.hitW && my >= w.hitY && my < w.hitY + w.hitH) { hover_ = i; break; }
  }
  if (hover_ >= 0) focus_ = hover_;
}

void Menu::chr(int c) {                                       // 0x389d6: the callsign field, at most 15 uppercase letters / digits / spaces
  if (scr != NEWCAMP || campSel_ < 0 || campSel_ >= 7) return;
  if (c >= 0x61 && c <= 0x7A) c -= 0x20;
  bool ok = (c >= 0x41 && c <= 0x5A) || (c >= 0x30 && c <= 0x39) || c == 0x20 || c == 0x2D;
  if (ok && nameEdit_.size() < 15) nameEdit_ += char(c);
}

Menu::Action Menu::key(int vk) {
  if (scr == NEWCAMP && vk == VK_BACK) { if (!nameEdit_.empty()) nameEdit_.pop_back(); return NONE; }
  auto& ws = widgets();
  if (ws.empty()) return NONE;
  auto step = [&](int d) {
    int n = int(ws.size()), i = focus_ < 0 ? (d > 0 ? -1 : 0) : focus_;
    for (int k = 0; k < n; k++) {
      i = (i + d + n) % n;
      if (!ws[i].disabled && ws[i].kind != 3) { focus_ = i; return; }
    }
  };
  switch (vk) {
    case VK_DOWN: case VK_TAB: step(1); break;
    case VK_UP: step(-1); break;
    case VK_RETURN: case VK_SPACE:
      if (focus_ >= 0 && !ws[focus_].disabled) activate(ws[focus_].id);
      break;
    case VK_ESCAPE: {
      static const int BACK[13] = {A_EXIT, A_Q_BACK, A_N_BACK, A_C_BACK, A_B_BACK, A_M_BACK, A_O_BACK, A_S_BACK, A_D_CONT, A_O2_BACK, A_CR_EXIT, A_CR_EXIT, A_MD_BACK};
      activate(BACK[scr]);
      break;
    }
    case VK_LEFT: if (scr == MAP) activate(A_M_LEFT); break;
    case VK_RIGHT: if (scr == MAP) activate(A_M_RIGHT); break;
  }
  Action a = pending_; pending_ = NONE;
  return a;
}

Menu::Action Menu::frame() {
  if (click_) {
    click_ = false;
    auto& ws = widgets();
    if (hover_ >= 0 && hover_ < int(ws.size()) && !ws[hover_].disabled) activate(ws[hover_].id);
  }
  if (scr == MAP && down_ && hover_ >= 0 && widgets()[hover_].kind == 3) activate(widgets()[hover_].id);   // continuous scrolling
  if ((scr == OPTIONS || scr == OPTIONS2) && down_ && hover_ >= 0) {                                        // dragging the bars
    int id = widgets()[hover_].id;
    if (id == A_SFX_BAR || id == A_MUS_BAR || id == A_TEX_BAR || id == A_JOY_BAR) setBar(id);
  }
  Action a = pending_; pending_ = NONE;
  return a;
}

// bars: the -/+ buttons step by 10 %, when clicking (or dragging) the bar the value comes from the cursor position
void Menu::setBar(int id) {
  auto bar = [&](int& v, int x0, int x1, int m, int p) {
    if (id == m) v = std::max(0, v - 10);
    else if (id == p) v = std::min(100, v + 10);
    else v = std::clamp((mx_ - x0) * 100 / std::max(1, x1 - x0), 0, 100);
  };
  if (id >= A_SFX_M && id <= A_SFX_BAR) bar(settings.sfx, 250, 572, A_SFX_M, A_SFX_P);
  else if (id >= A_MUS_M && id <= A_MUS_BAR) bar(settings.music, 250, 572, A_MUS_M, A_MUS_P);
  else if (id >= A_TEX_M && id <= A_TEX_BAR) bar(settings.texDist, 330, 572, A_TEX_M, A_TEX_P);
  else if (id >= A_JOY_M && id <= A_JOY_BAR) settings.joy = id == A_JOY_M ? 0 : id == A_JOY_P ? 1 : (mx_ > 499 ? 1 : 0);
}

// Create Mission: stepping one field (dir -1 / +1; the choice lists wrap around); the field numbering is shared with the drawing code
void Menu::createStep(int f, int dir) {
  auto cyc = [&](int& v, int n) { v = ((v + dir) % n + n) % n; };
  auto num = [&](int& v, int lo, int hi) { v = std::clamp(v + dir, lo, hi); };
  auto& s = spec;
  if (f < 4) cyc(s.enemy[f].type, 5);
  else if (f < 8) cyc(s.enemy[f - 4].skill, 4);
  else if (f < 12) num(s.enemy[f - 8].num, 1, 8);
  else if (f == 12) cyc(s.ground, 3);
  else if (f == 13) cyc(s.samSkill, 4);
  else if (f == 14) num(s.samNum, 0, 8);
  else if (f == 15) cyc(s.aaaSkill, 4);
  else if (f == 16) num(s.aaaNum, 0, 8);
  else if (f < 22) cyc(s.friendly[f - 20].type, 4);
  else if (f < 24) cyc(s.friendly[f - 22].skill, 4);
  else if (f < 26) num(s.friendly[f - 24].num, 1, 4);
  else if (f == 26) cyc(s.region, 4);
  else if (f == 27) { int t = ((s.tod - 1 + dir) % 5 + 5) % 5; s.tod = t + 1; }
}

void Menu::activate(int id) {
  clicks++;
  if (id >= A_CR_FIELD && id < A_CR_FIELD + 100) {
    int f = (id - A_CR_FIELD) / 2, up = (id - A_CR_FIELD) & 1;
    createStep(f, up ? 1 : -1);                                    // the SAM / AAA fields (13..16) are single fields: they step forward on click
    return;
  }
  if (id >= A_CR_WEAPON && id < A_CR_WEAPON + 15) {                // weapon load: clicking the n-th box sets the count to n (clicking the current one: one fewer)
    int r = (id - A_CR_WEAPON) / 5, c = (id - A_CR_WEAPON) % 5;
    int& v = r == 0 ? spec.aim9 : r == 1 ? spec.amraam : spec.jdam;
    int n = r == 2 ? c - 1 : c + 1;                                // JDAM: only the 3 right-hand boxes (1..3)
    v = v == n ? n - 1 : n;
    return;
  }
  if (id >= A_ROW_C && id < A_ROW_C + 100) {               // resumable campaign
    int k = id - A_ROW_C;
    if (slots_[k].done || slots_[k].camp < 0 || slots_[k].camp >= int(camps_.size())) return;
    curSlot_ = k; curCamp_ = slots_[k].camp;
    int mi = std::min<int>(slots_[k].mission, int(camps_[curCamp_].size()) - 1);
    cur_ = &camps_[curCamp_][mi];
    back_ = CONTCAMP; prepareBrief(); go(BRIEF);
    return;
  }
  if (id >= A_ROW_N && id < A_ROW_C) { campSel_ = id - A_ROW_N; nameEdit_ = campSel_ < 7 ? slots_[campSel_].name : ""; return; }   // 0x1891a
  if (id >= A_ROW_Q && id < A_ROW_N) { quickSel_ = quickPage_ * 5 + id - A_ROW_Q; quickSel_ = std::min(quickSel_, int(quick_.size()) - 1); return; }
  switch (id) {
    case A_QUICK: go(QUICK); break;
    case A_CREDITS: videos = {"HCREDITS.KDV"}; break;               // 0x12628: the credits video, then the main menu
    case A_NEW: go(NEWCAMP); break;
    case A_CONT: go(CONTCAMP); break;
    case A_OPTIONS: go(OPTIONS); break;
    case A_EXIT: pending_ = QUIT; break;
    case A_Q_ACCEPT:
      if (quick_.empty()) break;
      cur_ = &quick_[quickSel_]; curCamp_ = -1; back_ = QUICK;     // the statistics belong to the current pilot ([0x188b2])
      prepareBrief(); go(BRIEF);
      break;
    case A_Q_CREATE: go(CREATE_E); break;
    case A_CR_EXIT: go(QUICK); break;
    case A_CR_FRIENDLY: go(CREATE_F); break;
    case A_CR_ENEMY: go(CREATE_E); break;
    case A_CR_START:
      if (spec.valid()) { extraUnits = false; spec.region = std::clamp(spec.region, 0, 3); mission = "QMC0" + std::to_string(spec.region + 1); created = true; cur_ = nullptr; curCamp_ = -1; pending_ = START; }
      break;
    case A_Q_BACK: case A_N_BACK: case A_C_BACK: case A_O_BACK: saveProfile(); go(MAIN); break;
    case A_O_MORE: go(OPTIONS2); break;
    case A_O2_BACK: saveProfile(); go(OPTIONS); break;
    case A_SFX_M: case A_SFX_P: case A_SFX_BAR: case A_MUS_M: case A_MUS_P: case A_MUS_BAR: case A_TEX_M: case A_TEX_P: case A_TEX_BAR:
    case A_JOY_M: case A_JOY_P: case A_JOY_BAR: setBar(id); break;
    case A_FS: settings.fullscreen = !settings.fullscreen; break;
    case A_HD_ON: settings.hdTex = true; break;
    case A_HD_OFF: settings.hdTex = false; break;
    case A_GE_ON: settings.gEffects = true; break;
    case A_GE_OFF: settings.gEffects = false; break;
    case A_SKY_H: settings.skyHigh = true; break;
    case A_SKY_L: settings.skyHigh = false; break;
    case A_Q_PREV: quickPage_ = std::max(0, quickPage_ - 1); break;
    case A_Q_NEXT: quickPage_ = std::min(int(quick_.size() - 1) / 5, quickPage_ + 1); break;
    case A_N_ACCEPT: {
      // 0x18aa8 / 0x188d0: a new pilot in the selected slot with the entered callsign (not with an empty name), starting at campaign 1 mission 1
      if (campSel_ < 0 || campSel_ >= 7 || nameEdit_.empty() || camps_.empty()) break;
      int k = campSel_;
      slots_[k] = Slot{};
      slots_[k].camp = 0; slots_[k].mission = 0; slots_[k].name = nameEdit_;
      saveProfile();
      curSlot_ = k; curCamp_ = 0; cur_ = &camps_[0][0]; back_ = NEWCAMP;
      prepareBrief(); go(BRIEF);
      break;
    }
    case A_B_START: if (cur_) { mission = cur_->name; created = false; extraUnits = curCamp_ >= 0 && bonusNext_; bonusNext_ = false; pending_ = START; } break;
    case A_B_MAP: go(MAP); break;
    case A_B_BACK: go(back_); break;
    case A_B_STATS: go(STATS); break;
    case A_B_PREV: briefPage_ = std::max(0, briefPage_ - 1); break;
    case A_B_NEXT: briefPage_ = std::min(std::max(0, int(briefLines_.size() - 1) / 18), briefPage_ + 1); break;
    case A_M_BACK: go(BRIEF); break;
    case A_M_UP: mapY_ -= 8; break;
    case A_M_DOWN: mapY_ += 8; break;
    case A_M_LEFT: mapX_ -= 8; break;
    case A_M_RIGHT: mapX_ += 8; break;
    case A_S_BACK: go(cur_ ? BRIEF : MAIN); break;
    case A_S_MEDALS: medalBack_ = scr; go(MEDALS); break;            // 0x15a03 -> 0x15e9c
    case A_MD_BACK: go(medalBack_); break;
    case A_D_CONT:
      if (cur_ && curCamp_ >= 0 && last_.success) {          // campaign: next mission
        int next = cur_->idx + 1;
        if (next < int(camps_[curCamp_].size())) {
          cur_ = &camps_[curCamp_][next];
          if (curSlot_ >= 0) { slots_[curSlot_].mission = next; saveProfile(); }
          prepareBrief(); go(BRIEF);
        } else {
          // the campaign's last mission (0xfe3d): the pilot advances to the next campaign (after the last one: all done),
          // a medal is awarded, the medal screen opens
          if (curSlot_ >= 0 && slots_[curSlot_].active()) {
            Slot& p = slots_[curSlot_];
            p.mission = 0;
            p.completed = std::max(p.completed, curCamp_ + 1);
            // 0xfe67: campaign 5 opens only above 175000 (0x2ab98) points; otherwise it ends after campaign 4
            int avail = std::min(int(camps_.size()), p.car.score >= 175000 ? 5 : 4);
            if (curCamp_ + 1 < avail) p.camp = curCamp_ + 1;
            else { p.done = true; p.camp = -1; }
            // 0x53c4c / 0x53bfa: the medal video (3..7), after campaign 4 / 5 the ending (HEND) and the secret one (HSECRET), respectively
            static const char* MV[5] = {"HAIR", "HFLY", "HSTAR", "HAFC", "HPRES"};
            if (curCamp_ < 5) videos.push_back(std::string(MV[curCamp_]) + ".KDV");
            if (curCamp_ == 3) videos.push_back("HEND.KDV");
            else if (curCamp_ == 4) videos.push_back("HSECRET.KDV");
          }
          saveProfile();
          cur_ = nullptr;
          medalBack_ = MAIN; go(MEDALS);
        }
      } else if (cur_) { prepareBrief(); go(BRIEF); }
      else go(MAIN);
      break;
  }
  if (scr == MAP) {                                          // the map must not run off the screen
    mapX_ = std::clamp(mapX_, 0, 1024 - 500);
    mapY_ = std::clamp(mapY_, 0, 1024 - 480);
  }
}

std::string Menu::regionText(const Msn& m) const {
  std::string s = m.region < 4 ? regions_[m.region] : "";
  if (m.tod >= 0 && m.tod < 5) s += tods_[m.tod];
  return s;
}

void Menu::prepareBrief() {
  briefPage_ = 0;
  briefLines_.clear();
  if (!cur_) return;
  Blob t = res_->get(cur_->name + ".TXT");
  std::string txt = t.ok() ? std::string(reinterpret_cast<const char*>(t.p), t.n) : cur_->desc;
  briefLines_ = Screen::wrap(fat_, txt, 247);                     // 0x14ccc: x=38, y=135, 247 wide, 10 line spacing
}

void Menu::prepareMap() {
  if (!cur_) return;
  std::string err;
  if (mapW_.init(*res_, *exe_, cur_->name, err)) {
    for (auto& u : mapW_.units)
      if (u.side == 2) { mapX_ = int(u.pos.x / 64) - 250; mapY_ = int((65536 - u.pos.z) / 64) - 240; break; }
  }
  mapX_ = std::clamp(mapX_, 0, 1024 - 500);
  mapY_ = std::clamp(mapY_, 0, 1024 - 480);
}

// ---------------------------------------------------------------- drawing
void Menu::draw(Screen& s) {
  s.pal = &bg_[scr];
  s.pic(bg_[scr]);
  char b[160];
  switch (scr) {
    case QUICK: {                                             // the row text after the sprites (0x11d2a)
      if (quickSel_ < int(quick_.size())) {                   // 0x1183e: x=334, y=112, 246 wide
        auto lines = s.wrap(fat_, quick_[quickSel_].desc, 246);
        for (int k = 0; k < int(lines.size()) && k < 16; k++) s.text(fat_, 334, 112 + 10 * k, lines[k]);
      }
      int pages = std::max(1, (int(quick_.size()) + 4) / 5);
      std::snprintf(b, sizeof b, "~2PAGE %d OF %d", quickPage_ + 1, pages);
      s.textC(fat_, 160, 397, b);
      break;
    }
    case CONTCAMP:
      for (int k = 0; k < 7; k++) {
        const Widget& w = w_[CONTCAMP][1 + k];
        if (!slots_[k].active()) s.text(fat_, w.x + 52, w.y + 11, cstr(*exe_, 0x1835c), 1);   // „EMPTY”
        else s.text(fat_, w.x + 52, w.y + 11, "~2" + slots_[k].name);                          // 0x1891a: the pilot's callsign
      }
      break;
    case BRIEF:
      if (cur_) {
        if (cur_->camp >= 0) std::snprintf(b, sizeof b, "~3CAMPAIGN %d MISSION %d", cur_->camp + 1, cur_->idx + 1);
        else std::snprintf(b, sizeof b, "~3QUICK MISSION %d", cur_->idx + 1);
        s.text(fat_, 38, 105, b);                              // 0x1527b
        s.text(fat_, 38, 117, "~3" + cur_->title, 3, 247);
        int first = briefPage_ * 18;
        for (int k = 0; k < 18 && first + k < int(briefLines_.size()); k++) s.text(fat_, 38, 135 + 10 * k, briefLines_[first + k]);
        int pages = std::max(1, (int(briefLines_.size()) + 17) / 18);
        std::snprintf(b, sizeof b, "~2PAGE %d OF %d", briefPage_ + 1, pages);
        s.textC(fat_, 160, 365, b);
        // weapons (0x14fee): centered around x=486
        int y = 232;
        s.textC(fat_, 486, y, cstr(*exe_, 0x14d50)); y += 10;
        const uint32_t WS[3] = {0x14d6b, 0x14d8b, 0x14da9};
        for (int k = 0; k < 3; k++)
          if (cur_->ammo[k]) { s.textC(fat_, 486, y, fmtHash(cstr(*exe_, WS[k]), cur_->ammo[k])); y += 10; }
        std::snprintf(b, sizeof b, "~2FUEL %d LBS", cur_->fuel);
        s.textC(fat_, 486, y + 6, b);
        s.textC(fat_, 486, y + 18, regionText(*cur_));
      }
      break;
    case MAP:
      if (cur_ && cur_->region < 4 && mapBg_[cur_->region].w) {
        s.sprite(mapBg_[cur_->region], mapX_, mapY_, 500, 480, 0, 0, -1);
        auto toMap = [&](sim::D3 p, int& x, int& y) { x = int(p.x / 64) - mapX_; y = int((65536 - p.z) / 64) - mapY_; };
        // the player's route
        for (auto& u : mapW_.units)
          if (u.side == 2 && u.route >= 0 && u.route < int(mapW_.routes().size())) {
            auto& R = mapW_.routes()[u.route];
            int px, py; toMap(u.pos, px, py);
            for (size_t i = 0; i < R.size(); i++) {
              int x, y; toMap(R[i].pos, x, y);
              s.line(px, py, x, y, rgb(90, 170, 255));
              s.line(x - 3, y, x, y - 3, rgb(90, 170, 255)); s.line(x, y - 3, x + 3, y, rgb(90, 170, 255));
              s.line(x + 3, y, x, y + 3, rgb(90, 170, 255)); s.line(x, y + 3, x - 3, y, rgb(90, 170, 255));
              px = x; py = y;
            }
          }
        // units: ICONS_G (friendly), ICONS_R (enemy), ICONS_B (own) - 20x20 icons
        for (auto& u : mapW_.units) {
          int x, y; toMap(u.pos, x, y);
          if (x < -10 || x > 510 || y < -10 || y > 490) continue;
          int set = u.side == 4 ? 1 : u.side == 2 ? 2 : 0, ic;
          std::string m = upperS(u.model);
          bool big = m.rfind("A50", 0) == 0 || m.rfind("AN2", 0) == 0 || m.rfind("AWACS", 0) == 0 || m.rfind("B1B", 0) == 0 ||
                     m.rfind("C5", 0) == 0 || m.rfind("TU_", 0) == 0 || m.rfind("AF1", 0) == 0;
          if (u.cls == sim::C_RUNWAY) ic = 0;
          else if (u.cls == sim::C_AIR) ic = big ? 2 : 1;
          else if (m.rfind("NANUCHKA", 0) == 0) ic = 5;
          else if (u.cls == sim::C_GROUND) ic = 4;
          else ic = 3;
          if (u.side == 2 && icons_[2].h >= 185) { s.sprite(icons_[2], 0, 165, 20, 20, x - 10, y - 10, -1); continue; }
          s.sprite(icons_[set], 0, ic * 20, 20, 20, x - 10, y - 10, -1);
        }
        s.fill(500, 0, 1, 480, rgb(0, 0, 0));
        s.sprite(bg_[MAP], 500, 0, 140, 480, 500, 0, -1);      // redraw the right-hand strip (so the map does not overlap it)
      }
      break;
    case OPTIONS: case OPTIONS2: {
      auto bar = [&](int x0, int y0, int x1, int y1, int v) {  // filled bar in the field
        int w = (x1 - x0 - 3) * v / 100;
        if (w > 0) s.fill(x0 + 2, y0 + 2, w, y1 - y0 - 3, rgb(40, 190, 40));
      };
      auto choice = [&](int x0, int y0, int x1, const char* t, bool sel, bool live) {   // choice field with a label
        if (sel) { s.fill(x0, y0, x1 - x0 + 1, 17, rgb(0, 70, 0)); }
        s.textC(fat_, (x0 + x1) / 2, y0 + 4, t, live ? (sel ? 3 : 2) : 6);
      };
      if (scr == OPTIONS) {
        bar(330, 186, 572, 202, settings.texDist);
        bar(250, 354, 572, 370, settings.sfx);
        bar(250, 388, 572, 404, settings.music);
        choice(409, 152, 483, "HIGH", settings.skyHigh, true); choice(524, 152, 598, "LOW", !settings.skyHigh, true);
        // the original's resolution / memory settings are not meaningful here (the window is resizable): shown dimmed, 640X480 / HIGH
        choice(301, 119, 375, "320X240", false, false); choice(409, 119, 483, "400X300", false, false); choice(524, 119, 598, "640X480", true, false);
        choice(409, 253, 483, "HIGH", true, false); choice(524, 253, 598, "LOW", false, false);
        choice(409, 287, 483, "HIGH", true, false); choice(524, 287, 598, "LOW", false, false);
      } else {
        s.textC(fat_, 499, 123, settings.joy ? "JOYSTICK / GAMEPAD" : "NO JOYSTICK", 3);
        choice(302, 152, 376, settings.fullscreen ? "FULL" : "WINDOW", settings.fullscreen, true); choice(410, 152, 484, "N/A", false, false); choice(525, 152, 599, "N/A", false, false);
        choice(410, 186, 484, "HD", settings.hdTex, true); choice(525, 186, 599, "OLD", !settings.hdTex, true);
        choice(410, 253, 484, "ON", settings.gEffects, true); choice(525, 253, 599, "OFF", !settings.gEffects, true);
      }
      break;
    }
    case CREATE_E: case CREATE_F: {                           // enemy.pcx / friendly.pcx: the field texts (tables 0x13062 / 0x14147)
      static const char* EN[5] = {"NONE", "EF2000", "MIG-27", "MIG-29", "SU-27"};
      static const char* FR[4] = {"NONE", "F-14", "F-15", "F-16"};
      static const char* SK[4] = {"NOVICE", "INTERMEDIATE", "EXPERT", "ACE"};
      static const char* GR[3] = {"NONE", "CHEMICAL PLANT", "NUCLEAR POWER PLANT"};
      static const char* RG[4] = {"ISLAND", "SNOW", "DESERT", "JUNGLE"};
      static const char* TD[5] = {"DAY", "NIGHT", "DAWN", "STORM", "SUNSET"};
      auto fld = [&](int x0, int x1, int y, const std::string& t, bool live = true) { s.textC(fat_, (x0 + x1) / 2, y + 4, std::string(live ? "~2" : "~6") + t); };
      if (scr == CREATE_E) {
        for (int k = 0; k < 4; k++) {
          int y = 152 + 34 * k;
          bool on = spec.enemy[k].type > 0;
          fld(167, 259, y, EN[spec.enemy[k].type]);
          fld(347, 439, y, on ? SK[spec.enemy[k].skill] : "", on);
          fld(537, 551, y, on ? std::to_string(spec.enemy[k].num) : "", on);
        }
        fld(347, 551, 287, GR[spec.ground]);
        fld(389, 464, 320, spec.samNum ? (spec.samSkill == 1 ? "INTERMED." : SK[spec.samSkill]) : ""); fld(504, 579, 320, std::to_string(spec.samNum));
        fld(389, 464, 354, spec.aaaNum ? (spec.aaaSkill == 1 ? "INTERMED." : SK[spec.aaaSkill]) : ""); fld(504, 579, 354, std::to_string(spec.aaaNum));
        if (!spec.valid()) s.textC(fat_, 320, 396, "~4NOTHING TO DESTROY - SET AN ENEMY");
      } else {
        for (int k = 0; k < 2; k++) {
          int y = 152 + 34 * k;
          bool on = spec.friendly[k].type > 0;
          fld(213, 305, y, FR[spec.friendly[k].type]);
          fld(387, 479, y, on ? SK[spec.friendly[k].skill] : "", on);
          fld(557, 571, y, on ? std::to_string(spec.friendly[k].num) : "", on);
        }
        fld(479, 571, 219, RG[spec.region]);
        fld(479, 571, 253, TD[spec.tod - 1]);
        const int cnt[3] = {spec.aim9, spec.amraam, spec.jdam};
        for (int r = 0; r < 3; r++)
          for (int c = r == 2 ? 2 : 0; c < 5; c++) {
            int n = r == 2 ? c - 1 : c + 1;
            if (cnt[r] >= n) s.fill(397 + 46 * c + 2, 320 + 33 * r + (r == 2 ? 1 : 0) + 2, 11, 12, rgb(40, 190, 40));
          }
        if (!spec.valid()) s.textC(fat_, 320, 412, "~4SET AN ENEMY FIRST (ENEMY SCREEN)");
      }
      break;
    }
    case MEDALS: {                                             // 0x160a0: MED_TEST.PCX, the medals come from the MSTRIP.PCX strip (0x16213)
      static const int SY[5] = {0, 191, 381, 571, 761}, DX[5] = {118, 224, 331, 437, 277}, DY[5] = {103, 103, 103, 103, 300}, H[5] = {184, 184, 184, 184, 30};
      auto list = medalList();
      int over = -1;
      for (int k = 0; k < 5; k++) {
        if (!list[k].got) continue;
        s.sprite(mstrip_, 0, SY[k], 86, H[k], DX[k], DY[k]);
        if (mx_ >= DX[k] && mx_ <= DX[k] + 86 && my_ >= DY[k] && my_ <= DY[k] + H[k]) over = k;   // 0x162a8
      }
      if (over >= 0) {                                         // the name (y = 340) and the caption (y = 358), centered
        s.textC(fat_, 320, 340, std::string("~3") + list[over].name);
        s.textC(fat_, 320, 358, std::string("~3") + list[over].need);
      }
      break;
    }
    case STATS: case DEBRIEF: {                                 // 0x15a4d: pilot data; the debriefing uses the same background
      int y = 84, x = 66;
      auto L = [&](const std::string& t) { s.text(fat_, x, y, t); y += 13; };
      if (scr == DEBRIEF) {
        L(last_.success ? "~3MISSION COMPLETE" : last_.aborted ? "~4MISSION ABORTED" : "~4MISSION FAILED");
        y += 6;
        std::snprintf(b, sizeof b, "~2%3d ENEMY AIR KILLS,  %d BY YOU", last_.airKills, last_.playerAir); L(b);
        std::snprintf(b, sizeof b, "~2%3d ENEMY GROUND KILLS,  %d BY YOU", last_.groundKills, last_.playerGround); L(b);
        std::snprintf(b, sizeof b, "~2%3d MISSILES FIRED", last_.missiles); L(b);
        y += 6;
        L("~1MISSION GOALS");
        static const char* G[3] = {"PRIMARY", "SECONDARY", "BONUS"};
        for (int k = 0; k < 3; k++)
          if (last_.goalTotal[k]) {
            std::snprintf(b, sizeof b, "~2%s: %d OF %d", G[k], std::min(last_.goalDone[k], last_.goalTotal[k]), last_.goalTotal[k]);
            L(b);
          }
        if (last_.crashed) L("~4AIRCRAFT LOST");
        if (last_.ejected) L("~4EJECTED");
        std::snprintf(b, sizeof b, "~1SCORE: ~2%d", last_.score); L(b);
        int t = int(last_.time);
        std::snprintf(b, sizeof b, "~1FLIGHT TIME: ~2%d:%02d", t / 60, t % 60); L(b);
      } else {
        // 0x15a34: the original rows and positions (x = 92, the score at x = 400); rank: 0x15a16 (thresholds 0x1596c)
        static const int RT[5] = {15000, 30000, 55000, 100000, 160000};
        static const char* RN[6] = {"1ST LIEUTENANT", "CAPTAIN", "MAJOR", "LIEUTENANT COLONEL", "COLONEL", "BRIGADIER GENERAL"};
        int rk = 0;
        Career& car_ = car();
        while (rk < 5 && car_.score >= RT[rk]) rk++;
        auto P = [&](int px, int py, const std::string& t) { s.text(fat_, px, py, t); };
        std::snprintf(b, sizeof b, "~1CALLSIGN: ~2%s", curSlot_ >= 0 && slots_[curSlot_].active() ? slots_[curSlot_].name.c_str() : ""); P(92, 100, b);
        std::snprintf(b, sizeof b, "~1SCORE: ~2%d", car_.score); P(400, 100, b);
        std::snprintf(b, sizeof b, "~1RANK: ~2%s", RN[rk]); P(92, 120, b);
        if (curSlot_ >= 0 && slots_[curSlot_].camp >= 0) {        // 0x15b07: only if the pilot is in a campaign
          std::snprintf(b, sizeof b, "~2CAMPAIGN %d  MISSION %d", slots_[curSlot_].camp + 1, slots_[curSlot_].mission + 1); P(92, 140, b);
        }
        std::snprintf(b, sizeof b, "~1KILLS: ~2%d", car_.air); P(92, 180, b);
        std::snprintf(b, sizeof b, "~1CRASHES: ~2%d", car_.crashes); P(92, 200, b);
        std::snprintf(b, sizeof b, "~1EJECTIONS: ~2%d", car_.ejections); P(92, 220, b);
        std::snprintf(b, sizeof b, "~1LANDINGS: ~2%d", car_.landings); P(92, 240, b);
        std::snprintf(b, sizeof b, "~1SERVICE HOURS: ~2%d:%02d", car_.minutes / 60, car_.minutes % 60); P(92, 260, b);
        std::snprintf(b, sizeof b, "~1BULLETS - FIRED: ~2%d  ~1HIT: ~2%d  -  %d%c", car_.bFired, car_.bHit, car_.bFired ? car_.bHit * 100 / car_.bFired : 0, '%'); P(92, 296, b);
        std::snprintf(b, sizeof b, "~1MISSILES - FIRED: ~2%d  ~1HIT: ~2%d  -  %d%c", car_.mFired, car_.mHit, car_.mFired ? car_.mHit * 100 / car_.mFired : 0, '%'); P(92, 320, b);
        std::snprintf(b, sizeof b, "~1JDAMS FIRED: ~2%d  ~1TARGETS DESTROYED: ~2%d", car_.jFired, car_.jKills); P(92, 344, b);
        break;
      }
      break;
    }
    default: break;
  }
  // widgets: 0 normal, 1 under mouse / selected, 2 pressed
  auto& ws = widgets();
  for (int i = 0; i < int(ws.size()); i++) {
    const Widget& w = ws[i];
    if (w.kind == 3) continue;
    int st = 0;
    if (w.kind == 1) {
      bool sel = (scr == QUICK && quickPage_ * 5 + (w.id - A_ROW_Q) == quickSel_) || (scr == NEWCAMP && w.id - A_ROW_N == campSel_);
      if (w.disabled || scr != QUICK) continue;            // the campaign rows' sprites are not in BUTTONS.PCX
      st = sel ? 1 : 0;
      s.sprite(buttons_, w.sx[st], w.sy[st], w.w, w.h, w.x, w.y);
      continue;
    }
    if (!w.disabled) st = (i == hover_ && down_) ? 2 : (i == hover_ || i == focus_) ? 1 : 0;
    s.sprite(buttons_, w.sx[st], w.sy[st], w.w, w.h, w.x, w.y);
    if (w.disabled) s.shade(w.hitX, w.hitY, w.hitW, w.hitH, 0.45f);
  }
  // list row text over the sprites: quick mission 0x11d2a (x=88, y=116+48k, 3 rows)
  if (scr == QUICK || scr == NEWCAMP) {
    if (scr == QUICK) {
      for (int k = 0; k < 5; k++) {
        int i = quickPage_ * 5 + k;
        if (i >= int(quick_.size())) continue;
        const Msn& m = quick_[i];
        int c = i == quickSel_ ? 2 : 1, y = 116 + 48 * k;
        std::string l1 = fmtHash(cstr(*exe_, 0x118a0), i + 1);
        if (l1.size() > 1) l1[1] = char('0' + c);               // 0x11dd5: the color code depends on the selection
        s.text(fat_, 88, y, l1, c);
        s.text(fat_, 88, y + 10, m.title, c, 180);
        std::string r = regionText(m);
        s.text(fat_, 88, y + 20, r.substr(r.size() > 1 && r[0] == '~' ? 2 : 0), c, 180);
      }
    } else {
      for (int k = 0; k < 7; k++) {                             // 0x187c0: callsign fields; the selected row is editable
        const Widget& w = w_[NEWCAMP][2 + k];
        std::string t = k == campSel_ ? nameEdit_ : slots_[k].name;
        if (k == campSel_ && (GetTickCount() / 400) % 2 == 0) t += "_";
        s.text(fat_, w.x + 8, w.y + 3, std::string(k == campSel_ ? "~3" : "~2") + t, 2);
      }
    }
  }
  if (mx_ >= 0 && my_ >= 0) s.sprite(cursor_, 0, 0, cursor_.w, cursor_.h, mx_, my_);   // index 0 (red) is transparent
}

void Menu::drawLoading(Screen& s) {
  s.pal = nullptr;
  s.pic(loading_);
  s.textC(fat_, 320, 440, "~5" + cstr(*exe_, 0x112a5), 5);     // „LOADING SIMULATION”
}

void Menu::missionEnded(const MissionResult& r) {
  last_ = r;
  // 0xfeb9: after every mission landing, ejection, crash (once), service time (in minutes)
  Career& car_ = car();
  if (r.landed) car_.landings++;
  if (r.ejected) car_.ejections++;
  if (r.crashed) car_.crashes++;
  car_.minutes += int(r.time / 60);
  bonusNext_ = false;
  if (cur_ && curCamp_ >= 0) {
    // 0xff1a: only in a campaign: score, kills, weapon counters
    car_.score += r.score; car_.air += r.playerAir; car_.ground += r.playerGround;
    car_.bFired += r.bulletsFired; car_.bHit += r.bulletsHit; car_.mFired += r.missilesFired; car_.mHit += r.missilesHit;
    car_.jFired += r.jdamFired; car_.jKills += r.jdamKills;
    // 0xfd30: the mission score reaches the threshold (0xfcbc) -> extra enemy in the next load ([0x22c4d])
    static const int THR[5][8] = {{3700, 5600, 6000, 4000, 7200, 4500, 5900, 8000}, {4100, 6800, 4800, 7000, 4100, 7200, 6700, 0},
                                  {4800, 6500, 9700, 6300, 5300, 5600, 6400, 9100}, {7600, 3600, 7900, 2900, 7200, 2300, 4600, 5000},
                                  {6700, 5300, 3500, 4800, 6000, 0, 0, 0}};
    int c = std::clamp(curCamp_, 0, 4), m = std::clamp(cur_->idx, 0, 7);
    bonusNext_ = r.score >= THR[c][m];
  }
  stSorties_++;
  if (r.success) stWins_++;
  if (r.crashed) stCrashes_++;
  stKills_ += r.playerAir + r.playerGround;
  stMissiles_ += r.missiles;
  stHours_ += r.time / 3600.0;
  saveProfile();
  go(DEBRIEF);
}

void Menu::loadProfile() {
  FILE* f = std::fopen(savePath_.c_str(), "r");
  if (!f) return;
  char key[32];
  while (std::fscanf(f, "%31s", key) == 1) {
    std::string k = key;
    if (k == "medals") { int m; std::fscanf(f, "%d", &m); }   // old format (discarded)
    if (k == "slot") {                                     // old format: slot without a name
      int i, c, m;
      if (std::fscanf(f, "%d %d %d", &i, &c, &m) == 3 && i >= 0 && i < 7) { slots_[i].camp = c; slots_[i].mission = m; slots_[i].name = "PILOT " + std::to_string(i + 1); }
    }
    else if (k == "pilot") {                               // pilot slot campaign mission done name (_ in place of a space)
      int i, c, m, dn, cp; char nm[64] = {};
      if (std::fscanf(f, "%d %d %d %d %d %63s", &i, &c, &m, &dn, &cp, nm) == 6 && i >= 0 && i < 7) {
        slots_[i].camp = c; slots_[i].mission = m; slots_[i].done = dn != 0; slots_[i].completed = cp; slots_[i].name = nm;
        for (char& ch : slots_[i].name) if (ch == 0x5F) ch = 0x20;
      }
    }
    else if (k == "cur") { std::fscanf(f, "%d", &curSlot_); if (curSlot_ < -1 || curSlot_ >= 7) curSlot_ = -1; }
    else if (k == "opt") { int sh = 1, fs = 0, ge = 1, hd = 0; if (std::fscanf(f, "%d %d %d %d %d %d %d %d", &settings.sfx, &settings.music, &settings.texDist, &sh, &settings.joy, &fs, &ge, &hd) >= 5) { settings.skyHigh = sh != 0; settings.fullscreen = fs != 0; settings.gEffects = ge != 0; settings.hdTex = hd != 0; } }
    else if (k == "career" || k == "pcareer") {          // career: old, shared (for pilot 0); pcareer: per pilot
      int i = 0;
      if (k == "pcareer" && (std::fscanf(f, "%d", &i) != 1 || i < 0 || i >= 7)) break;
      Career& c = slots_[i].car;
      if (std::fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d", &c.score, &c.air, &c.ground, &c.crashes, &c.ejections, &c.landings,
                      &c.minutes, &c.bFired, &c.bHit, &c.mFired, &c.mHit, &c.jFired, &c.jKills) != 13) break;
    }
    else if (k == "stats") {
      if (std::fscanf(f, "%d %d %d %d %d %lf", &stKills_, &stCrashes_, &stSorties_, &stWins_, &stMissiles_, &stHours_) != 6) break;
    }
  }
  std::fclose(f);
}

void Menu::saveProfile() {
  if (!persist) return;
  FILE* f = std::fopen(savePath_.c_str(), "w");
  if (!f) return;
  for (int i = 0; i < 7; i++) {
    const Slot& p = slots_[i];
    if (!p.active()) continue;
    std::string nm = p.name.empty() ? "PILOT" : p.name;
    for (char& ch : nm) if (ch == 0x20) ch = 0x5F;
    std::fprintf(f, "pilot %d %d %d %d %d %s\n", i, p.camp, p.mission, int(p.done), p.completed, nm.c_str());
    const Career& c = p.car;
    std::fprintf(f, "pcareer %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", i, c.score, c.air, c.ground, c.crashes, c.ejections,
                 c.landings, c.minutes, c.bFired, c.bHit, c.mFired, c.mHit, c.jFired, c.jKills);
  }
  std::fprintf(f, "cur %d\n", curSlot_);
  std::fprintf(f, "opt %d %d %d %d %d %d %d %d\n", settings.sfx, settings.music, settings.texDist, int(settings.skyHigh), settings.joy, int(settings.fullscreen), int(settings.gEffects), int(settings.hdTex));
  std::fprintf(f, "stats %d %d %d %d %d %f\n", stKills_, stCrashes_, stSorties_, stWins_, stMissiles_, stHours_);
  std::fclose(f);
}
