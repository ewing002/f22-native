# The original AI bytecode interpreter (F22.EXE)

The behaviour of every AI aircraft in the original game is driven by **bytecode scripts** (data in the EXE, 0x374c0–0x37a40), which a small interpreter runs (0x36d17). So far the native simulation has approximated this with a hand-written state machine (`sim.cpp`, S_PARKED … S_PULLUP).
For exact behaviour the scripts and the helper routines that serve them have to be ported. Status: **stages 1 and 2 done**, **stage 3 in progress** (VM, tools, unit record, Host; helper routines partly done).

## The VM (done: `native/src/aivm.cpp`, `tools/f22script.py`)
- Environment (ebp + 0xa7): r0..r3 (32 bit), ret (+0x10), flags (+0x14), PC (+0x18). `ebp+0xeb` requests a new entry point (it is loaded into the PC in the next step).
- Instruction: 4 bytes (op, a, b, c). Handler return value: carry = continue in the same step, no carry = stop (END).
- Flags: instructions that use helper routines (conditions) put the helper routine's carry into **bit 1** (1 = "done"); arithmetic uses the x86 ZF / SF / CF flags.
  `JCLR a` jumps if `flags & a == 0`, `JSET a` if `!= 0`; `JGE` is SF==0 (the original compares 0x400 with SF<<3).
- Own instructions (in the VM): END 00, JMP 01, CALL 02, RET 03, FLAGS 04, JCLR 05, JSET 06, LD0 07, LDR 08, DEC 12, ADD 16, SHL 17, CMP 1c, JGE 1d.
- Verification: `f22native --aivmtest` (the climb script 0x37550 with truncated helper routines; `aivmtest.txt`).

## Instructions that touch the aircraft state (these will live in the Host) and their helper routines
| op | name | address | size (instructions, up to the 1st ret) | direct calls |
|---|---|---|---|---|
| 0x0b | C_25b31 | 0x25b31 | 34 | 0x257e9, 0x2583e, 0x25a01, 0x25a36, 0x27166 |
| 0x0c | C_25cde | 0x25cde | 13 | 0x30780, 0x3a32d |
| 0x0f | C_25dab | 0x25dab | 10 | 0x257e9, 0x2583e, 0x25fa4 |
| 0x10 | SPD→0x2583e | 0x2583e | 30 | 0x5271a |
| 0x11 | C_25d48 | 0x25d48 | 34 | 0x40c75 |
| 0x14 | C_26142 | 0x26142 | 111 | 0x257e9, 0x260fe, 0x2611d, 0x40c75 |
| 0x18 | C_26028 | 0x26028 | 10 | 0x257e9, 0x2583e, 0x25fa4 |
| 0x19 | A_25e4b | 0x25e4b | 11 | 0x257e9, 0x2583e |
| 0x1f | C_25b9f | 0x25b9f | 13 | 0x25a0e, 0x25be6 |
| 0x21 | C_25be6 | 0x25be6 | 45 | 0x3a8f0, 0x40c75 |
| 0x23 | C_25c5a | 0x25c5a | 24 | 0x40c75 |
| 0x26 | A_26d32 | 0x26d32 | 33 | 0x23cc0, 0x26cb3, 0x308f9, 0x39b95 |
| 0x27 | A_26d9a | 0x26d9a | 64 | 0x30683, 0x306a3, 0x30780, 0x30aec, 0x3a345, 0x575e6, 0x5777d, 0x5781c, 0x5785e, 0x578a0 |
| 0x0e | TGT→0x308f9 | 0x308f9 | 46 | 0x30780, 0x30c05 |

Simple instructions that work from memory / registers (Host): STF 0a (field ← r), LDF 15 (r ← field), WPSET 0d (waypoint), RND 13, RADIO 1b, AGL 1e
(height above ground), LEADER 20, TKOFF 22, CLS 24 / ACT 1a (per-class operations, tables 0x36bdc / 0x36ad0), FORM 25, STATE 28 (state = [ebp+0xf3]),
VMAX 29 / VTGT 2a (speeds from the performance table: 0x36c02 / 0x36c22).

