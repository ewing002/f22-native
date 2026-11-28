# F-22 Lightning II native rewrite – feature and behaviour notes

A native Windows program (C++17, Win32 + OpenGL 3.3) with no external libraries and no web technology.

- **Data:** read directly from the original `RESOURCE.RES` (terrain, 3D models, missions).
- **Game rules:** reverse engineered from the original `F22.EXE` – flight model, weapons,
  enemy AI, SAM / anti-aircraft guns, mission triggers (`src/sim.cpp`, with the original
  addresses in the code). At run time the tables are read from the original program: the player's own `F22.EXE`
  is decrypted in memory (`sim::Exe::loadGameExe`, the two XOR passes of its start-up stub 0x2514; an `F22_decrypted.bin`
  is accepted as a fallback), so the game's data is not in the source code.
- **Launcher:** `f22launcher.exe` (`src/launcher/launcher.cpp`) – game folder check / choice, HD textures, fullscreen, window size,
  volumes, skip intro, original AI, texture export; it writes the game's `f22native.sav` "opt" line and `f22launcher.ini`.

## Command-line switches

```
build\f22native.exe             (original menu: quick mission, campaigns, briefing, map)
build\f22native.exe C03M01      (a mission directly, without the menu)
build\f22native.exe --autotest  (flight self-test: log + screenshots into the build folder)
build\f22native.exe --menutest  (an image of every menu screen: menu_*.bmp)
build\f22native.exe --flowtest  (menu -> mission -> debriefing -> another mission)
build\f22native.exe --viewtest C04M01      (images of the F1–F8 views: view_*.bmp; autopilot trial: viewtest.txt)
build\f22native.exe --geartest C04M01      (images of landing gear, weapon bay, flaps, ailerons: gear_*.bmp)
build\f22native.exe --fxtest C04M01        (images of the effects: fx_*.bmp)
build\f22native.exe --wingtest C04M01      (trial of the wingman commands: viewtest.txt)
build\f22native.exe --topdown C04M01 2500   (the airfield from above, from 2500 m: topdown_*.bmp;
                                             negative altitude: a low view along the runway)
build\f22native.exe --exe <F22.EXE>         (the original program elsewhere; default: next to RESOURCE.RES, then next to the exe)
build\f22native.exe --exetest              (in-memory decryption of F22.EXE vs. F22_decrypted.bin: exetest.txt)
build\f22native.exe --size 1600x900        (window size, default 1280x720)
build\f22native.exe --nointro              (no start-up videos)
```

The campaign progress and the pilot statistics are stored in `build\f22native.sav`.

## Menu and HUD

- Options (OPTIONS / MORE OPTIONS, the original screens): volume of sound effects and music (bar, -/+ buttons or dragging), sky detail (HIGH: cloud layer, LOW: none), terrain texturing distance (35–100 % of the view distance), joystick / game controller on / off. The screen resolution, the object and terrain resolution, the throttle, the pedal and the "redouts / blackouts" field are shown dimmed and have no effect (the window can be resized freely; the G effect is always on, see below). The settings are stored in the `f22native.sav` file.
- Menu: the original 640x480 screens (PCX) and button sprites; the positions of buttons, list rows and pagers
  come from the original widget tables (e.g. main menu 0x12190), the positions of the texts from the drawing routines,
  font: FATFNT.BMP, colours: the `~1`–`~4` gradients of the background's palette. There is no multiplayer mode and no
  credits roll.
- HUD: the elements of the original 0x369ce routine in a 320x240 base unit, scaled up with the resolution
  (SMALL320/SMALL640.BMP fonts): heading tape with waypoint marker, speed and altitude tapes
  with readout boxes, roll scale (10°, negative degrees dotted), cannon aiming pipper, missile circle and
  lock diamond, target box / off-screen target indicator, left and right data blocks, weapon row,
  GEAR/FLAPS/BRAKE/RADAR, stall warning, mission caption, messages. The field of view of the 3D image follows
  the original focal length (256 pixels / 240 lines).
- Sound (src/audio.cpp): an own mixer on Win32 waveOut and a MIDI player on the Windows built-in
  synthesizer, without external libraries. The original sound identifiers (0x65c60 table) and call sites:
  missile launch, cannon, anti-aircraft gun, explosion, "yeehaw" on the player's own air victory, tyre squeal,
  landing gear, flaps, brake, weapon bay, fly-by (within 40 wu, ≥550 knots), lock tone
  (tracking: BEEP at double pitch, locked: TONE2), missile warning, the voice recordings of the missions'
  radio messages, menu click. The engine (0x2ccc9) takes its pitch from the thrust and falls silent below 59 %;
  it is fixed in the cockpit and gets quieter with distance from outside.
