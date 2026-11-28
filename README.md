# F-22 Lightning II – native rewrite

A native Windows rewrite of NovaLogic's **F-22 Lightning II** (1996, DOS) in C++17 with Win32 and OpenGL 3.3.
It uses no external libraries (only the Windows SDK: user32, gdi32, opengl32, winmm; the launcher also comctl32, shell32, ole32), no DOSBox and no emulation.
The program reads the original game data (`RESOURCE.RES`) and draws and simulates it with its own engine. All game rules
come from reverse engineering the original `F22.EXE`; the addresses of the original routines are quoted in the code.

**The game data is not included.** You need your own copy of the game. Nothing from the original game is stored in this
repository (no `F22.EXE`, `RESOURCE.RES`, no decrypted image, no videos). The program reads your own `F22.EXE` and
`RESOURCE.RES` at run time; the encrypted `F22.EXE` is decrypted in memory, nothing is written back to the game.

## What you need and where it goes

Copy `f22native.exe` and `f22launcher.exe` into your installed F-22 game folder (the one that holds `F22.EXE` and `RESOURCE.RES`).
The program searches for `RESOURCE.RES` next to the exe and up to four folders above it, so the exe may also sit in a subfolder of the
game folder; `F22.EXE` is looked for next to `RESOURCE.RES`, then next to the exe. `--res <path>` / `--exe <path>` (or "Browse" in the
launcher) point to a game folder elsewhere.

| File / folder | Required | Where it comes from | Purpose |
|---|---|---|---|
| `f22native.exe` | yes | built from this repository, or the release zip | the game |
| `f22launcher.exe` | optional | built from this repository, or the release zip | the launcher: settings, game folder, texture export, Play |
| `F22.EXE` | yes | your installed game (661 089 bytes, the DOS program) | the original program; it is decrypted in memory at start-up and the game rules (flight model, weapon tables, AI scripts) are read from it |
| `RESOURCE.RES` | yes | your installed game (the file in the game folder, ~33 MB) | terrain, 3D models, textures, menus, sounds, missions |
| `video\*.KDV` | optional | extracted from `RESOURCE.RES` of the **CD** (~409 MB), see below | intro, medal, ending and credits videos; without them the videos are silently skipped |
| `textures\*.png` | optional | made by the program itself (launcher: "Export textures", or `--dumptex`), then edited by you | replaced textures |
| `f22native.sav` | created | the program | settings, campaign progress, pilot statistics |
| `f22launcher.ini` | created | the launcher | the launcher's own choices (game folder, window size, skip intro, original AI) |

Minimal layout:
```
F-22 game folder\
  F22.EXE           (the original game files stay as they are)
  RESOURCE.RES
  f22native.exe
  f22launcher.exe   (optional)
  video\            (optional) HDOLBY.KDV HAIR.KDV HF22INTR.KDV ... NOVA_640.KDV
  textures\         (optional) F2201.png, terrain_T1_000.png, ...
```

### 1. The original program (`F22.EXE`)
`F22.EXE` is encrypted: a FlashTek X-386 DOS extender with a start-up stub that decrypts the 32-bit image in two XOR passes (the second
with a rolling key). `sim::Exe::loadGameExe` (`src/sim.cpp`) repeats those passes in memory at every start, so no extra file is needed.
The result is the flat image (file offset = virtual address) that the reverse-engineering notes refer to.