## Inventory
- AI-specific routines: 54 in total, about 1800+ instructions altogether (up to the first `ret`; the real size is larger), shared math / engine routines: ~10.
- The helper routines use the fields of the original unit record (+0x24/+0x28/+0x2c angles, +0x7f speed, +0x8f target, +0xd3 / +0xcb / +0xcf angle command and
  turn limit, +0x103 target speed, +0xff performance record, +0xdf leader, +0xf3 state, +0xf7 …). The native `Unit` has a different structure, so to
  move forward the unit record has to be mapped onto the original one.

## Stage 2 (done): the unit record and the Host (`native/src/aiunit.h/.cpp`)
- `OUnit`: a byte-exact mirror of the original record (0x140 bytes), with the names of the ~60 offsets used by the AI in the `of::` namespace (the meanings come from the original code and from the native
  simulation; unverified ones are marked). The VM environment lives in the record (r0..r3 = 0xa7.., ret = 0xb7, flags = 0xbb, PC = 0xbf, new entry point = 0xeb).
- `OHost`: the instructions built on fields, with the original's precision – STF (signed b << 8 | c offset), LDF, RND (the original generator: `s = rol(rol(s, 11) + s, 3)`,
  0x56682), AGL (height − ground, 0x5271a), WPSET, STATE. The rest (`other`) belongs to the helper routines.
- Ground height (0x5271a): the maximum of the 4 neighbouring sample points, 512x512 grid, x / z >> 23.
- Verification: `--aivmtest` – the climb script with the original record: the commanded pitch, the state and the loop exiting at the threshold are all correct.

## Stage 3 (in progress): ported helper routines (`OHost`, `aiunit.cpp`)
All disassembled from the EXE, with the original's precision (integer arithmetic, tables taken from the EXE image):
- Math: `sinCos` (0x575e6, table 0x56840, linear interpolation), `atan2o` (0x3a8f0, table 0x3aa7c), `rebuildDir` (0x40c75 → 0x575e6/0x5777d/0x5785e/0x578a0/0x5781c:
  rotating the (0,0,1) vector by the angles C, B, A, fistp = rounding), the movement direction in the +0x73 / 0x77 / 0x7b fields.
- Instructions (the helper routine's carry goes into bit 1 of the flags, `fin`): SPD 10 (0x36f9c: speed toward +0x103 with the acceleration from +0x20 of the performance record, done when it is reached,
  or when the ground avoidance starts), 11 (0x25d48: pitch / roll to zero), 1f (0x25b9f: taxiing to the waypoint, braking near 0x40000), 21 (0x25be6: turning
  toward the waypoint, ±0x60c0 per step), 23 (0x25c5a: turning to the commanded heading, ±0x3060).
- Ground avoidance 0x2583e: every fourth step, look ahead by 0x17c; if the ground + 0xa0000 is not lower → state 7, entry point 0x37550, the previous state is stored in +0xfa.
- Verification: `--aivmtest` (sin/cos of 45° = 46340 / 46341, atan2 for the four axis directions, turning toward the ground waypoint in 92 steps with the direction vector,
  levelOut to zero in 24 steps).
- Distance approximation 0x52aaa (`dist3`: max + (other two) * 0x5000 / 65536) and the target-speed tracking 0x25780 (`matchSpeed`); `--aivmtest` checks them.
- Unit list (`OWorld`): the original records, pointers are "addresses" (BASE + index * 0x140), the chain is in the +0x34 field; `coverUnit` (0x30825), `dist3to` (0x30780),
  `aimAt` (0x25a36 + 0x260fe / 0x2611d / 0x25ae9: heading and pitch error with the 0x2c4d0 turn-factor table, braking, roll follows the turn).
  Verification: `--aivmtest` (the nearest unit aiming at me; the aiming converges to zero error in 400 steps, the vector ratio is 3:8).
- Firing and defence: `fireCheck` (0x257e9: fires every 128 steps – the shot is emitted through the `spawnShot` callback, in place of 0x3a5da / 0x3a722 – then the aircraft evades), `provoke` (0x258b1:
  the defender goes to state 6; the evasion script is chosen by the word at performance record +0x38 + skill * 2 (+8 / +0x10 depending on altitude) and the original RNG from the 0x36c32 table),
  `near3` (0x259d4). Verification: `--aivmtest` (fires, state 6, entry point 0x377d4).