- Music (0xea60): menu intro and loop, mission start (QINTRO, TAXILOOP), takeoff
  (TAKEOFF, SOARINTR, SOARLOOP), destruction of an enemy / friendly aircraft, crash (CRSHBURN, DIELOOP),
  mission completion (F-22WIN), landing; in combat the ACT3 / ACT5 pieces according to the threat.
- Time of day (the upper 4 bits of byte 0 of the ORF: day, night, dawn, overcast, sunset): the original swaps the whole palette
  using the PALttnn.PCX palettes (0x2309c). In region 4 this matches the tiles' palette by index,
  so we fitted an affine colour mapping for each time of day (~2 colour values
  deviation) and apply it to the final 3D colour in every region; the sky / fog colour was measured from the original
  screenshots (PAL04nn). Clouds: SKY5.BMP on a single plane at 2000 wu altitude (0x72b0).
  Sun / moon: SUN01.PCX, MOON64.PCX. For trying out: `--tod 1..5` overrides the mission's time of day.
- Transparent canopy: the models' 0x80 material flag (brightness modulating, 128 = unchanged) also applies to a material
  without a name – the F-22's canopy (colour 80) is a darkening glass through which the pilot and the instrument panel can be seen.
- Moving parts (0x2ad27, the angles of the models' sub-part records, 0x40bd3): the nose gear turns forward, the two
  main gears turn inward by 90° according to the gear position (1.6 s), and disappear only when fully retracted;
  ailerons and elevators follow the stick, flaps (42° / 14°), speed brake, weapon bay doors
  (90°), afterburner flame. The rotation is about the part's attachment point; in the left-handed
  original a positive angle rotates in the opposite direction. This is not yet wired up for the AI aircraft.
- Effects (src/fx.cpp): missile smoke trail (one STRAIL piece per step in the missile's direction, 0.8 s;
  Sidewinder, JDAM and the enemy missiles – the player's AMRAAM has none, 0x39034 +0x30);
  on the spot of a destroyed ground target a crater (POTHOLE), 6 flying apart, smoking fireballs, a flame burning for 30 s
  (FLAMES), from which a drifting, growing smoke column rises (FSMOKE); a shot-down aircraft crashes smoking,
  explodes at impact and burns (0x2bbc2). Debris (DEBRIS, 0x317b4): for an air target 5–12, for a ground one 5 pieces, flying apart in random directions, slowing down and
  tumbling (160 steps). Model shadow (our own addition – the original only has the terrain's shadow map):
  a flat, translucent projection of the aircraft and ground units onto the ground plane in the direction of the sunlight. The AI aircraft's landing gear is not animated in the original either (only the
  F-22's), so we hide their wheels in the air.
- The image has the original orientation: the left-handed world is brought into right-handed OpenGL with an X mirroring
  (previously the image was the mirror image of the original).

## Controls (the original layout, 0x283e5 / 0x27b86 / 0x2998c)

Keys are handled by scan code, like in the original, so the physical position of the key matters, not its
label. On a Hungarian keyboard: thrust 100 % = ö, 110 % = ü (the position of the American "0" and "-"),
' = á, [ = ő, ] = ú; the Hungarian "0" key has no function.

| Key | Function |
|---|---|
| ↑ / ↓ (grey arrows) | nose down / up (stick) |
| ← / → | roll; also steering on the ground |
| Del / PgDn | rudder left / right |
| Space | fire (cannon: held; missile/bomb: fires after the weapon bay opens) |
| 5, 6, 7, 8, 9, 0, - | thrust 0 / 60 / 70 / 80 / 90 / 100 / 110 % (110 = afterburner) |
| 1 / 2 / 3 / 4 | AMRAAM / Sidewinder / cannon / JDAM |
| W | weapon switch |
| Tab / Shift+Tab | next / previous target in the 30.9° cone in front of the nose (a unit airborne for an air weapon, a unit standing on the ground for JDAM; it does not look sideways) |
| ' | the unit nearest the crosshair in the 12.7° cone |
| Enter | shoot list: the 4 nearest enemy targets (air for an air weapon, mission target for JDAM), the first becomes the target; after a crash: restart |
| [ / ] | previous / next target on the shoot list |
| M | mission goals on / off |
| Ctrl+J | eject |
| G | landing gear (does not move above 350 knots or on the ground) |
| F | flaps (cannot be lowered above 400 knots) |
| B | speed brake + wheel brake |
| C | countermeasures (one flare + one chaff) |
| R | radar on/off (needed for the AIM-120; harder to be detected when off) |
| A | autopilot: AUTO PILOT (flies to the waypoint) |
| L | autopilot: AUTO LEVELING (levels out) |
| H | home: the second-to-last waypoint + AUTO PILOT |
| N | next waypoint |
| F1 … F8 | views (see below); pressing the same key again: the other variant of the view |
| Alt+Enter, or the top-left switch of the OPTIONS 2 screen (WINDOW / FULL) | full screen on / off (borderless, stretched to the monitor size); an addition, not in the original. The switch is in the original "THROTTLE" row, because the row's caption is part of the background image |
| , / . | HUD brightness decrease / increase in 20 % steps (20–100 %); an addition, not in the original |
| Ctrl/Alt + arrows | rotating the view (virtual cockpit, padlock, external view) |
| Ctrl+P | wingman: back to the patrol route ("WING - FOLLOWING PATROL ROUTE") |
| Ctrl+F | wingman: back into formation, does not attack ("RETURNING TO FORMATION") |
| Ctrl+E | wingman: attack the nearest enemy within 6080 wu ("ENGAGING ENEMY") |
| Ctrl+M | wingman: attack the player's target ("USING YOUR TARGET FOR ATTACK") |
| Ctrl+C | wingman: attack the aircraft threatening the player ("GOT YOUR BACK") |
| Alt+W | view back to the default |
| S / X | external view: zoom in / out (faster with Ctrl/Alt) |
| F9 / F10 / F11 / F12 | time acceleration 1x / 2x / 4x / 6x |
| numeric 7 / 9 / 4 / 5 / 6 / 2 | full-screen MFD page: HUD repeater (message log), artificial horizon, defensive, navigation, attack, weapons; pressing the same key again: back |
| S / X (in MFD view) | map scale 10 / 20 / 40 / 80 nautical miles |
| P | pause |
| I | inverted elevator (our own addition) |
| joystick | X / Y axis: stick; 3rd axis (if present): thrust; 4th axis: rudder. Buttons (from 0): 0 fire, 1 weapon switch, 2 next air target, 3 countermeasures, 4 landing gear, 5 flaps, 6 brake, 7 radar, 8 cockpit / external view. The keyboard works in parallel |
| game controller (XInput, Xbox) | left stick: stick (forward = nose down); right stick X: rudder; RT: fire; LT: brake; A weapon switch, B countermeasures, X / D-pad left: next / previous air target, Y cockpit / external view, LB landing gear, RB flaps, D-pad up / down: thrust (repeats when held), right: radar, right stick click: nearest target, left stick click: AUTO LEVELING, Start: pause, Back: end of mission |
| Esc / End | end of mission, debriefing (with a direct launch Esc: exit) |

### Views (0x2998c table)

| Key | View |
|---|---|
| F1 | PLAYER VIRTUAL – 3D cockpit, the head can be rotated with Ctrl/Alt + arrows (up/down ±70°) |
| F2 | PLAYER COCKPIT (cockpit + HUD), again: PLAYER FULL VIEW (HUD only) – the mission starts with this |
| F3 | PLAYER EXTERNAL – an orbiting camera around the aircraft, initially 28° from the side, 14° from above |
| F4 | PLAYER FLYBY – a stationary camera 300 wu in front of the aircraft; it relocates after 375 wu |
| F5 | PADLOCK VIEW – the head follows the designated target |
| F6 | WINGMAN EXTERNAL / FLYBY – the wingman aircraft (if there is one) |
| F7 | MISSILE FLYBY / EXTERNAL – the friendly missile fired most recently |
| F8 | TARGET EXTERNAL / FLYBY – the designated target |

The name of the view is shown in red text for 10 seconds (top right, 0x548a7). If the subject of the view
does not exist, the key has no effect; if it ceases to exist during the flight (e.g. the missile hit), F2 comes back.
The HUD instruments – as in the original – are only visible in the F2 views.

### Autopilot (0x2db9b)

- AUTO LEVELING (L): pitch and roll command proportional to the angle (11.4 / 128 per degree), roll
  at most half deflection; if the aircraft is nearly level and the terrain under the next 30 s of the route is +400 wu
  higher, it climbs (0x2d8ab).
- AUTO PILOT (A): thrust 80 %, it turns toward the waypoint (desired roll = heading error x 16, at most
  49°, plus a direct heading turn of at most 0.18° per step), and holds the waypoint's altitude (or the
  terrain-safe altitude): desired pitch = altitude difference x 0.176°/wu, at most 67.5°.
- A stick movement beyond half deflection on either axis, touchdown or (for AUTO PILOT) changing the
  thrust switches it off. Caption: "AUTO LEVELING" / "AUTO PILOT" (20, 220), red.

### G effect (not from the original: a convenience addition)
- Based on the G load shown on the HUD (`gDisp`): above 5 g tunnel vision builds up, with persistently high load
  the image darkens; it fades quickly when the load ends. Below −0.5 g red vision (redout).
  There is no effect on the ground and while paused. For checking, it can be forced with the `F22_GTEST=<0..1>` (negative = red) environment
  variable and the `--viewtest` switch.

### ILS (not from the original: an addition)
- With the landing gear down, in the air, within 12 nautical miles of the nearest runway, if the aircraft is at most 35° off the runway
  axis, the localizer (vertical) and glide slope
  (horizontal) indicators appear in the middle of the HUD, with a 1.5° scale, and the "ILS tt.t" distance (nautical miles). The glide angle is 3°;
  the indicator moves to where the runway is (left = turn left, down = the aircraft is above it). It uses the end of the runway
  that the aircraft is facing. In the original "ILS" is only an MFD page name (0x320f9); its
  calculation is not there, this is an ordinary ILS. Verification: `--ilstest` (`viewtest.txt`, `ils_1.bmp`).

### Voice messages (OCV)
- The original EXE plays a fixed-name sound in three places (0x239f8): when the throttle is raised from zero
  `OCV032.WAV` (0x2749d), when it is pulled down to zero `OCV033.WAV` (0x274b7), when the landing gear retracts automatically
  `OCV047.WAV` (0x2db64). All three are implemented. The missions' radio messages (TWR, AWC, WNG… files) play
  from the events of the mission file, the replies to the wingman commands (WNG, 0x11..0x15) to the commands.
- I did not find the call sites of the other OCV files (`OCV030`, `OCV031`, `OCV040`) in the code; the sound
  identifier of `OCV040` (5) exists, but no playing code refers to it directly.

### Medals (0x160a0 screen, 0xfe3d allocation)
- In the original every medal is awarded for completing a campaign: AIR MEDAL for the 1st, FLYING CROSS for the 2nd,
  SILVER STAR for the 3rd, AIR FORCE CROSS for the 4th, PRESIDENT UNIT CITATION for the 5th campaign (names: 0x15f40,
  caption: "FOR COMPLETION OF CAMPAIGN %d"). After the success of the campaign's last mission 0xfe3d advances
  the pilot to the next campaign and signals the medal; the screen shows as many medals as there are
  completed campaigns (+1 after the last one, 0x1888b).
- Medals are per pilot (according to the pilot's campaign). After completing a campaign the debriefing's
  CONTINUE button opens the medal screen; otherwise the MEDALS button of the STATS / DEBRIEF screen.
- Medal screen (0x160a0): `MED_TEST.PCX` background, the medals from the `MSTRIP.PCX` strip (0x16213; source y 0, 191,
  381, 571, 761; their position x = 118 / 224 / 331 / 437, y = 103, 86x184, the ribbon 277, 300, 86x30). The name of the medal
  under the mouse (y = 340) and "FOR COMPLETION OF CAMPAIGN n" (y = 358) centred. Verification: `--menutest`
  (`menu_medals.bmp`: the pilot in the 3rd campaign, the mouse on the 2nd medal).

### Create Mission (quick mission screen, CREATE MISSION button)
- The original's two screens (`ENEMY.PCX`, `FRIENDLY.PCX`) are the background; the positions of the fields were measured from the image, the captions
  come from the EXE's tables (0x13062.., 0x14147..). **Enemy:** four flights (NONE / EF2000 / MIG-27 / MIG-29 / SU-27,
  skill NOVICE..ACE, count 1–8), ground target (NONE / CHEMICAL PLANT / NUCLEAR POWER PLANT), SAM and
  anti-aircraft gun (skill and count; clicking the field steps through the values). **Friendly:** two allied flights
  (NONE / F-14 / F-15 / F-16, skill, 1–4 units), terrain (ISLAND, SNOW, DESERT, JUNGLE), time of day
  (DAY, NIGHT, DAWN, STORM, SUNSET) and the weapon load (AIM-9X 0–5, AMRAAM 0–5, JDAM 0–3: clicking the
  n-th box makes n the count). Buttons: EXIT, FRIENDLY / ENEMY, START MISSION (Esc: back).
- I did not reverse engineer the code of the original mission builder (the processing of the QMC01..04.ORF templates); this
  part is **our own implementation**. It uses the QMC0x template (a multiplayer map)'s base and the player, copies the enemy from the units of the
  multiplayer samples (`pool_`) (the air unit on route 5 / 15 of the template, the
  ally on route 7, starting in the air), the SAM, the anti-aircraft defence and the ground target (the "plant" is a group of four of the
  template's GSP_11H / GSP_08 objects: there is no plant model). Every enemy unit and
  ground target is a primary goal; the mission is successful when all are destroyed. Without a goal (NONE everywhere)
  START does not start. The first allied flight is the player's wingman.
- Verification: `--createtest [QMC01..QMC04]` (`viewtest.txt`: unit list, goals; `create_1.bmp`), and
  `--menutest` (`menu_create_e.bmp`, `menu_create_f.bmp`, mouse trial in `menutest.txt`).

### Sun (addition)
- The edge of the sun's disc (SUN01.PCX) is faded radially, and the disc has a radius of 3° (day), 2.5° (dawn) and
  3.5° (sunset), so that it has no hard, grey rim. This is not the original look; the position of the sun
  is still an approximation (see under time of day).

### Replaceable textures (`textures` folder next to the program)
- An addition, not in the original. Dumping the original textures: `f22native.exe --dumptex` (or "Export textures" in the launcher): into the
  `textures` folder go the textures of every model (`<NAME>.png`, e.g. `F2201.png`, ~370 files) and the tiles of the four terrain sets
  (`terrain_T<terrain>_<000..127>.png`, 128x128). It does not overwrite an existing file.
- Replacing: edit (or replace) the file with any image editor, keeping the same name; at startup the program loads from the `textures` folder
  whatever is there instead of the original. A deleted file = the original texture. Format: PNG (8-bit grey / RGB / RGBA / palette, not interlaced) or
  BMP (24 / 32 bit). Any size (for models it need not be a power of two, the magnification is filtered, with mipmaps).
- Models: cut-out (transparent) textures are dumped with an alpha channel; for an image without alpha the original transparent areas stay transparent.
  The brightness maps (grey modulation textures) cannot be replaced.
- Terrain: the tiles are square, of any size (we consider them up to 512x512); if several sizes are mixed, all
  grow to the largest. The order / rotation of the tiles depends on the map (the same tile occurs several times), so editing a tile shows up everywhere.
- The HD switch does not scale the replaced textures further (they appear at their own resolution), but it does scale the untouched originals.

### HD textures (OPTIONS 2, the row under the left-hand switch: HD / OLD; `--hd` on the command line)
- An addition, not in the original. On: the palettised textures (terrain tiles, aircraft and building models) are scaled to 2x resolution
  when loaded: Catmull-Rom (bicubic) + mild sharpening; on cut-out (transparent) textures the edge-preserving EPX / Scale2x; the magnification is
  filtered (in the old state pixelated on the models). Off (OLD): the original textures, with the original filtering. When switching, the loaded textures
  are re-uploaded immediately (the switch may take a few seconds), the setting stays in the save. The caption of the row is part of the background image
  (the left-hand switch is in the original "THROTTLE" row, this one is in the next row).
- It is not artificial-intelligence upscaling: it cannot add new detail, it only smooths pixelation and blocky edges.

### G effect on / off (OPTIONS 2, REDOUTS / BLACKOUTS: ON / OFF)
- The additional G effect (tunnel vision, blackout, red vision) can be switched in the REDOUTS / BLACKOUTS row of the OPTIONS 2 screen
  (ON / OFF); the setting goes into the profile (`opt` line, seventh number). When off, the vision does not darken.

### Looking into the sun (original code)
- The sun's model (SUN.PAK) is loaded (0x23604), its handler writes to address 0x37a04, but I found no read of it.
  No text, table or code section in the EXE points to an effect for looking into the sun (glare, lens flare, whiteout). Therefore the original game has no look-into-the-sun effect, and the native version has none either.

### Target selection (0x30974..0x31022, commands 0x283e5 -> 0x27b86)
- Tab / Shift+Tab (0x30edd / 0x30f4d): in the order of the unit list, the first unit after / before the current target
  that matches the weapon's type mask (0x27c16: for an air weapon +0x45 = 1 airborne, for JDAM 2 on the ground),
  is in the 30.9° cone in front of the nose (0x2c in the 0x56840 sine table) and within 15200 wu, and is not blocked by the terrain
  (0x30c05: 64 wu steps). It does not look sideways.
- ' (0x30fba): the unit nearest the crosshair in the 12.7° (0x12) cone, likewise.
- Enter (0x30d2e): shoot list – at most 4 enemy targets by distance (0x27c1b: air unit for an air weapon,
  mission target for JDAM: 0x245c5, the subject of the target trigger); [ / ] (0x30dfa / 0x30dcc) cycle through it.

### Ejection (Ctrl+J, 0xe310)
- Only in the air, once. EJECT.WAV, the pilot (PAR_H00A, after 2 s PAR_H00B) leaves with the aircraft's speed and
  a 0.5 wu/step kick in the aircraft's "up" direction; in the external view the camera follows him; the aircraft becomes a crashing wreck
  (0x2bbc2), with 2–33 pieces of debris (0x317b4). APPROXIMATION: the time of the parachute opening and the descent rate
  (about 7 m/s) are estimated.

### Mission goals (M, [0x531cb] -> 0x2431e)
- During flight at the top of the screen: title (MISSION GOALS / MISSION FAILED / MISSION COMPLETED), then PRIMARY /
  SECONDARY / BONUS GOALS and IMMEDIATE MISSION FAILURES; per row "FLIGHT GROUP <name>" / "GROUND TARGET GROUP
  <name>" / "F22 GROUP STORM" / unit name + condition (" MUST BE DESTROYED" …), when completed " - COMPLETED -",
  for failures " IS DESTROYED", when occurred " - OCCURED -" (the original's texts). APPROXIMATION: for a unit
  goal the model's name is shown (the original 0x305d4 appends a unique name), some of the colours are estimated.

### Score, rank and pilot statistics (0x105c0, 0xfeb9, 0xff1a, 0x15a34)
- Mission score: air kill 250, ground kill 2 x 100 (the original adds it twice), missile hit 150,
  bullet hit 25, successful landing 1000, completed BONUS goal 1000, condition 500 · hp / maxhp, in a campaign
  +1000. The debriefing lists it.
- After every mission to the career: landing, ejection, crash (once), service time in minutes.
  In a campaign only: score, air / ground kills, bullets and missiles fired / hit, JDAM released /
  targets destroyed (according to the original, a quick mission does not count into these).
- Rank from the score: below 15000 / 30000 / 55000 / 100000 / 160000 1ST LIEUTENANT, CAPTAIN, MAJOR,
  LIEUTENANT COLONEL, COLONEL, above that BRIGADIER GENERAL.
- STATS screen: the original's rows and positions (CALLSIGN, SCORE, RANK, CAMPAIGN / MISSION, KILLS, CRASHES,
  EJECTIONS, LANDINGS, SERVICE HOURS, BULLETS, MISSILES, JDAMS). The callsign is fixed here ("STORM 1").
  Profile: `career` line.

### Extra enemies after good performance ([0x22c4d], 0xfd30)
- If a campaign mission's score reaches its threshold (0xfcbc: e.g. campaign 1 3700, 5600, 6000, 4000, 7200,
  4500, 5900, 8000), in the next mission the enemy aircraft flagged with +0x20 = 1 in the ORF also appear
  (1–9 aircraft per mission). The flag only applies to the next load. Earlier the native code regarded these as
  multiplayer units and skipped them.

### Pilots: NEW / CONTINUE CAMPAIGN (0x187a0, 0x188d0, 0x18aa8, 0x1891a)
- NEW CAMPAIGN: 7 pilot slots, the callsign can be typed into the selected row (at most 15 characters, Backspace
  deletes); ACCEPT does not start with an empty name. The new pilot starts with mission 1 of campaign 1 (the record zeroed).
- CONTINUE CAMPAIGN: the pilots' callsigns; clicking continues from where the pilot is. The campaigns
  come in order: after the last mission of a campaign the 1st mission of the next campaign (0xfe3d), after the 5th "all done".
- The career (score, statistics) and the medals are stored per pilot; a quick mission counts for the most recently
  selected pilot ([0x188b2]). Profile: `pilot` / `pcareer` / `cur` lines; the old slots
  become pilots named "PILOT n". Verification: `--menutest` (`menu_newcamp_name.bmp`, `menutest.txt`).

### Combat music (0xeb53, every 64 timer ticks)
- Average = (3·average + shots) / 4, where a missile fired at the player counts 4, a bullet 1, and likewise for a friendly (side 8) aircraft
  (0x399a1, 0x3a1b2). Counters: ground units targeting the player (+ the player's ground target within 15 nautical miles),
  air units and missiles targeting the player, air combat anywhere. Intensity = average/64 + (those targeting
  the player + 2)/4 + (air combat ? 1 : 0). With a ground threat ACT5INTR → ACT5LOOP; otherwise by intensity
  SOARLOOP / ACT3INTR → ACT3_LP1 / ACT3_LP2 / ACT3_LP3; 60 calls at intensity → ACT1, 60 calls at calm →
  SOARLOW (0x7c43f playback, 0x7c46c queuing; songs: the slots of 0x7b8f0).

### Triggers, waypoint, sun
- "Detected" (condition 3): the original sends it on a hit (0x26478, 0x4105b). Global (kind 3) events:
  0 = mission start (0x54e3a), 1 = the player reached a waypoint with action 9 and parameter 1 (0x2b842).
- Waypoint: reached within 152 wu horizontally (0x2b7c9); with action 6 an "arrived" event, then the next
  waypoint, cyclically (0x30ce0).
- Lock (0x2a79b): without a target there is no lock, it does not look for a new target.
- The direction of the sun / moon and the light direction (also for the shadow) in every time of day (1, 1, 1): 45°, 35.26° high
  (0x37db0, 0x3f6b0).
- On ejection the aircraft becomes a crashing wreck (0x2bbc2), with 2–33 pieces of debris (0x317b4); the impact is not a crash.
- The unit row of the mission goals: model + group name + serial number (0x305d4; the source of the serial number is estimated).

### Videos (KDV, from the CD's `NOVA/RESOURCE.RES` file)
- Instead of the installed `RESOURCE.RES` (33 MB), the one on the CD (409 MB) contains the videos: 22 `.KDV` files, of which
  the 11 high-resolution (H…) ones are needed: `HDOLBY`, `NOVA_640`, `HF22INTR`, `HAIR`, `HFLY`, `HSTAR`, `HAFC`,
  `HPRES`, `HEND`, `HSECRET`, `HCREDITS`. The program looks for them in the `video` folder next to it; if one is missing, the video
  is silently skipped. Extraction: `py tools/f22res.py extract cd/NOVA/RESOURCE.RES -o extracted_cd "*.KDV"`.
- Format (reverse engineered, `tools/f22kdv.py` and `src/kdv.cpp`): `xVDK` header, `pVDK` palette, `iVDK` frame
  (4x4 blocks, 2-bit commands, 0 = transparent colour), `sVDK` / `aVDK` sound (8-bit PCM, 22050 Hz, two
  `SAMP` = left / right, or one = mono). Timing: 120 Hz / divisor (8 = 15, 10 = 12 frames/s). The player computes the frame from the position
  of the sound. `py tools/f22kdv.py mp4 file.kdv out.mp4` converts it into an ordinary video (ffmpeg).
- When they play (according to the original's routines): at startup Dolby, Nova, the F-22 intro (0x8a954; any key or
  click skips it, `--nointro` omits it); on the main menu's CREDITS button `HCREDITS` (0x12628); after the success of a campaign's last
  mission the campaign's medal video (1st–5th: `HAIR`, `HFLY`, `HSTAR`, `HAFC`, `HPRES`), after the 4th
  also `HEND`, after the 5th `HSECRET` (0x53c4c / 0x53bfa), then the medal screen.
- The 5th (secret) campaign only opens above a total score of 175000, otherwise after the 4th the pilot is "done" (0xfe67).
- Verification: `--videotest FILE.KDV` (playback timing + `video_N.bmp`), `--menutest` (`menutest.txt`: the end of the campaigns
  and the video order).

### Refinements (from the original code)
- Lock tone (0x3310a): TONE2 on channel 1, pitch 0x1c0000 = 28 x the natural one (the sample's 192 Hz base
  becomes about 5.4 kHz, a high tone similar to BEEP).
- Cannon pipper with a target (0x31bc0): the bullet travels 4 wu per step; the flight time is searched starting from t0 = distance / 4 and decreasing to
  12 steps, where the distance / 4 of the target's position t steps later is closest to t;
  the pipper is the intercept point computed with the target's motion (the original does not account for the shooter's own motion).
- Wingman "cover" (Ctrl+C, 0x30825 / 0x37147): the nearest unit targeting the leader (measured from the leader).

## From the original rules (a selection)

- Unit: 1 world unit = 16 feet; a fixed 40 Hz simulation step; the world wraps around.
- Takeoff: about 190–210 knots; landing only near an airfield, with the gear down,
  at most 385 knots, with a 0–33.75° pitch.
- Missiles: pure pursuit, they steer after reaching terminal speed; above 45° they lose
  the target for good; damage with a random 50–100 % multiplier; the player starts with 512 hit points.
- SAM: the radar searches every 0.8 s within 3800 wu (it sees the player only at
  ~40–100 % because of stealth), it fires every 120 s per launcher.

- Sideslip (0x2cdb0): from the beta angle a lateral force perpendicular to the velocity, plus additional
  drag – with the original's integer arithmetic.
- Evasion (0x258b1): the 8 original bytecode manoeuvres (hard turn, Immelmann, roll-off turn,
  vertical scissors, split-S, loop, zigzag, turn + Immelmann); the choice is made from a bitmask depending on the aircraft's
  type, skill and altitude above the terrain. There is no terrain following during
  a manoeuvre (as in the original), so a low aircraft can even fly into the ground.
- Formation (0x25e4b): the original offset table of 8 formations (0x255fc), rotated to the leader's position;
  speed matching within a 10 wu box.
- Lock cursor (0x2a886): every 2 steps it moves toward the target by z·sin(0.275°) in body coordinates.

## Terrain and ground collision
- The terrain height is a 512x512 grid (128 wu cells, value x 4 wu). Collision in the original's two steps: a pre-filter with the maximum of the
  cell's four corners (0x5271a), the contact with the height interpolated on the two triangles along the cell's (x, z)–(x+1, z+1) diagonal (0x52796) – the player's
  touchdown (0x2b555), the missiles (0x39de5), the bullets (0x4101c) and the AI aircraft (0x269d6) all work this way. The drawn terrain mesh shows exactly this
  surface (the exact height at every grid point, with the same diagonal), so the ground seen and the collision agree.

## Known approximations

- AI: by default the native transcription of the bytecode scripts (a hand-written state machine) runs. The original scripts can be experimentally enabled with the full instruction set,
  group handling, wingman commands and trigger events: `f22native --aivm` (description: `AI_VM.md`).
  In the native AI the control effectiveness is limited to at least 0.15 for the turn rate of manoeuvres.
- The player's wingman waits for the player to take off and starts from the point of the taxi route nearest to it (the missions often
  put the wingman already on the runway, while the route starts from the parking area); I did not find the original's way of starting.
- Triggers: with the native AI every step we look at the state (dead / airborne / arrived / hit); under `--aivm`
  the original event dispatcher (0x23cc0) also works for the VM's events.
- HUD: the cannon pipper without a target (0x31b2a: 47 steps ahead along the aircraft's [+0x83] vector; the meaning of the vector is unverified) is still computed in a simplified way from the angular velocity; with a target the original algorithm (see above).
- Menu: the starting height of the weapon list is estimated.
- Cockpit displays (0x1fb40): in the cockpit view (F1, F2) the artificial horizon, defensive, navigation, attack and
  weapons pages are rendered live onto the cockpit model's F22TOPLT / F22SCRS / F22SCRN / F22SCRLT / F22SCRBT texture
  (128x128, the page's rectangle onto the frame's inner rectangle), without text, as in the original (the text is only in the
  full-screen view). The HUD repeater (F22TOPS, the top-left display) stays dark in the original
  (0x1fb40 only draws displays 2..6); here, as an addition, it shows the message log in SMALL320
  letters, with line breaks, the most recent lines (the last message green, the earlier ones yellow).
- MFD pages (0x1cba0): the frame and the rectangles (0x1cbc0, 0x1cac0, 0x1ca60), the elements and
  captions of the pages at their original positions; the shape of the symbols, the map scale and the compass radius are estimated (the 0x1dbb9
  drawing routine was only roughly reverse engineered).
- Views: the external camera's orbit angles are tied to the aircraft's heading in a simplified way (the details of the original
  0x29c5b matrix chain are not clear); the padlock smoothing is estimated.
- Wingman: in the missions the F-22 (`wing` = 1) in the player's group (7) is the wingman; because of the player's
  own side code (2) it used to fly without a leader, now it comes in the player's formation. The commands follow the meaning of the
  original's scripts (0x3796c, 0x37998, 0x379ac, 0x379c4, 0x379d0) with the states of the native AI
  (not with a bytecode interpreter); the target selection of "cover" (0x30825) is approximate.
- Joystick (winmm): it uses the first detected device, with an 8 % dead zone; the button layout is our own (the original `JOYPORT.DRV` driver is DOS-based, its calibration menu is not there). I have not tried it with a real joystick. Test: `f22native.exe --joytest` (joytest.txt: detection and a 5 s sampling).
- Game controller: XInput (loaded at run time from the system DLL), the first connected device; old DirectInput controllers (e.g. PlayStation) show up on the winmm joystick path if Windows handles them. Not tried by hand.
- Sound: the position of the countermeasure release sound (FLARE.WAV) is estimated;
  the call sites of the OCV030 / OCV031 / OCV040 sounds are unknown, these do not play.
- Time of day: the size of the sun / moon disc is estimated (the original SUN.PAK quad is scaled by the drawing routine); the size of the cloud texture
  (16 wu / texel) is estimated; the own PAL palettes of regions 1–3 do not match the tiles by index,
  so there too we use the mapping fitted from region 4.
- Self-test with sound: `build\f22native.exe --soundtest` (soundtest.wav + soundtest.txt).
