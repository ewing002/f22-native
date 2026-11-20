// Audio: custom mixer on Win32 waveOut (44.1 kHz stereo, 16 bit) and a MIDI player on the Windows
// built-in synthesizer (midiOut). Uses only the Windows SDK (winmm).
// The original sound model (0x6604e / 0x660cf / 0x6652e): sound id, volume 0..255,
// pan, pitch multiplier (16.16), fixed channels (0 engine, 1 lock tone, 2 warning).
#pragma once
#include <string>
#include <vector>

#include "res.h"

class Audio {
 public:
  bool init(const Res& res);
  void shutdown();
  bool ok() const { return wave_ != nullptr; }

  int load(const std::string& wavName);             // load a sample from RESOURCE.RES (cached); -1 if missing
  // one-shot sound: vol 0..1, pan -1 (left) .. 1 (right), pitch multiplier
  void play(int sample, float vol, float pan = 0, float pitch = 1);
  // fixed channels (0..7): looping sounds, adjustable while playing
  void chPlay(int ch, int sample, float vol, float pan, float pitch, bool loop);
  void chSet(int ch, float vol, float pan, float pitch);
  void chStop(int ch);
  int addPcm8(const std::string& key, const uint8_t* p, size_t n, int rate);   // 8-bit unsigned PCM (video audio); replaces if the key already exists
  double chPos(int ch);                              // playback position of the channel in seconds
  bool chPlaying(int ch);
  void stopAll();

  // music (MIDI): immediate switch, or queue after the end of the current track
  void musicPlay(const std::string& mid, bool loop);
  void musicQueue(const std::string& mid, bool loop);
  void musicStop();
  std::string musicCurrent();
  float sfxVol = 1.0f, musicVol = 0.8f;
  std::vector<int16_t>* record = nullptr;   // if set: the mixer output is also written here

  void mixThread();
  void midiThread();

 private:
  struct Sample { std::vector<float> d; int rate = 22050; };
  struct Voice { int s = -1; double pos = 0, step = 1; float vol = 0, pan = 0, pitch = 1; bool loop = false, on = false; };
  struct Ev { uint32_t tick; uint8_t b[3]; uint8_t n; uint32_t tempo; };   // n=0: tempo change
  struct Song { std::string name; std::vector<Ev> ev; int ppq = 240; bool loop = false; };

  bool parseMidi(const std::string& name, Song& out);
  void allNotesOff();

  const Res* res_ = nullptr;
  void* wave_ = nullptr;                             // HWAVEOUT
  void* midi_ = nullptr;                             // HMIDIOUT
  void* event_ = nullptr;
  void* cs_ = nullptr;                               // CRITICAL_SECTION*
  void* thread_ = nullptr;
  void* mthread_ = nullptr;
  volatile bool quit_ = false;
  std::vector<Sample> samples_;
  std::vector<std::string> names_;
  Voice ch_[8];
  std::vector<Voice> shots_;
  // MIDI
  std::vector<Song> queue_;
  Song cur_;
  bool curValid_ = false, restart_ = false;
  float lastMusicVol_ = -1;
};
