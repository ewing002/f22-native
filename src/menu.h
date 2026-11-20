// The original menu system: main menu, quick mission, new / continued campaign, briefing,
// mission map, options, pilot statistics, loading screen and debriefing.
// The positions and sprites of buttons, list rows and pagers come from the original program's widget tables
// (0x44-byte records, e.g. main menu: 0x12190); the text positions come from the drawing routines.
#pragma once
#include <string>
#include <vector>

#include "res.h"
#include "sim.h"
#include "ui.h"

struct MissionResult {
  bool success = false, failed = false, crashed = false, aborted = false;
  int airKills = 0, groundKills = 0, playerAir = 0, playerGround = 0, missiles = 0, bombs = 0;
  int goalTotal[4] = {}, goalDone[4] = {};
  float time = 0;
  int score = 0, bulletsFired = 0, bulletsHit = 0, missilesFired = 0, missilesHit = 0, jdamFired = 0, jdamKills = 0;
  bool ejected = false, landed = false;
};

class Menu {
 public:
  enum Action { NONE, START, QUIT };
  enum Scr { MAIN, QUICK, NEWCAMP, CONTCAMP, BRIEF, MAP, OPTIONS, STATS, DEBRIEF, OPTIONS2, CREATE_E, CREATE_F, MEDALS };

  bool init(const Res& res, const sim::Exe& exe, const std::string& saveDir, std::string& err);
  // input in 640x480 coordinates (mx < 0: the mouse is outside the image)
  void mouse(int mx, int my, bool down);
  Action key(int vk);
  Action frame();                          // process the clicks
  void draw(Screen& s);
  void drawLoading(Screen& s);
  void missionEnded(const MissionResult& r);
  void go(Scr s);
  void createStep(int f, int dir);
  void setPilotForTest(int camp, bool done) { curSlot_ = 0; slots_[0].camp = camp; slots_[0].done = done; if (slots_[0].name.empty()) slots_[0].name = "TEST"; }   // self-test only
  void chr(int c);
  std::string testCampaignEnd(int camp, int score);   // self-test only: success of the campaign's last mission, CONTINUE
  std::string statusLine() const { std::string r; for (int i = 0; i < 7; i++) if (slots_[i].active()) r += std::to_string(i) + ":" + slots_[i].name + "/C" + std::to_string(slots_[i].camp + 1) + "M" + std::to_string(slots_[i].mission + 1) + " "; return r + "cur=" + std::to_string(curSlot_); }                         // WM_CHAR (NEW CAMPAIGN: callsign)
  bool persist = true;
  std::vector<std::string> videos;         // the main program plays these (KDV file names), then clears the list
  bool campaignMission() const { return cur_ && curCamp_ >= 0; }
  bool extraUnits = false;                 // [0x22c4d] for the next load (read by main, cleared after START)
  sim::CreateSpec spec;                    // Create Mission settings
  bool created = false;                    // the selected mission is the Create Mission one (spec is valid)
  int clicks = 0;                          // click counter (for the sound)                     // false: the profile is not written (self-test)

  // settings (OPTIONS / MORE OPTIONS screen; saved in the profile)
  struct Settings { int sfx = 100, music = 80, texDist = 100; bool skyHigh = true; int joy = 1; bool fullscreen = false; bool gEffects = true; bool hdTex = false; } settings;
  std::string mission;                     // the selected mission (e.g. C04M01)
  Scr scr = MAIN;

 private:
  struct Widget {
    int sx[3], sy[3], w = 0, h = 0, x = 0, y = 0;
    int kind = 0;                          // 0 button, 1 list row, 2 pager
    int id = 0;                            // action
    bool disabled = false;
    int hitX = 0, hitY = 0, hitW = 0, hitH = 0;
  };
  struct Msn { std::string name, title, desc; int region = 0, tod = 0, camp = -1, idx = 0, ammo[3] = {}, fuel = 0; };

  Widget wid(uint32_t addr, int kind, int id, bool disabled = false);
  std::vector<Widget>& widgets();
  void activate(int id);
  void prepareBrief();
  void prepareMap();
  void loadProfile();
  void saveProfile();
  std::string regionText(const Msn& m) const;

  const Res* res_ = nullptr;
  const sim::Exe* exe_ = nullptr;
  std::string savePath_;
  Font fat_;
  Pic bg_[13], mstrip_, buttons_, cursor_, loading_, mapBg_[4], icons_[3];
  std::vector<Widget> w_[13];
  Scr medalBack_ = MAIN;                   // where the medal screen's BACK button leads
  void setBar(int id);
  std::vector<Msn> quick_;
  std::vector<std::vector<Msn>> camps_;
  std::string regions_[5], tods_[5];
  int mx_ = -1, my_ = -1;
  bool down_ = false, click_ = false;
  int hover_ = -1, focus_ = -1;
  int quickSel_ = 0, quickPage_ = 0, campSel_ = 0, slotSel_ = -1, briefPage_ = 0;
  Action pending_ = NONE;
  // the selected mission
  const Msn* cur_ = nullptr;
  int curCamp_ = -1, curSlot_ = -1;
  std::vector<std::string> briefLines_;
  sim::World mapW_;
  int mapX_ = 0, mapY_ = 0;
  Scr back_ = MAIN;
  MissionResult last_;
  // career (0x18870): score, air / ground kills, crashes, ejections, landings, service minutes,
  // shells fired / hit, missiles fired / hit, JDAMs released / targets destroyed
  struct Career { int score = 0, air = 0, ground = 0, crashes = 0, ejections = 0, landings = 0, minutes = 0,
                  bFired = 0, bHit = 0, mFired = 0, mHit = 0, jFired = 0, jKills = 0; };
  // pilots (the original's 7 slots, 0x187a0 / 0x2a5aa): callsign, campaign / mission ([0x18888] / [0x18889]),
  // "all done" ([0x1888b]) and their own career; the campaigns follow one another in order (0xfe3d)
  struct Slot { int camp = -1, mission = 0; std::string name; bool done = false; int completed = 0; Career car;
                bool active() const { return camp >= 0 || done; } };
  Slot slots_[7];
  std::string nameEdit_;                   // NEW CAMPAIGN: edited callsign of the selected row (15 characters, 0x187c0 +0xc)
  int stKills_ = 0, stCrashes_ = 0, stSorties_ = 0, stWins_ = 0, stMissiles_ = 0;
  Career& car() { static Career none; if (curSlot_ >= 0 && slots_[curSlot_].active()) return slots_[curSlot_].car; none = Career{}; return none; }
  bool bonusNext_ = false;               // [0x22c4d]: for the next campaign mission
  // medals (0x160a0 screen, 0xfe3d award): the i-th medal is awarded for completing the i-th campaign
  struct Medal { const char* name; const char* need; bool got; };
  std::vector<Medal> medalList() const;
  double stHours_ = 0;
};
