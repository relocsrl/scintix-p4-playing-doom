# Jev plays DOOM — work notes (session of 2026-09-21/22)

Handoff notes for whoever picks this up next (human or Claude Code). What was
built, why it is shaped this way, what was verified, and what is still open.
User-facing docs are in [`tools/README.md`](../tools/README.md) §3–4.

## Goal

Let **Jev** (TypeSafe AI's "System One" model, released 2026-09-16: typed,
calibrated decisions in ~100–300 ms instead of prose) play DOOM on the SCINTIX P4
through the firmware's `/agent` WebSocket (on `master` since v0.5.0), in two
configurations:

- **Level 1** — Jev alone, a few decisions per second.
- **Level 2** — Jev for the reflexes + an LLM supervisor (Claude by default) that
  every ~10 s reads the automap/trajectory and sets the strategic intent.

## What was added

| Path | What |
|------|------|
| `tools/doom_jev_agent.py` | The agent. `--mode jev` / `--mode supervised` (`--supervisor anthropic\|openai\|dry`), `--check` preflight, `--realtime`, `--jev dry` (no key), `--log run.jsonl`. Stdlib HTTP only (urllib), needs just `websockets`. |
| `tools/doom_agent_sim.py` | Simulator of the firmware `/agent` API (same JSON): 2 rooms, corridor, door with Doom semantics, imps, barrel, exit → intermission → E1M2, death → respawn, lockstep catch-up like the board, realtime `hold` mode. Scenario flags `--spawn X,Y,HEADING`, `--hp`, `--menu-secs`. |
| `tools/tests/mock_servers.py` | Fake TypeSafe `/v1/systemone` (validates request shape, real response schema, synthetic 429s) + fake LLM (Anthropic Messages, OpenAI Chat). Whole two-level pipeline runs with no keys. |
| `tools/firmware-realtime-agent-mode.patch` | The `{"hold": true}` free-running agent mode as a patch; already applied in `main/` since v0.5.0, kept for reference. |
| `tools/README.md`, `tools/requirements.txt` | Docs for all of the above (no new dependencies). |
| `binaries/scintix-p4-doom-vX.Y.Z/` | Prebuilt images of each release (committed), `flash.bat COMx [full\|app\|erase]` and a `README.txt` with the plain esptool commands. |

## TypeSafe / Jev API (as documented at docs.typesafe.ai, Sept 2026)

- `POST https://api.typesafe.ai/v1/systemone`, `Authorization: Bearer <key>`,
  body `{"model": "jev-latest", "state": <string|object>, "questions": {...}}`.
- Question primitives: `choice` (`instructions`, `criteria` map option→description,
  max 255 options), `noul` (yes/no probability, `criteria.true/false`), `score`
  (2–10 ordered levels).
- Response: `answers.<id>.{choice, probabilities, confidence}` / `.noul`, plus
  `usage.input_tokens`. $0.042 / M input tokens, output free. 64k tokens context;
  1200 req/min.
- Access was early-access/waitlist at release.

## Agent design

- **State (~560 tokens; ~1.5k per call with the questions):** legend, player
  (hp/armor/weapon/ammo/compass heading/level), the 51-ray fan collapsed to 7 sectors
  (min/max) + detected openings, enemies in view (`enemies_in_view`, monsters only) and
  barrels apart (`hazards_in_view`: obstacles, not targets), `door_ahead`, blocked flags, last 8
  macros, a 9×25 automap window, and `commander` (the supervisor's intent with
  `heading_error_deg` / `turn_hint`).
- **Questions per step:** `action` = Choice over 14 macros (forward, forward_long,
  forward_left/right, turn_left/right ~30°, turn_*_small ~9°, turn_around, back,
  strafe_left/right, use, wait); `fire` = Noul. The criteria text of each macro says
  *when* it is the right one — this is the main tuning surface (`MACROS`,
  `build_questions`).
