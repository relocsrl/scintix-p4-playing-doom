# Jev plays DOOM — work notes (session of 2026-09-21/22)

Handoff notes for whoever picks this up next (human or Claude Code). What was
built, why it is shaped this way, what was verified, and what is still open.
User-facing docs are in [`tools/README.md`](../tools/README.md) §3–4.

## Goal

Let **Jev** (TypeSafe AI's "System One" model, released 2026-09-16: typed,
calibrated decisions in ~100–300 ms instead of prose) play DOOM on the SCINTIX P4
through the firmware's `/agent` WebSocket (branch `ai-agent-api`), in two
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
| `tools/firmware-realtime-agent-mode.patch` | Optional firmware patch (not applied to `main/`): `{"hold": true}` = free-running agent mode. |
| `tools/README.md`, `tools/requirements.txt` | Docs for all of the above (no new dependencies). |
| `_flash_realtime/` (local only, `*.bin`/`*.elf` are gitignored) | Prebuilt images of `ai-agent-api` + the realtime patch, `FLASH.txt` with the esptool commands, `SHA256SUMS`. |

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
  (min/max) + detected openings, enemies in view, `door_ahead`, blocked flags, last 8
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
  enemy in view); approach-door (USE reaches only 64 units); door-wait (USE *toggles*
  a door — a second press while it rises sends it back down — 30-tic cooldown).
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

1. Flash (`_flash_realtime/FLASH.txt`, or apply the patch and `idf.py build app-flash`),
   check `chip revision` and the IP on the serial log.
2. `python tools\doom_jev_agent.py <IP> --mode supervised --check`, then level 1,
   level 2, then `--realtime`. Keep the `--log` JSONL files.
3. Tune `MACROS` descriptions / `build_questions` from the real logs.
4. Housekeeping: merge `ai-agent-api` into `master` (origin/master's README already
   describes the agent); pin `CONFIG_ESP32P4_REV_MIN_301=y` in `sdkconfig.defaults`;
   README says 32 MB flash but `sdkconfig` is 16 MB; `pytest_doom_p4.py` is Espressif
   boilerplate; delete the stale `Claude outputs/` folder (old pre-review patch).