- Target-search family (`findTarget`: 0x30871 / 0x308f9 / 0x30974 / 0x309fe): the nearest unit with the side and type masks, within weapon range; flags: TS_MIN (farther than the previous one), TS_STEALTH (for a hidden target, range * +0xa3), TS_LOS (line of sight 0x30c05 – the `los` callback), TS_ME (the target is targeting me). `assignWing` (0x27166): the leader
  assigns its wingmen ever more distant targets, with script 0x375a8. Verification: `--aivmtest`.
- Combat steps (op 0b / 18): `patrolStep` (0x25b31: ground avoidance, firing, wingman assignment, waypoint done within 0xc80000, aiming at the waypoint, accelerating to minimum speed),
  `attackStep` (0x26028: aiming at the target, speed matching, done when the distance is <= +0x10f and the errors are <= 0x200000 = 22.5°; the target goes into defence),
  `groundAttack` (0x25fa4: ground target, aiming above the ground + 0x57e0000), `leaderFar` (0x25c9e). Verification: `--aivmtest` (the attack is done in 2 steps within the 22.5° threshold).
- `maneuver` (op 14, 0x26142): turns toward the commanded pitch (+0xd7) / heading (+0xd3) / roll (+0xdb) with the turn factor and braking, done when all are reached exactly
  (0x80000000 = no command); `formationStep` (op 19, 0x25e4b): aiming at the position beside the leader (the slot record's vector rotated by the leader's heading), within 0xa0000 the roll goes
  to zero and the speed matches the leader's, outside that the speed depends on the distance from the leader (leader − 0x500 + distance >> 10); for a ground leader, script 0x37970.
  Verification: `--aivmtest` (the turn is done exactly in 5 steps; the formation flyer settles on the leader's speed).
- `fireAtTarget` (op 0c, 0x25cde: cannon at close range <= 0x17c0000, missile farther out, bomb for a ground type – the shot goes to the `weapon` callback), `pursuitStep` (op 0f, 0x25dab:
  pursuit; when the op is not in the done state it stops the step, like the 0x36f76 handler), `notifyAttacked` (0x270a6 / 0x270ed: the first state-4 wingman of the attacked group
  goes after the attacker; the leader table is the `groupLeader` callback). Note: after 0x25c9e (far from the leader) the original immediately overwrites the 0x3759c entry point with 0x376b0 – it is ported this way.
- Simple instructions: TGT 0e (0x36f26: target search if it has no target and may search), LEADER 20 (0x37259: ground waypoint record: for type 1 the 0x374e8 script and the commanded heading,
  for other types the wingman takes off), TKOFF 22 (0x372e8: accelerating to takeoff speed, event + takeoff callback, state 2), FORM 25 (0x37395: into formation or into attack),
  VMAX 29 / VTGT 2a (speeds from the performance record and from the 0x36c02 / 0x36c22 skill tables). Verification: `--aivmtest` (takeoff in 30 steps, VMAX).
- ACT 1a (`actOp`, 0x37154: 0 target search, 1 the leader's target, 3 the nearest unit attacking the leader), CLS 24 (`classOp`, 0x3737c: depending on the waypoint's class 0 next waypoint, 1 jump,
  2 target assignment with range 0x2c880000, 3 named target, 6 events; 4 and 5 – 0x2728c, 0x27366 – still to do). Verification: `--aivmtest`.
- Line of sight `visible` (0x30c05: the ray with step spacing distance / 64, in distance >> 22 steps; it stays below the maximum of the four neighbours, or the interpolated ground (0x52796, `groundI`) does not reach it;
  with a step count of zero the original would repeat 2^32 times, here it is limited to 4096; the target search uses this when there is no `los` callback) and the movement `move` (0x40cfa: position += direction * speed >> 16).
  Verification: `--aivmtest` (flat terrain: visible; a mountain between the two aircraft: blocked).
- Note: the remaining combat helper routines (0x25a36 aiming, 0x258b1 firing, 0x257e9, 0x270a6, 0x3a5da / 0x3a722, 0x308f9) also use the unit pointer list
  (0x304b8, +0x34 chaining) and the other units' records, so these first require the world model (a unit list with the original records).
- The instruction set is complete: CLS 4 (`assignFromLeader`, 0x2728c: target assignment up to 0x16440000, with ever more distant targets), CLS 5 (0x27366: a one-time radio message from the mission's
  radio table – the class is then zeroed), A_26d32 (`radarStep`, op 26: every 32 steps a target search up to 3800 wu with concealment and line of sight, 1 wu above the radar;
  "detected" event (1, 4); from the radar's chain (+0x97, the 0x26824 attachment: launchers within 1900 wu) the launcher that faces the target the most, if it has not fired for 0x12c0 steps –
  `pickLauncher` 0x26cb3 – launches a SAM), A_26d9a (`aaaStep`, op 27: every 32 steps the nearest air target within 1140 wu, lead (distance / 0x30000) * speed in the target's
  direction, turning by at most 45°, burst 0x3a345).
- The ground air defence also runs on the VM (`--aivm`): the anti-aircraft gun (type 0) and the radar (type 2) get the script from the 0x36ba0 role table (0x37528 / 0x3753c), the native
  `stepGroundUnits` does not run on them; the launchers (1) are passive, the radar launches them.
- Fixed: the concealment factor +0xa3 of the records (0.4–1.0 for the player, 0.4 for the side-2 wingmen) used to be 0, so the VM never saw aircraft with side flag 0x22 (the player included).
- Verification: `--autotest --aivm` – 28 events (radar detections), 5 enemy shots; the native AI is more aggressive in the same test (11 enemy, 4 friendly shots,
  3 aircraft shot down) – not comparable with the original, because the original game cannot be run for comparison.

## End to end: `--aiflight`
One aircraft flying exclusively with the original scripts (VM + ported helper routines + `move`): the 0x374c8 entry point (taxiing along the ground waypoints: 0x25be6 / 0x25b9f / LEADER), 0x374e8
(turning to heading, takeoff TKOFF, turn), 0x37510 (FORM + patrol + CLS) – around the rectangle of waypoints. The performance record comes from the EXE's 0x24618 table (type 0), the ground is flat.
Result (`aiflight.txt`): state 1 (taxiing) takes off after 600 wu (3 → state 2, the aircraft climbs at ~740 wu), it flies through the 4 waypoints (course 86° / 177°, altitude ~400 wu),
and the waypoint pointer advances. So the aircraft flies with the original scripts, without the native state machine.

## Group handling (`OGroups`, `aiunit.cpp`)
The original structure: 36 groups (0x24), one leader per group (0x24940 table) and at most 10 wingmen (0x249d0 + group * 0x28 + slot * 4); from the leader's +0x97 field the
wingmen form a chain (+0x97), each one's +0xdf field is the leader; the wingman's +0xe3 field is the formation slot record (0x14 bytes: vector, +0xc heading offset, +0x10 pitch offset), +0x93 is the death handler.
Ported: `addLeader` (0x2385a), `addWing` (0x267d7), `buildChains` (0x268e2), `leaderDied` (0x2668f: the next wingman becomes the leader, the waypoint pointer passes over,
the leader of the remaining ones is updated; in state 4 0x37510, from taxiing 0x374c8), `wingDied` (0x26714: unchaining, the one behind the taxiing aircraft inherits the taxiing), `groundedGone` (0x2691d: the counter of
those on the ground, an event at zero), `join` (0x26824: nearby joiners go to the head of the chain), `activateGroup` / `activateUnit` (0x23ab7 / 0x23ae7: the taxi script, by role from the 0x36ba0 table),
`promote` (0x26650). Verification: `--aivmtest` (chain, unchaining, leader death).
Still to do for hooking it into the game: the default values for creating an aircraft (0x23770 / 0x2af07: zeroing, pointers, initial state from the ORF record) and the mapping of the native Unit.

## Hooking into the game (experimental): `f22native --aivm` (`aibridge.h / .cpp`)
Every unit has one original record (`OUnit`) (position in 16.16 wu, angles 2^25 = 360°, speed in 16.16 wu / step); the airborne AI aircraft (not the player) are driven by the VM
once per step (40 Hz), the native state machine (`stepAI`) does not run on them. In each step: the positions of the non-VM units (player, ground units) go into the record, the VM runs (0x36d41: script, clearing the
INCOMING flag, counter, `move`), then the position, angles, speed, state and target are written back into the native `Unit` (the state is for the HUD / broadcast: 0 parked, 1 taxiing,
2 route, 3 takeoff, 4 formation, 5 attack, 6 evasion, 7 evasion from the ground).
- Initial values (0x26a3a / 0x26c31): from the ORF record the group, role (type), skill, bound / type mask / wingman flag, the turn limits from the performance record and from the skill
  factor (0x36c12), the weapon range (0x39034), the route and the taxi waypoints (the ORF's 16 / 12-byte records directly, at the `ROUTE_BASE` / `TAXI_BASE` addresses), the initial state according to the
  native `restart()` decision (parked / taxiing / route); the groups with `OGroups` (the formation slot from the 0x255fc table).
- Events: a hit (`World::damage`) sends the attacked unit into evasion and sends its group after the attacker (0x26468); a death updates the group's chain; a leader activated by a mission event
  starts taxiing / flying. The VM's shots (cannon, missile, bomb, countermeasures) go into the native weapon system.
- Wingman commands (`AiBridge::wingCommand`, 0x2770d … 0x27780): the first wingman in the player's chain (if it is airborne) gets a new entry point – Ctrl+P 0x3796c (patrol), Ctrl+F 0x37998
  (formation), Ctrl+E 0x379ac (+0xf8 = 1, attack), Ctrl+M 0x379c4 (the player's locked target, if there is one and it is not the wingman itself), Ctrl+C 0x379d0 (cover); the radio message comes from the script
  (RADIO 1b: text 0x36bc8, sound 0x36bf8 – only if the aircraft's leader is the player). Verification: `--wingtest --aivm` – all five commands switch to the expected state and target, the radio text comes from the EXE.
- Fixed: the chain of the records (+0x34) was zeroed by the initial values, so the target search saw only one aircraft; now under `--autotest --aivm` the aircraft also start fighting (attack, evasion, firing).
- Triggers (`World::triggerEvent`, 0x23cc0): the original event dispatcher – (type cl, condition ch, subject bx, parameter dx); the response of a matching trigger that has not yet been fulfilled runs immediately
  (`World::respond`, according to the 0x23910 response table; the target response by the word at +6: 0 → only dx = 0, otherwise dx >= +6 in quarters, the others only for dx = 0). The VM's events:
  takeoff (TKOFF: 1, 1, identifier), the last grounded aircraft of the group has also started (0x2691d: 0, 1, group), waypoint event (CLS 6: 1, 2, identifier and 0, 2, group).
  The native poller `runTriggers` remains for the other conditions (destruction, hit), the two use the same `fired` flag. Verification: `--autotest --aivm`
  (3 events, 1 fulfilled trigger).
- Not yet hooked up: the CLS 4 / 5 class and the two combat actions (0x26d32, 0x26d9a), the INCOMING flags from the missiles (it reads the native `incomingIR / Radar` fields).
- Verification: `--autotest --aivm` and `--wingtest --aivm` run without errors (18 aircraft: parked / taxiing / route / formation; altitude 700–2300 wu above the ground, speed ≤ 960 knots),
  the tests without the switch (autotest, wingtest, menutest, flowtest, soundtest) are unchanged.

## Remaining stages
1. **Done:** VM, disassembler, script list (`extracted/ai_scripts.txt`), verification test.
2. ~~The unit record and the Host~~ (done).
3. Porting the entire NPC simulation onto the original record: the 100 reachable routines (the AI helper routines and their deeper calls, the physics at 0x40cfa), the condition helper routines one by one compared with the original (ground movement / takeoff, route, formation, target tracking), then the combat routines (pursuit, cannon / missile).
4. Replacing the native state machine with the VM, regression tests (autotest, wingtest, formation, evasion).
