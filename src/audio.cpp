#include "audio.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {
constexpr int RATE = 44100, FRAMES = 882, NBUF = 4;   // 20 ms-os pufferek
WAVEHDR gHdr[NBUF];
int16_t gBuf[NBUF][FRAMES * 2];
CRITICAL_SECTION gCs;
DWORD WINAPI mixEntry(LPVOID p) { static_cast<Audio*>(p)->mixThread(); return 0; }
DWORD WINAPI midiEntry(LPVOID p) { static_cast<Audio*>(p)->midiThread(); return 0; }
struct Lock { Lock() { EnterCriticalSection(&gCs); } ~Lock() { LeaveCriticalSection(&gCs); } };
}  // namespace

bool Audio::init(const Res& res) {
  res_ = &res;
  InitializeCriticalSection(&gCs);
  cs_ = &gCs;
  WAVEFORMATEX f = {};
  f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 2; f.nSamplesPerSec = RATE; f.wBitsPerSample = 16;
  f.nBlockAlign = 4; f.nAvgBytesPerSec = RATE * 4;
  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HWAVEOUT wo = nullptr;
  if (waveOutOpen(&wo, WAVE_MAPPER, &f, DWORD_PTR(event_), 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
    wave_ = wo;
    for (int i = 0; i < NBUF; i++) {
      std::memset(gBuf[i], 0, sizeof gBuf[i]);
      gHdr[i] = {};
      gHdr[i].lpData = reinterpret_cast<LPSTR>(gBuf[i]);
      gHdr[i].dwBufferLength = sizeof gBuf[i];
      waveOutPrepareHeader(wo, &gHdr[i], sizeof(WAVEHDR));
      waveOutWrite(wo, &gHdr[i], sizeof(WAVEHDR));
    }
    thread_ = CreateThread(nullptr, 0, mixEntry, this, 0, nullptr);
  }
  HMIDIOUT mo = nullptr;
  if (midiOutOpen(&mo, MIDI_MAPPER, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
    midi_ = mo;
    mthread_ = CreateThread(nullptr, 0, midiEntry, this, 0, nullptr);
  }
  return wave_ || midi_;
}

void Audio::shutdown() {
  quit_ = true;
  if (thread_) { SetEvent(HANDLE(event_)); WaitForSingleObject(HANDLE(thread_), 1000); CloseHandle(HANDLE(thread_)); thread_ = nullptr; }
  if (mthread_) { WaitForSingleObject(HANDLE(mthread_), 1000); CloseHandle(HANDLE(mthread_)); mthread_ = nullptr; }
  if (wave_) {
    HWAVEOUT wo = HWAVEOUT(wave_);
    waveOutReset(wo);
    for (int i = 0; i < NBUF; i++) waveOutUnprepareHeader(wo, &gHdr[i], sizeof(WAVEHDR));
    waveOutClose(wo);
    wave_ = nullptr;
  }
  if (midi_) { allNotesOff(); midiOutClose(HMIDIOUT(midi_)); midi_ = nullptr; }
}

// ---------------------------------------------------------------- samples
int Audio::load(const std::string& wavName) {
  std::string n = upper(wavName);
  for (size_t i = 0; i < names_.size(); i++) if (names_[i] == n) return samples_[i].d.empty() ? -1 : int(i);
  Sample s;
  Blob b = res_ ? res_->get(n) : Blob{};
  if (b.ok() && b.n > 44 && std::memcmp(b.p, "RIFF", 4) == 0) {
    int chs = 1, bits = 16;
    size_t o = 12;
    while (o + 8 <= b.n) {                               // walk the RIFF chunks
      uint32_t id = b.u32(o), len = b.u32(o + 4);
      if (id == 0x20746D66) { chs = b.u16(o + 10); s.rate = int(b.u32(o + 12)); bits = b.u16(o + 22); }   // "fmt "
      if (id == 0x61746164) {                            // "data"
        size_t end = std::min(b.n, size_t(o + 8 + len)), bpf = size_t(chs) * (bits / 8);
        for (size_t p = o + 8; p + bpf <= end; p += bpf) {
          float v = 0;
          for (int c = 0; c < chs; c++)
            v += bits == 8 ? (b.p[p + c] - 128) / 128.0f : int16_t(b.u16(p + c * 2)) / 32768.0f;
          s.d.push_back(v / chs);
        }
        break;
      }
      o += 8 + len + (len & 1);
    }
  }
  Lock l;
  names_.push_back(n);
  samples_.push_back(std::move(s));
  return samples_.back().d.empty() ? -1 : int(samples_.size() - 1);
}

void Audio::play(int s, float vol, float pan, float pitch) {
  if (s < 0 || !wave_) return;
  Lock l;
  Voice v;
  v.s = s; v.vol = vol; v.pan = pan; v.pitch = pitch; v.on = true;
  v.step = double(samples_[s].rate) / RATE * pitch;
  if (shots_.size() > 24) shots_.erase(shots_.begin());   // the oldest one is silenced
  shots_.push_back(v);
}

int Audio::addPcm8(const std::string& key, const uint8_t* p, size_t n, int rate) {
  Sample sm;
  sm.rate = rate;
  sm.d.resize(n);
  for (size_t i = 0; i < n; i++) sm.d[i] = (p[i] - 128) / 128.0f;
  std::string k = upper(key);
  Lock l;
  for (size_t i = 0; i < names_.size(); i++)
    if (names_[i] == k) { samples_[i] = std::move(sm); return int(i); }
  names_.push_back(k);
  samples_.push_back(std::move(sm));
  return int(samples_.size() - 1);
}

double Audio::chPos(int c) {
  if (c < 0 || c >= 8) return 0;
  Lock l;
  const Voice& v = ch_[c];
  if (!v.on || v.s < 0) return -1;
  return v.pos / samples_[v.s].rate;
}

void Audio::chPlay(int c, int s, float vol, float pan, float pitch, bool loop) {
  if (c < 0 || c >= 8 || s < 0 || !wave_) return;
  Lock l;
  Voice& v = ch_[c];
  v.s = s; v.pos = 0; v.vol = vol; v.pan = pan; v.pitch = pitch; v.loop = loop; v.on = true;
  v.step = double(samples_[s].rate) / RATE * pitch;
}

void Audio::chSet(int c, float vol, float pan, float pitch) {
  if (c < 0 || c >= 8) return;
  Lock l;
  Voice& v = ch_[c];
  v.vol = vol; v.pan = pan; v.pitch = pitch;
  if (v.s >= 0) v.step = double(samples_[v.s].rate) / RATE * pitch;
}

void Audio::chStop(int c) { if (c >= 0 && c < 8) { Lock l; ch_[c].on = false; } }
bool Audio::chPlaying(int c) { Lock l; return c >= 0 && c < 8 && ch_[c].on; }
void Audio::stopAll() { Lock l; for (auto& v : ch_) v.on = false; shots_.clear(); }

void Audio::mixThread() {
  HWAVEOUT wo = HWAVEOUT(wave_);
  std::vector<float> acc(FRAMES * 2);
  while (!quit_) {
    WaitForSingleObject(HANDLE(event_), 50);
    for (int i = 0; i < NBUF && !quit_; i++) {
      if (!(gHdr[i].dwFlags & WHDR_DONE)) continue;
      std::fill(acc.begin(), acc.end(), 0.0f);
      {
        Lock l;
        auto mixVoice = [&](Voice& v) {
          if (!v.on || v.s < 0) return;
          const std::vector<float>& d = samples_[v.s].d;
          float g = v.vol * sfxVol, gl = g * std::min(1.0f, 1 - v.pan), gr = g * std::min(1.0f, 1 + v.pan);
          for (int k = 0; k < FRAMES; k++) {
            size_t p = size_t(v.pos);
            if (p >= d.size()) {
              if (!v.loop) { v.on = false; return; }
              v.pos -= double(d.size()); p = size_t(v.pos);
              if (p >= d.size()) { v.pos = 0; p = 0; }
            }
            float fr = float(v.pos - double(p)), a = d[p], b = p + 1 < d.size() ? d[p + 1] : (v.loop ? d[0] : a);
            float x = a + (b - a) * fr;
            acc[k * 2] += x * gl; acc[k * 2 + 1] += x * gr;
            v.pos += v.step;
          }
        };
        for (auto& v : ch_) mixVoice(v);
        for (auto& v : shots_) mixVoice(v);
        shots_.erase(std::remove_if(shots_.begin(), shots_.end(), [](const Voice& v) { return !v.on; }), shots_.end());
      }
      for (int k = 0; k < FRAMES * 2; k++) {                // soft saturation
        float x = acc[k];
        x = x / (1 + std::fabs(x) * 0.35f);
        gBuf[i][k] = int16_t(std::clamp(x, -1.0f, 1.0f) * 32000);
      }
      if (record) record->insert(record->end(), gBuf[i], gBuf[i] + FRAMES * 2);
      waveOutWrite(wo, &gHdr[i], sizeof(WAVEHDR));
    }
  }
}

// ---------------------------------------------------------------- MIDI
bool Audio::parseMidi(const std::string& name, Song& out) {
  Blob b = res_ ? res_->get(upper(name)) : Blob{};
  if (!b.ok() || b.n < 22 || std::memcmp(b.p, "MThd", 4) != 0) return false;
  auto be32 = [&](size_t o) { return uint32_t(b.p[o]) << 24 | uint32_t(b.p[o + 1]) << 16 | uint32_t(b.p[o + 2]) << 8 | b.p[o + 3]; };
  auto be16 = [&](size_t o) { return uint32_t(b.p[o]) << 8 | b.p[o + 1]; };
  out = Song{};
  out.name = name;
  out.ppq = int(be16(12));
  if (out.ppq <= 0 || (out.ppq & 0x8000)) out.ppq = 240;
  int ntrk = int(be16(10));
  size_t o = 8 + be32(4);
  for (int t = 0; t < ntrk && o + 8 <= b.n; t++) {
    if (std::memcmp(b.p + o, "MTrk", 4) != 0) break;
    size_t end = std::min(b.n, size_t(o + 8 + be32(o + 4))), p = o + 8;
    uint32_t tick = 0;
    uint8_t run = 0;
    auto vlq = [&]() { uint32_t v = 0; while (p < end) { uint8_t c = b.p[p++]; v = v << 7 | (c & 0x7F); if (!(c & 0x80)) break; } return v; };
    while (p < end) {
      tick += vlq();
      if (p >= end) break;
      uint8_t st = b.p[p];
      if (st == 0xFF) {                                    // meta
        uint8_t type = b.p[p + 1]; p += 2;
        uint32_t len = vlq();
        if (type == 0x51 && len == 3) out.ev.push_back({tick, {0, 0, 0}, 0, uint32_t(b.p[p]) << 16 | uint32_t(b.p[p + 1]) << 8 | b.p[p + 2]});
        p += len;
        if (type == 0x2F) break;
        continue;
      }
      if (st == 0xF0 || st == 0xF7) { p++; uint32_t len = vlq(); p += len; continue; }   // sysex: kihagyjuk
      if (st & 0x80) { run = st; p++; }
      if (!run) break;
      int n = ((run & 0xF0) == 0xC0 || (run & 0xF0) == 0xD0) ? 1 : 2;
      Ev e{tick, {run, 0, 0}, uint8_t(1 + n), 0};
      for (int k = 0; k < n && p < end; k++) e.b[1 + k] = b.p[p++];
      out.ev.push_back(e);
    }
    o = end;
  }
  std::stable_sort(out.ev.begin(), out.ev.end(), [](const Ev& a, const Ev& c) { return a.tick < c.tick; });
  return !out.ev.empty();
}

void Audio::allNotesOff() {
  if (!midi_) return;
  for (int c = 0; c < 16; c++) {
    midiOutShortMsg(HMIDIOUT(midi_), DWORD(0xB0 | c) | 123 << 8);
    midiOutShortMsg(HMIDIOUT(midi_), DWORD(0xB0 | c) | 121 << 8);
  }
}

void Audio::musicPlay(const std::string& mid, bool loop) {
  Song s;
  if (!parseMidi(mid, s)) return;
  s.loop = loop;
  Lock l;
  queue_.clear();
  cur_ = std::move(s); curValid_ = true; restart_ = true;
}
void Audio::musicQueue(const std::string& mid, bool loop) {
  Song s;
  if (!parseMidi(mid, s)) return;
  s.loop = loop;
  Lock l;
  if (!curValid_) { cur_ = std::move(s); curValid_ = true; restart_ = true; }
  else queue_.push_back(std::move(s));
}
void Audio::musicStop() { Lock l; queue_.clear(); curValid_ = false; restart_ = true; }
std::string Audio::musicCurrent() { Lock l; return curValid_ ? cur_.name : std::string(); }

void Audio::midiThread() {
  HMIDIOUT mo = HMIDIOUT(midi_);
  using clk = std::chrono::steady_clock;
  Song song;
  bool playing = false;
  size_t idx = 0;
  uint32_t lastTick = 0, tempo = 500000;
  double tUs = 0;                                          // time of lastTick (µs) from the start of the track
  clk::time_point t0 = clk::now();
  int chanVol[16];
  auto sendVol = [&]() { for (int c = 0; c < 16; c++) midiOutShortMsg(mo, DWORD(0xB0 | c) | 7 << 8 | DWORD(std::clamp(int(chanVol[c] * musicVol), 0, 127)) << 16); };
  while (!quit_) {
    {
      Lock l;
      if (restart_) {
        restart_ = false;
        allNotesOff();
        playing = curValid_;
        if (playing) song = cur_;
        idx = 0; lastTick = 0; tempo = 500000; tUs = 0; t0 = clk::now();
        for (int& v : chanVol) v = 100;
        if (playing) { sendVol(); lastMusicVol_ = musicVol; }
      }
      if (playing && lastMusicVol_ != musicVol) { sendVol(); lastMusicVol_ = musicVol; }
    }
    if (!playing) { Sleep(20); continue; }
    double now = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    while (idx < song.ev.size()) {
      const Ev& e = song.ev[idx];
      double et = tUs + double(e.tick - lastTick) * tempo / song.ppq;
      if (et > now) break;
      tUs = et; lastTick = e.tick;
      if (e.n == 0) tempo = e.tempo;
      else {
        uint8_t b1 = e.b[1], b2 = e.b[2];
        if ((e.b[0] & 0xF0) == 0xB0 && b1 == 7) { chanVol[e.b[0] & 15] = b2; b2 = uint8_t(std::clamp(int(b2 * musicVol), 0, 127)); }
        midiOutShortMsg(mo, DWORD(e.b[0]) | DWORD(b1) << 8 | DWORD(b2) << 16);
      }
      idx++;
    }
    if (idx >= song.ev.size()) {                            // end: next in queue, repeat or silence
      Lock l;
      if (!queue_.empty()) { cur_ = std::move(queue_.front()); queue_.erase(queue_.begin()); curValid_ = true; restart_ = true; }
      else if (song.loop) { idx = 0; lastTick = 0; tUs = 0; t0 = clk::now(); }
      else { curValid_ = false; playing = false; allNotesOff(); }
      continue;
    }
    Sleep(2);
  }
}
