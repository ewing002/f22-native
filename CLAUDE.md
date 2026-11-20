# F-22 Lightning II native rewrite – handover notes for Claude Code

A native Windows rewrite of NovaLogic's **F-22 Lightning II** (1996, DOS) in C++17 / Win32 / OpenGL 3.3 with **no external
libraries and no web technology** (standing rule from the owner: no web-based solution, no JavaScript). Goal: **behave exactly
like the original.** Game rules come from reverse engineering `F22.EXE` (decrypted flat image, file offset = virtual address),
never from invention; where the original is not known the code and `docs/FEATURES.md` say "approximation".

The program contains no original data: it reads `RESOURCE.RES` and `F22_decrypted.bin` (made from the owner's own `F22.EXE`) at run time.
**Never commit** `F22.EXE`, `RESOURCE.RES`, `F22_decrypted.bin`, videos, extracted resources or dumped textures (see `.gitignore`).

Write reports, docs and code comments in English. The owner writes Hungarian in the chat; answer in Hungarian there.

## Working with the owner
- "go on" = continue with the next step, then build, test and report briefly.
- The owner runs the Windows exe from a game folder holding `RESOURCE.RES`, `F22_decrypted.bin`, `video\` (see README.md). After a change that is
  worth playing, build, run the self-tests, copy the exe (and docs) into the owner's package and rebuild its zip.
- Settings and progress: `f22native.sav` next to the exe (do not overwrite the owner's save when testing: run tests from a copy of the folder).
- Personal-use project ("a game of my childhood"); the GitHub repository is private.

## Files at run time
| File | Role |
|---|---|
| `RESOURCE.RES` | the installed game's resource archive (terrain, models, menus, sounds, missions); read by `src/res.cpp` |
| `F22_decrypted.bin` (or `extracted\F22_decrypted.bin`) | decrypted program image: tables for flight model, weapons, perf records, AI bytecode (0x374c0-0x37a40), key tables |
| `video\*.KDV` | videos from the CD's RESOURCE.RES (`tools/f22videos.py`) |
| `textures\*.png` | optional replaced textures (`--dumptex` creates them) |

## Build
```
cmake -S . -B build -A x64 && cmake --build build --config Release        # Visual Studio 2022 / MSVC (CI uses this)
cmake -S . -B build2 -G Ninja -DCMAKE_CXX_COMPILER=clang++ && cmake --build build2    # the owner's usual dev build (clang, MinGW headers)
```
New .cpp files go into `CMakeLists.txt`. OpenGL extension headers are vendored in `third_party/` (MSVC's SDK lacks them). `NOMINMAX` is defined by CMake.
On the owner's machine the `python` command is the Store alias: **use `py`** (3.14, with capstone, pillow, unicorn). The MSVC build tools are in
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` (`vcvars64.bat`).
Editing tip (Windows): avoid patching C++ with Python heredocs through bash when the text contains `\n` escapes (they get turned into real newlines);
use the Edit tool or write the patch script to a file.

## Tests (all must return rc=0; run from a copy of the game folder)
`--autotest` (flight self-test: log + screenshots), `--menutest`, `--flowtest`, `--viewtest`, `--geartest`, `--fxtest`, `--wingtest`, `--ilstest`, `--soundtest`,
`--createtest`, `--ejecttest`, `--aivmtest` (AI interpreter + ported routines), `--aiflight` (one aircraft driven only by the original scripts);
`--autotest --aivm` and `--wingtest --aivm` run the same with the original AI scripts. Logs: `autotest.txt`, `viewtest.txt`, `aivmtest.txt`, `aiflight.txt`.

## Reverse-engineering workflow
1. `py tools\f22unpack.py` -> `F22_decrypted.bin` (Unicorn runs the original unpacker).
2. Disassemble with capstone (32-bit, flat addresses), e.g. `Cs(CS_ARCH_X86, CS_MODE_32).disasm(data[addr:addr+0x400], addr)`; stop-at-`ret` linear sweeps are enough for most routines.
3. Port the routine to C++ with the original integer arithmetic, quote the address in a comment, add a check to a `--*test` switch.
4. `tools/f22script.py` disassembles the AI bytecode; `tools/f22res.py`, `f22model.py`, `f22terrain.py`, `f22kdv.py` read the resource formats.
Key facts: PIT clock 120 Hz (simulation step 40 Hz); world 65536 wu (16.16 fixed point, wraps); angles 2^25 = 360 degrees; terrain 512x512 height grid of 128 wu cells,
ground test = max of four corners (0x5271a) then interpolated height (0x52796); score formula 0x105c0; target selection 0x30edd / 0x30fba / 0x30d2e.

## Architecture
- `sim.cpp/.h` – world, units, flight model of the player (0x2dc22), weapons, native AI state machine, triggers (`World::respond`, `World::triggerEvent` = original event dispatcher 0x23cc0).
- `aivm.cpp` – the original AI bytecode interpreter (0x36d17), `aiunit.cpp/.h` – the original unit record (`OUnit`, offsets in `of::`), `OHost` (all ported helper routines and instructions),
  `OGroups` (group / wing chains), `aibridge.cpp` – the experimental bridge that lets the VM drive the game's air AI and ground defences (`--aivm`). See `docs/AI_VM.md`.
- `world.cpp` – terrain and model renderer, textures (HD upscaler, replaceable textures), `hud.cpp`, `menu.cpp` (original menu screens and widget tables), `audio.cpp` (own mixer on waveOut + MIDI),
  `fx.cpp` (explosions, smoke, debris), `kdv.cpp` (video codec), `imgio.cpp` (PNG/BMP reader, PNG writer, own inflate).

## Open items
- Original VM-driven AI is experimental: it does not yet replace the native AI by default (the owner's comparison with the original game is missing).
- Create-mission generator, MFD ring radii, flare/chaff sound call site, OCV030/031/040 voice triggers, sun size are approximations (see `docs/FEATURES.md`).
- The wingman does not wait for the player in the original exactly like here (the original launch trigger was not found); see "Known approximations".
- Multiplayer is out of scope.