- **Policy:** `mixed` = argmax, sample from Jev's distribution when confidence < 0.35.
- **Safety nets** (deterministic, always flagged in telemetry, off with
  `--no-heuristics`): unstick after 4 blocked moves; anti-oscillation (only with no
  *hostile* in view — barrels do not count); stall (net displacement < 48 units over
  the last 10 decisions with no hostile in view → turn toward open space, then a
  10-decision cooldown; `--stall-steps`, `--stall-radius`; quiet while working a door);
  door-retry / dead-door. **These are `--nets all` now** — see the memory rework below;
  with the default `--nets doom` only the two real mechanics stay: approach-door (USE
  reaches only 64 units); door-wait (USE *toggles* a door — a second press while it
  rises sends it back down — 30-tic cooldown).
- **Barrel bug (fixed 2026-09-23, first real-Jev run on the board):** barrels were
  reported to Jev inside `enemies_in_view`, and the prompt says "turn to centre an
  enemy", so Jev dithered `turn_left_small`/`turn_right_small` in front of a barrel
  forever. The anti-oscillation net stayed off (it required *nothing* visible) and
  unstick never fired (turning in place is never `blocked`). Fix: barrels moved to
  `hazards_in_view` + legend/instructions ("never centre one, go around it"),
  anti-oscillation ignores barrels, and the new stall net. Reproduced and verified on the
  simulator with a scripted dithering policy (spawn `1940,300,270`: barrel only in view):
  old code 0 units moved in 40 steps, new code escapes (anti-osc ×10, stall at 11/21/31).
- **Door back-and-forth (fixed 2026-09-23, same run):** the first `use` did not take
  (pressed at 65 units while still sliding in), Jev then just `wait`ed, the stall net
  saw "going nowhere" and turned the marine away — right as the door opened after a
  later press, and again at every visit. First fix was another override (a stall net
  that stays quiet while a door rises + a USE-retry net); the user then asked whether
  stacking logic on top of Jev is the right shape at all.
- **Memory instead of overrides (2026-09-23, follow-up):** it is not, and it is the
  opposite of how System One is meant to be driven — Jev has no memory between calls,
  so the *caller* owns the context and passes it in the `state`; overriding Jev's
  answer takes the decision away from it. Reworked so the things a single frame can't
  show are **facts in the state**, and Jev decides:
  - `door_ahead.presses` / `tics_since_press` (from `Memory.door_work`): pressed this
    door N times, last one this many tics ago. `< 50` = still rising (wait), `>= 50`
    while still in view = the press didn't take (press again). `door_ahead.wont_open`
    after 3 presses (into `dead_doors`, 2 min). The per-door memory keeps counting
    while the marine looks away (so a locked door revisited over and over still ends
    up dead) and is dropped when the door is gone from view while facing its spot
    (opened), after 10 s, or 256 units off.
  - `progress.moved_last_10_steps` / `going_nowhere` (< 48 units) and `dithering`.
  - The macro/legend/instruction criteria were rewritten to use these; DryJev too.
  - Also fixed the frozen-game detector: a straight move that comes back `blocked`
    (into a wall or a barrel) no longer counts as "nothing changed", so pushing on a
    barrel is no longer mistaken for an end-of-level/menu freeze (`unfreeze` pulses).
  - `--nets` chooses the layer: **`doom`** (default) enforces only the two real Doom
    mechanics (USE reach, USE toggling a rising door); **`all`** restores the old
    overrides (unstick / anti-osc / stall / door-retry / dead-door); **`none`**
    (= `--no-heuristics`) is pure Jev.
  - Simulator, DryJev with `--nets doom` (spawn `1060,320,90`, door1 variants): normal
    door passed at step 7, stubborn (first press ignored) at 14 with 2 presses, locked
    given up after 3 presses (`wont_open` shown to Jev for 14 steps) and never
    revisited. Barrel (spawn `1940,300,270`): pushes once, then `going_nowhere` in the
    state turns it away (net 508 units; no false `unfreeze` any more). Default game
    2095 units in 80 steps, exit→E1M2 and death→respawn both still work. `--nets all`
    unchanged from the previous fix; regressions (barrel dither, default) hold.
