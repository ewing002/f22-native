# F-22 Lightning II – native rewrite

A native Windows rewrite of NovaLogic's **F-22 Lightning II** (1996, DOS) in C++17 with Win32 and OpenGL 3.3.
It uses no external libraries (only the Windows SDK: user32, gdi32, opengl32, winmm), no DOSBox and no emulation.
The program reads the original game data (`RESOURCE.RES`) and draws and simulates it with its own engine. All game rules
come from reverse engineering the original `F22.EXE`; the addresses of the original routines are quoted in the code.

**The game data is not included.** You need your own copy of the game. Nothing from the original game is stored in this
repository (no `F22.EXE`, `RESOURCE.RES`, no decrypted image, no videos).

## What you need and where it goes

Everything below goes into **one folder** (the "game folder"). The program searches for `RESOURCE.RES` next to the exe and up to
four folders above it, so the exe may also sit in a subfolder of the game folder; `--res <path>` overrides the search.

| File / folder | Required | Where it comes from | Purpose |
|---|---|---|---|
| `f22native.exe` | yes | built from this repository, or the release zip | the game |
| `RESOURCE.RES` | yes | your installed game (the file in the game folder, ~33 MB) | terrain, 3D models, textures, menus, sounds, missions |
| `F22_decrypted.bin` | yes | made from your `F22.EXE`, see below; may also be in `extracted\F22_decrypted.bin` | the decrypted flat image of the original program; the game rules (flight model, weapon tables, AI scripts) are read from it at run time |
| `video\*.KDV` | optional | extracted from `RESOURCE.RES` of the **CD** (~409 MB), see below | intro, medal, ending and credits videos; without them the videos are silently skipped |
| `textures\*.png` | optional | made by the program itself (`textura_kiiras.bat` / `--dumptex`), then edited by you | replaced textures |
| `f22native.sav` | created | the program | settings, campaign progress, pilot statistics |

Minimal layout:
```
F-22 game folder\
  f22native.exe
  RESOURCE.RES
  F22_decrypted.bin
  video\            (optional) HDOLBY.KDV HAIR.KDV HF22INTR.KDV ... NOVA_640.KDV
  textures\         (optional) F2201.png, terrain_T1_000.png, ...
```

### 1. `F22_decrypted.bin`
The original `F22.EXE` is encrypted (FlashTek X-386 DOS extender stub + XOR/rotate encrypted image). `tools/f22unpack.py` runs the
original unpacking stub inside the Unicorn CPU emulator and dumps the decrypted flat image (file offset = virtual address). It never modifies the game.
```
py -m pip install unicorn
py tools\f22unpack.py --exe "<game folder>\F22.EXE" -o "<game folder>\F22_decrypted.bin"
```
The image is 2 MiB. Keep it out of any public place: it is derived from the original program.

### 2. Videos (optional)
The CD version has the videos (NovaLogic KDV files) inside the CD's own `RESOURCE.RES` (409 MB, a different file from the 33 MB one
in the installed game). Extract the KDV files next to the exe:
```
py tools\f22videos.py "<CD>\RESOURCE.RES" "<game folder>\video"
```
The program uses `HDOLBY`, `NOVA_640`, `HF22INTR` (startup), `HAIR`, `HFLY`, `HSTAR`, `HAFC`, `HPRES` (after campaigns 1-5),
`HEND`, `HSECRET`, `HCREDITS` (all `.KDV`). Playback: any key or click skips. The format is documented in `docs/FEATURES.md`;
`tools/f22kdv.py` converts a KDV to MP4/WAV for inspection.

### 3. Replaceable textures (optional)
`f22native.exe --dumptex` (or double-click `textura_kiiras.bat` from the release zip) writes every model texture and every terrain tile
of the original as PNG into `textures\`. Edit or replace a file under the same name and the program uses it instead of the original;
delete it to get the original back. Details are in `docs/FEATURES.md` ("Replaceable textures"). In the options (OPTIONS 2) there is also
a switch for automatically upscaled HD textures.

## Running
```
f22native.exe                       original menu: quick mission, campaigns, briefing, map
f22native.exe C03M01                start a mission directly (no menu)
f22native.exe --aivm                experimental: air AI and ground defences run the original AI bytecode scripts
f22native.exe --hd                  HD (upscaled) textures
f22native.exe --res <RESOURCE.RES>  use another game data path
```
Keys follow the original layout by **scan code** (physical key position); the full table, the views, the autopilot and the wingman commands
are in `docs/FEATURES.md`. Self-tests: `--autotest`, `--menutest`, `--flowtest`, `--viewtest`, `--geartest`, `--fxtest`, `--wingtest`, `--ilstest`,
`--soundtest`, `--aivmtest`, `--aiflight`, `--createtest`, `--ejecttest` (they write logs and screenshots next to the exe).

## Building
Windows only. CMake 3.20+ and either Visual Studio 2022 (MSVC) or MinGW-w64 / clang; no other dependency (the Khronos OpenGL headers are in `third_party/`).
```
build.bat                                  (checks the tools, builds, makes dist\F22Native\)
cmake -S . -B build -A x64 && cmake --build build --config Release      (Visual Studio)
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ && cmake --build build   (Ninja + clang / g++)
```
The MSVC build links the C runtime statically, so the exe needs no redistributable. GitHub Actions builds the exe on every push and attaches
a zip to releases made from a tag like `v1.0`.

## Repository layout
- `src/` – the program (`sim.cpp` simulation, `world.cpp` renderer and models, `menu.cpp` original menus, `hud.cpp`, `audio.cpp`, `fx.cpp`, `kdv.cpp` video,
  `aivm.cpp` / `aiunit.cpp` / `aibridge.cpp` original AI bytecode interpreter and its host, `imgio.cpp` PNG/BMP I/O).
- `tools/` – Python reverse-engineering tools (`f22unpack.py`, `f22res.py`, `f22model.py`, `f22terrain.py`, `f22script.py`, `f22kdv.py`) and `package.py` (release zip).
- `docs/FEATURES.md` – behaviour, controls and approximations; `docs/AI_VM.md` – the original AI bytecode machine; `CLAUDE.md` – handover notes for development.

## Status
Playable: all campaigns and missions, the original menus and medals, mission goals and triggers, flight model, weapons, AI, wingman commands, HUD and MFD
pages, sound and music, videos, create-mission, plus additions (fullscreen, G-effect switch, ILS, HD / replaceable textures). Known approximations are listed
in `docs/FEATURES.md`. Multiplayer is not implemented.

This is a fan project for personal use and preservation; it is not affiliated with NovaLogic.