For reverse engineering you may still want that image as a file: `tools/f22unpack.py` runs the original stub in the Unicorn CPU emulator
and writes it (`py -m pip install unicorn`, then `py tools\f22unpack.py --exe "<game folder>\F22.EXE" -o "<game folder>\F22_decrypted.bin"`).
The program accepts such an `F22_decrypted.bin` (or `extracted\F22_decrypted.bin`) next to `RESOURCE.RES` when `F22.EXE` is not found, and
`f22native.exe --exetest` checks that the in-memory decryption gives the same bytes (result in `exetest.txt`). Keep the image out of any public
place: it is derived from the original program.

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
"Export textures" in the launcher (or `f22native.exe --dumptex`) writes every model texture and every terrain tile
of the original as PNG into `textures\`. Edit or replace a file under the same name and the program uses it instead of the original;
delete it to get the original back. Details are in `docs/FEATURES.md` ("Replaceable textures"). In the options (OPTIONS 2) there is also
a switch for automatically upscaled HD textures.

## Running
Start **`f22launcher.exe`** and press **Play**, or start `f22native.exe` directly. The launcher shows whether the game files were found
(and lets you pick the game folder with "Browse" if they are elsewhere) and sets, before the game starts:

| Launcher setting | Stored in | Same as |
|---|---|---|
| HD textures, Fullscreen, Sound effects, Music | `f22native.sav` (the game's own settings) | OPTIONS screens in the game, Alt+Enter |
| Window size | `f22launcher.ini` | `--size 1920x1080` |
| Skip the intro videos | `f22launcher.ini` | `--nointro` |
| Original AI scripts (experimental) | `f22launcher.ini` | `--aivm` |
| Game folder chosen with "Browse" | `f22launcher.ini` | `--res <dir>\RESOURCE.RES --exe <dir>\F22.EXE` |

"Export textures" runs the texture dump in the background, "Open textures folder" opens `textures\`, "Read me" opens this file.

Command line:
```
f22native.exe                       original menu: quick mission, campaigns, briefing, map
f22native.exe C03M01                start a mission directly (no menu)
f22native.exe --aivm                experimental: air AI and ground defences run the original AI bytecode scripts
f22native.exe --hd                  HD (upscaled) textures
f22native.exe --res <RESOURCE.RES>  use another game data path
f22native.exe --exe <F22.EXE>       use another original program path
f22native.exe --size 1600x900       window size (default 1280x720)
f22native.exe --nointro             skip the start-up videos
```
Keys follow the original layout by **scan code** (physical key position); the full table, the views, the autopilot and the wingman commands
are in `docs/FEATURES.md`. Self-tests: `--autotest`, `--menutest`, `--flowtest`, `--viewtest`, `--geartest`, `--fxtest`, `--wingtest`, `--ilstest`,
`--soundtest`, `--aivmtest`, `--aiflight`, `--createtest`, `--ejecttest`, `--exetest` (they write logs and screenshots next to the exe).

## Building
Windows only. CMake 3.20+ and either Visual Studio 2022 (MSVC) or MinGW-w64 / clang; no other dependency (the Khronos OpenGL headers are in `third_party/`).
```
build.bat                                  (checks the tools, builds both exes, makes dist\F22Native\)
cmake -S . -B build -A x64 && cmake --build build --config Release      (Visual Studio)
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ && cmake --build build   (Ninja + clang / g++)
```
The build makes `f22native.exe` (the game) and `f22launcher.exe` (the launcher). The MSVC build links the C runtime statically, so neither needs a redistributable. GitHub Actions builds the exe on every push and attaches
a zip to releases made from a tag like `v1.0`.

## Repository layout
- `src/` – the program (`sim.cpp` simulation and the in-memory `F22.EXE` decryption, `world.cpp` renderer and models, `menu.cpp` original menus, `hud.cpp`, `audio.cpp`, `fx.cpp`, `kdv.cpp` video,
  `aivm.cpp` / `aiunit.cpp` / `aibridge.cpp` original AI bytecode interpreter and its host, `imgio.cpp` PNG/BMP I/O); `src/launcher/` – the launcher.
- `tools/` – Python reverse-engineering tools (`f22unpack.py`, `f22res.py`, `f22model.py`, `f22terrain.py`, `f22script.py`, `f22kdv.py`) and `package.py` (release zip).
- `docs/FEATURES.md` – behaviour, controls and approximations; `docs/AI_VM.md` – the original AI bytecode machine; `CLAUDE.md` – handover notes for development.

## Status
Playable: all campaigns and missions, the original menus and medals, mission goals and triggers, flight model, weapons, AI, wingman commands, HUD and MFD
pages, sound and music, videos, create-mission, plus additions (launcher, fullscreen, window size, G-effect switch, ILS, HD / replaceable textures). Known approximations are listed
in `docs/FEATURES.md`. Multiplayer is not implemented.

This is a fan project for personal use and preservation; it is not affiliated with NovaLogic.