- **First real-Jev-on-the-board run, three more "our data, not Jev" fixes (2026-09-23,
  same day):** watching Jev play E1M1 live surfaced three behaviours, all traced to
  what we fed it / how we turned its answer into a move, confirmed from the run.jsonl:
  1. *~360° spins at a wall.* 25 % of decisions were `sampled` (mixed policy draws
     from Jev's distribution when confidence < 0.35). Sampled steps hit a wall 20 %
     of the time vs 8 % on argmax, and the drawn macro was near-uniform noise — a
     random `turn_around` (180°) twice = the spin. Fix: `mixed` no longer samples
     while blocked or `going_nowhere` (takes Jev's top choice there); `--policy
     sample` still always samples. `pick_macro(..., allow_sample=)`.
  2. *Plowing into barrels.* The 51-ray fan is walls only, so `ahead` read the wall
     *behind* a barrel (e.g. barrel at 45 units, `ahead` = 412) and forward looked
     clear. Fix: `obstacle_ahead()` folds a barrel in the walking corridor into
     `view.ahead_min_dist`; legend + forward criteria + DryJev updated.
  3. *Dancing around an enemy instead of shooting.* Aggregate turn sizes are actually
     right (small ≈ 9°, big ≈ 34°), so it is not a fixed-overshoot; it is a latency
     loop — at ~0.7 s Jev latency a correction acts on a stale bearing and swings
     past, then back. When centred Jev fires 100 % and hits; it just kept turning
     afterwards. Fix: `aim` in the state with `aim.on_target` (|bearing| ≤ 8 and dist
     ≤ 700, ammo left) = hold heading (`wait`) and fire, do not turn; criteria for
     the small turns / `wait` / instruction updated; DryJev holds-and-fires when
     centred in range, backs off in melee (< 120), advances when centred but far.
     The distance gate matters: without it the marine froze aiming at an imp 1025
     units away (caught in the sim regression, fixed).
  - The real structural fix for #3 is the realtime firmware patch
    (`tools/firmware-realtime-agent-mode.patch`): no think-pause, continuous turning,
    so the aim loop cannot form. Still not flashed.
  - Simulator after all three: close combat (spawn `1300,330,270`) holds heading and
    kills the zombieman in 2 shots, no dance; doors / barrel / default / exit→E1M2 /
    death all still pass.
- **Still spinning at a dead end / door: two more (2026-09-23):** the log still showed
  `turn_around` twice in a row (180+180 = back to start) — some from sampling drawing
  the 180 flip in a non-stuck moment, some Jev's own argmax. Two Doom mechanics added
  (in `--nets doom`, class "an action that undoes itself", like USE re-closing a
  rising door): (a) sampling never draws `turn_around` (kept only when it is Jev's
  argmax); (b) a second `turn_around` in a row is replaced with a turn toward the most
  open direction (`no-spin` flag). Also added `way_out` {bearing, dist, side,
  dead_end} to the state and rewrote the turn_around criteria + DryJev stuck-handling
  to head for it. Verified: a policy that returns `turn_around` every step now yields
  **0 consecutive** turn_arounds (each second one → a single open-ward turn);
  regressions all hold.
- **View resolution: 7 sectors vs the full 51-ray fan (2026-09-23):** the user asked
  whether collapsing the firmware's 51-ray fan (+/-45 deg, ~1.8 deg/ray) to 7 sectors
  (min/max each) makes Jev "see badly". Built `tools/compare_view.py`: replays a
  run.jsonl and asks Jev twice per step with two states differing ONLY in `view`
  (7 sectors vs the raw 51 rays), everything else identical. Over ~80 real situations:
  **~80-85% identical macro**, and **identical on every committed action** (door
  use/wait, enemy hold-and-fire, forward, wall turns). Divergences were all
  low-confidence (0.2-0.4) steering near-ties in tight spots, and the full fan was not
  more confident there. The one consistent win for the full fan: **approaching a
  narrow doorway** (e.g. step 45/46 forward_right confidence 0.26/0.57 -> 0.71/0.77;
  the 7-sector view sometimes hesitated with a wrong small turn). Full fan costs +~775
  tokens/call (+23%). Decision (user picked "adaptive"): `view_of()` adds `view.rays`
  (the full fan) on top of the sectors only near a door or when stuck (`--view-rays
  auto`, default; `always`/`never` to force). The doorway benefit is taken where it
  matters without paying +23% in empty corridors. DryJev / regressions unaffected
  (they read `view.sectors`, always present).
- **Left/right sweep in place (fixed 2026-11-02, L1 and L2 on the board):** the marine
  stood still for 80+ decisions (`mv 0`) turning RRR LLLL RRR... Causes: (a) the view
  is only +/-45 and Jev has no memory, so after each turn it forgot the open direction
  it had just seen and turned back to "look again"; (b) `dithering` only caught strict
  L/R/L/R alternation, not runs; (c) in L2 the commander's `target_heading` (0 = north)
  pointed into the wall right in front, so `turn_hint` kept pulling it back to the
  wall, then going_nowhere turned it away, and so on; (d) the supervisor returned
  empty text (`max_tokens` 400 used up by the model's default adaptive thinking) so the
  stale heading was never replaced. Fixes, all as facts in the state (no new override):
  `Memory.note_view` keeps a **panorama** (depth per 10-deg compass bin, reset after
  moving 64 units) and the state gets `best_seen` {heading, dist, bearing, side} when
  stuck; `dithering` also = turns-only in both directions while going_nowhere
  (`is_sweeping`); `commander.turn_hint` becomes `blocked` when the panorama shows a wall
  on the target heading (`target_heading_open_dist` < 160) and `paused` while stuck;
  instructions/criteria updated (keep turning the same way to best_seen, open path beats
  turn_hint); DryJev uses best_seen. Supervisor: `max_tokens` 4000 + effort `low`, clear
  error with `stop_reason` on an empty reply, system prompt says headings are not paths
  and barrels are never targets. Verified: replay of the log's sweep (synthetic room)
  -> `dithering` true, `turn_hint` blocked, DryJev out in 4 steps; sim regressions
  (default identical to before, barrel, door) pass. **Not verified with real Jev.**
- **Realtime: every decision one macro late (fixed 2026-11-02, L1 `--realtime` run):**
  stuck ~110 decisions in a small room doing forward / L L / forward / R R. In
  realtime the board answers one frame after the new keys go down, so the obs the
  next decision was made from predated the macro just sent: after a turn_left the
  next state still showed the old heading (35% of 572 turns in that run), Jev turned
  again (60 deg instead of 30), and the following `forward` ran while the second turn
  was still swinging it, into the wall (`BLOCKED` with ah ~290). `best_seen` was right
  (heading ~290, 440 open) and Jev did pick forward there; the lag threw it off. Fix:
  after sending a macro, wait until its hold expires, then take a fresh frame with an
  empty `{"hold": true, "tics": 1}` (stays realtime; its `blocked` refers to the macro).
  Halves the decision rate on the board (~1.7/s instead of ~3.5/s at ~280 ms Jev
  latency) but each decision sees the result of the previous one. `--rt-pipelined`
  restores the old behaviour. Sim (DryJev): stale-after-turn 50% -> 0%; lockstep
  unchanged. **Not yet re-run on the board.**
- **Invisible wall: pushing forever into an "open" direction (fixed 2026-11-02, L1 run):**
  stuck 50+ decisions with `mv 0`, pattern `forward BLOCKED / turn_right / turn_left /
  forward BLOCKED`, `ah465` every time. The firmware ray (`agent_ray_traverse`) stopped
  only on one-sided/blocking lines or openings < 56 high, **not on a step up > 24 units**
  (unwalkable in Doom), so the view read 465 open across a ledge; the panorama
  ("latest look wins") re-learned 465 after every turn, so best_seen/way_out pointed
  straight back at it. Two fixes: (a) agent: `Memory.note_walk_block` records the
  heading of every forward/forward_long that comes back `blocked` (not with a monster
  in the way), `mask_walk_blocks` lowers ray depths within +/-12 deg of it to 32 while
  within 48 units of that spot (150 decisions), in place, so view/sectors/openings/
  way_out/panorama all agree; `view.ahead_unwalkable` + legend sentence. Covers also
  solid decorations and body-catching corners the rays can't show. (b) firmware: the
  ray tracks the floor height along its path and stops at a step up > 24
  (`s_ray_floor`, far side via `P_PointOnLineSide(trace...)`). Verified: synthetic
  replay of the log (scratch test, DryJev) old = stuck 40/40 with the same pattern,
  new = out at step 15; sim regressions (default, barrel, door) pass. **Firmware change
  not built/flashed yet; agent not yet re-run with real Jev.**
- **Floors: stairs, lifts, nukage (2026-11-02, firmware + agent):** Jev knew nothing
  about heights (the agent didn't even pass `z`) and the automap drew EVERY discovered
  line as `#`, so stairs looked like a stack of walls and nukage pools were invisible
  (the marine died standing in them). Firmware (`doom_port.c` / `doom_agent.[ch]`):
  each ray now reports `rise` (highest walkable floor above the player's, > 0 =
  stairs/ramp up), `hurt` (distance to where a damaging floor begins) / `safe` (on a
  damaging floor: distance to a safe one), only when present; the obs adds
  `floor_damage` (sector specials 7=5, 5=10, 4/11/16=20) and `radsuit_tics`; a step
  > 24 with a lift special (10/21/62/88/120-123) is reported as `door_ahead` with
  `kind: "lift"`; the automap draws `#` only for lines that block walking, `:` for a
  walkable step, `~` for the edge of a damaging floor, and no longer draws flat open
  sector boundaries (an opened door disappears from the map). Agent: sectors get
  `climb` / `hurt_dist`, `view.ahead_hurt_dist`, `floor` {hurts, radsuit, way_off}
  while standing on one, `player.radsuit_secs`, `door_ahead.kind`; legend, macro
  criteria, priority (0) "get off a damaging floor first", supervisor + MCP map
  legends, DryJev. All new fields are optional: old firmware / the simulator just
  don't send them. Verified with synthetic obs only; **firmware not built** (no IDF
  on the machine of that session).
- **Door: USE pressed out of reach (fixed 2026-11-02, L2 realtime run):** Jev pressed
  `use` 3 times standing still at 78 units (USE reaches 64); the approach-door net let
  it through (threshold 80, meant for momentum), the 3 misses marked a normal door
  `wont_open`, and the marine swung left/right in front of it for 25 decisions (~1
  min). Now `use_reaches()`: <= 64, or <= 80 only right after a forward move;
  approach-door and the press counter both use it; criteria say `dist <= 60`.
- **Turn / counter-turn in place (fixed 2026-11-02, L2 realtime run):** 763 of 1024
  decisions were a turn opposite to the previous one, standing still, no enemy in
  view (e.g. steps 905-948: heading 312 <-> 346 with best_seen at 325, 714 open).
  A big turn is ~34 deg on the board, the instructions asked |bearing| <= 10 before
  walking, so with the target in between Jev overshot one way, then the other,
  forever. New `--nets doom` mechanic `no-undo` (same class as `no-spin`: an action
  that undoes the previous one): a turn in place (big or small) opposite to the
  previous turn in place, with no hostile in view and no door being worked, becomes
  `forward_<Jev's side>` if >= 64 units are clear ahead, else the previous turn again
  (keep sweeping one way). Legend/criteria: big turns only for |bearing| > 25, 10-25
  = forward_left/right or a small turn. Sim with a policy mimicking the log (big
  turns only, walk only within +/-10): no-enemy stretch zig-zags forward ~760 units
  in 30 steps instead of turning in place; DryJev regressions unchanged (net never
  fires). **Not yet run with real Jev.**
- **Game handling** (always on): death → wait 1 s, USE (level restarts); "frozen"
  game (8 steps with no change at all = intermission, or a menu) → USE/wait pulses,
  then "press Esc" hint; WebSocket/Wi-Fi loss → reconnect (10 attempts, backoff);
  level change detected.
- **Jev errors:** 2 s timeout (must stay well under the board's 5 s agent watchdog),
  one retry on refused connection, 4xx≠429 fatal (no retry), otherwise local
  fallback policy with backoff 1→15 s and "back online".
- **Supervisor:** background thread; replans every 10 s or on triggers (≥5 blocked in
  a row or ≥8 in the last 20 steps, hp −25, new level, no automap growth in 20 s).
  Prompt = automap ASCII + trajectory + stats; reply = JSON
  `{goal, target_heading, fire_policy, preferred_weapon, note}`.

## Firmware facts learned (useful beyond this work)

- **Lockstep catch-up:** doomgeneric runs vanilla sync (`new_sync = 0`). After a
  pause of n tics, the first `doomgeneric_Tick()` runs `min(5, max(1, n))` buffered
  tics with the new keys already held (BuildNewTic caps the buffer at 5). So a
  lockstep step of `tics` advances ≈ `tics + 4` after a ≥150 ms pause. The agent
  estimates it from its own pause (`--catchup -1`).
- **Turning:** 6 slow tics (320/65536 turn = 1.76°/tic) then 3.52°/tic. Walk accel
  0.78 u/tic², friction 0.90625, top speed ≈ 8.3 u/tic.
- **USE:** 64-unit reach, edge-triggered (`usedown`), toggles vertical doors;
  `door_ahead` stays present until the opening is 56 units high.
- **Intermission:** the level's mobjs stay alive, so `/agent` keeps returning a valid
  but frozen observation; USE/FIRE advances the stats screen.
- **Title demo:** the demo player is a real mobj → observations look valid; always
  start a New Game before connecting an agent.
- **Chip revision:** `sdkconfig` targets ESP32-P4 **v3.1–v3.99**
  (`CONFIG_ESP32P4_REV_MIN_301`, `SELECTS_REV_LESS_V3` off) — the IDF 5.5.4 default,
  not pinned in `sdkconfig.defaults`. `sdkconfig.no_psram_backup` is a leftover
  v0.1–v1.99 config from the original JC4880P443 port.

## Realtime patch (`tools/firmware-realtime-agent-mode.patch`)

- `{"hold": true}` in an action: keys stay held, the game never pauses, the reply
  comes after one frame, `tics` = auto-release timeout (1..175, default 35). Keys are
  swapped by diff (a held FIRE/USE is not re-pressed). `blocked` refers to the
  previous held action (threshold scaled with elapsed tics). Disconnect or 5 s of
  silence → release keys, back to local play. A request without `hold` → lockstep.
- **Built with ESP-IDF v5.5.4** for esp32p4 with this repo's `sdkconfig` and
  `managed_components`: no errors/warnings in the patched files, app 0x146020 bytes
  (68 % of the 4 MB partition free). **Not yet run on the board.**
- Apply: `git apply tools/firmware-realtime-agent-mode.patch` (files are CRLF;
  if it refuses, `--ignore-whitespace`; GNU `patch` needs `--binary`).
  Undo: `git checkout -- main/doom_agent.h main/doom_agent.c main/doom_port.c`.
- Or flash the prebuilt images in `_flash_realtime/` (see its `FLASH.txt`).

## Verification done

- Container + the local Linux VM, against the simulator and the mocks: Python 3.13.7
  + websockets 16.0 (same as `tools\venv`) and Python 3.10 + websockets 16.1.
- `--check` paths (ok, 401, Jev down, board down, missing keys/model), L1/L2 lockstep
  and realtime, anthropic/openai/dry supervisors, `--no-heuristics --policy sample`,
  exit→E1M2 and death→respawn scenarios (both pacings), link drop and recovery, Jev
  down at start then recovery. pyflakes clean.
- Measured on the simulator with 130 ms model latency: lockstep 2.4 decisions/s at
  67 % of real time; realtime 7.4 decisions/s at 100 %.
- **Not verified:** real Jev and real Claude (no keys in that session), and anything
  on the physical board.

## Next steps

1. Flash a release (`binaries/.../flash.bat COMx`, or `idf.py build app-flash`),
   check `chip revision` and the IP (Wi-Fi or Ethernet) on the serial log.
2. `python tools\doom_jev_agent.py <IP> --mode supervised --check`, then level 1,
   level 2, then `--realtime`. Keep the `--log` JSONL files.
3. Tune `MACROS` descriptions / `build_questions` from the real logs.
4. Housekeeping (the merge into `master` is done): pin `CONFIG_ESP32P4_REV_MIN_301=y` in `sdkconfig.defaults`;
   README says 32 MB flash but `sdkconfig` is 16 MB; `pytest_doom_p4.py` is Espressif
   boilerplate; delete the stale `Claude outputs/` folder (old pre-review patch).
