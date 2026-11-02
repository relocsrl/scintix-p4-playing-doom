# SCINTIX P4 DOOM — agent tools

Tools to drive DOOM running on the SCINTIX P4 from a computer, and to let an AI
play it — an LLM via MCP, or TypeSafe's **Jev** (a fast "System One" model) at
several decisions per second, alone or supervised by an LLM. They talk to the
firmware's **`/agent` WebSocket**, which by default runs the game
in **lockstep**: you send an action, the game advances a few tics, and you get
back a structured observation.

| File | What it is |
|------|------------|
| `agent_client.py` | Test client / interactive REPL to drive the game by hand |
| `doom_mcp_server.py` | MCP server exposing the game as tools for an LLM (slow, deliberate play) |
| `doom_jev_agent.py` | **Jev plays** — level 1 (Jev alone) or level 2 (Jev + LLM supervisor) |
| `doom_agent_sim.py` | Simulator of the device's `/agent` API, to develop/demo without the board |
| `tests/mock_servers.py` | Fake Jev + fake LLM endpoints: run the whole pipeline with no API keys |
| `firmware-realtime-agent-mode.patch` | Optional firmware patch: free-running `/agent` (`"hold": true`) for real-time play |
| `requirements.txt` | Python deps (`websockets`, `mcp`) |

## 0. Prerequisites

- Firmware v0.5.0 or later on the device: a build from `master`, or the prebuilt
  images in [`../binaries/`](../binaries/).
- The device on the network, over Wi-Fi or Ethernet (a cable in the carrier's RJ45,
  DHCP). Note its IP from the serial log (`Connected to Wi-Fi with IP ...` /
  `Ethernet up with IP ...`), look for `scintix-p4-xxxxxx` in your router's client
  list, or use `192.168.4.1` if it's running the captive-portal AP (`SCINTIX-P4-XXXXXX`).
- **Python 3.10+** on the PC.

Install the deps (a virtualenv is recommended):

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1          # PowerShell  (cmd: .venv\Scripts\activate.bat)
pip install -r requirements.txt        # installs websockets + mcp
```

> Get the game **into a level first** (New Game from the menu, with the keyboard)
> before driving it — at the title/menu there is no live player to observe.

## 1. Manual control — `agent_client.py`

```bash
python agent_client.py <device-ip>            # runs a short demo sequence
python agent_client.py <device-ip> --repl     # interactive control
```

REPL commands (optional integer = tics to advance, default 8):

```
w/s = forward/back   a/d = turn left/right   q/e = strafe left/right
f = fire   u = use/open   1-7 = select weapon   o = observe only
m = automap (ASCII)  raw {...json...} = send a raw action          quit = exit
```

## 2. Let an LLM play — `doom_mcp_server.py` (MCP)

The MCP server is **launched by your MCP client**, not by you. You only tell the
client how to start it (command + args + the `DOOM_WS_URL` env var). It holds one
persistent lockstep connection for the whole session.

It exposes these tools: `observe`, `move_forward`, `move_back`, `turn_left`,
`turn_right`, `strafe_left`, `strafe_right`, `fire`, `use_`, `select_weapon`,
`get_map`.

### Register it with Claude Code (CLI)

With the venv active in your terminal (so `python` has the deps and Claude Code
inherits the environment):

```
claude mcp add doom-scintix-p4 \
    -e DOOM_WS_URL=ws://<device-ip>/agent \
    -- python D:/_GIT/scintix-p4-playing-doom/tools/doom_mcp_server.py
```

Then launch `claude` in the same terminal and check it's wired up:

```
claude mcp list          # should show doom-scintix-p4
```
(or `/mcp` inside a session). If you ever get an ImportError on launch, use the
venv's interpreter explicitly instead of bare `python`:
`D:/_GIT/scintix-p4-playing-doom/tools/.venv/Scripts/python.exe`.

### Or register it with Claude Desktop

Edit `%APPDATA%\Claude\claude_desktop_config.json` (Windows) and restart the app.
Use the **full path** to the Python that has the deps — desktop apps don't inherit
your shell PATH/venv:

```json
{
  "mcpServers": {
    "doom-scintix-p4": {
      "command": "D:/_GIT/scintix-p4-playing-doom/tools/.venv/Scripts/python.exe",
      "args": ["D:/_GIT/scintix-p4-playing-doom/tools/doom_mcp_server.py"],
      "env": { "DOOM_WS_URL": "ws://<device-ip>/agent" }
    }
  }
}
```

## 3. Let Jev play — `doom_jev_agent.py` (System One, fast)

[Jev](https://typesafe.ai) is TypeSafe AI's *System One* model: instead of prose it
returns **typed, calibrated decisions** (a choice with probabilities, a yes/no
probability) in ~100–300 ms. `doom_jev_agent.py` feeds it the `/agent` observation
and lets it drive the marine several times a second. Two levels of control:

| Mode | What runs | When to use |
|------|-----------|-------------|
| `--mode jev` (default) | **Level 1 — Jev alone.** Every step: observation → compact JSON state → one Jev call with two questions: *which macro* (Choice over 14 movement/interaction macros) and *hold fire?* (Noul). | Pure reflexes, fastest, cheapest. |
| `--mode supervised` | **Level 2 — Jev + LLM supervisor.** Same reflex loop, plus a slow *System Two* (Claude by default; any OpenAI-compatible endpoint; or a built-in `dry` heuristic) that every ~10 s (or when stuck / hurt / new level / no progress) reads the **automap**, the trajectory and the stats and rewrites the **intent**: goal, target compass heading, fire policy, preferred weapon. The intent is injected into Jev's state as `commander` (with a `turn_hint`); Jev keeps taking the fast decisions. | Exploration and strategy: the reflex layer has no memory of the map. |

### Setup

Same venv as §0 — only `websockets` is needed (Jev and the supervisor are called
over plain HTTPS with the standard library).

```powershell
# in tools/, venv active (see §0)
$env:TYPESAFE_API_KEY  = "..."      # Jev (TypeSafe API console)
$env:ANTHROPIC_API_KEY = "..."      # only for --mode supervised with --supervisor anthropic (default)
$env:OPENAI_API_KEY    = "..."      # only for --supervisor openai (or any OpenAI-compatible server)
```
(`export VAR=...` on Linux/macOS.) Stop the MCP server if it is running — the
device accepts **one** agent connection — and get the game **into a level**.

### Preflight: `--check`

```bash
python doom_jev_agent.py <device-ip> --check                        # Jev key + latency + board
python doom_jev_agent.py <device-ip> --mode supervised --check      # + the supervisor LLM
```
It sends three sample decisions to Jev (latency, tokens per call, cost per hour),
one sample plan to the supervisor, opens `/agent` once, prints what it found and
exits. A bad key (401/403) or a rejected request (4xx) is reported here, before the
game starts; without the board the cloud checks still run.

### Run

```bash
python doom_jev_agent.py <device-ip>                          # level 1: Jev alone
python doom_jev_agent.py <device-ip> --mode supervised        # level 2: Jev + Claude (claude-sonnet-5)
python doom_jev_agent.py <device-ip> --mode supervised --supervisor openai --supervisor-model <model> \
                                     --supervisor-base-url https://api.openai.com/v1   # or any compatible server
python doom_jev_agent.py <device-ip> --mode supervised --supervisor dry   # heuristic "frontier" planner, no LLM key
python doom_jev_agent.py <device-ip> --jev dry                # no Jev key: transparent local policy (same loop)
python doom_jev_agent.py <device-ip> --log run.jsonl --steps 500 --show-map-every 30
```

The run also takes care of the game around the reflexes: when the marine **dies**
it waits a second and presses USE (the level restarts); when **nothing changes**
for `--frozen-after 8` steps whatever it presses — the end-of-level stats screen —
it presses USE to move on (and tells you to press Esc on the keyboard if a menu is
open); when the **network link drops** it reconnects (up to 10 attempts) and resumes.

Every step prints one live line — a **radar of what Jev sees** and the **macro it
chose**, colour-coded (nice next to the game on screen / on video):

```
 137 hp100 [.:# .  ]  ah931  AIM imp 1041@+1  door 979@+3  => < turn_left_small  FIRE  c0.59  mv214  407ms
```
Left to right: step, `hp`; the **radar** `[.:# .  ]` = the 7 depth sectors left→right,
taller/greener = more open, dimmer/red = a near wall (the centre cell is straight
ahead) — this is literally what Jev reads; `ah` = clear distance straight ahead (wall
OR barrel); then the salient things in view — `en <name> dist@bearing` (yellow), or
**`AIM …`** in red when the enemy is on target, `barrel …` (magenta), `door
dist@bearing` (cyan; `(x2)` = pressed twice, `!` = won't open); `BLOCKED` if the last
move hit something; then `=> <glyph> <macro>` = the decision (e.g. `^` forward, `->`
turn_right, `()` use, `..` wait), **`FIRE`** in red when it shoots, `c` = confidence,
`mv` = units moved over the last 10 decisions, and the model latency. A one-line key
is printed once at the start. `--color auto` (default) colours it when the output is a
terminal; `--color always` (for a recorder that isn't a TTY) or `never`. The automap
(`--show-map-every`) still prints every N steps. In level 2 the supervisor's plan
prints too:

```
  [supervisor stuck, 2.1s] goal: Head east along the corridor and open the door at its end | heading 90 | fire normal | ...
```
Trailing flags mark anything that
was **not** Jev's own decision: `FALLBACK` (Jev unreachable → local policy, retried
with backoff 1→15 s; a 401/403/4xx stops the retries), `sampled` (low confidence →
sampled from Jev's probabilities), `approach-door` / `door-wait` / `no-spin` (the
always-on Doom mechanics; `no-spin` = a second turn_around in a row was replaced, it
would only spin back), `unstick` / `anti-osc` / `stall` / `door-retry` / `dead-door`
(only with `--nets all`; `door-noted` = a door just found not to open), `respawn` / `unfreeze`
(death and end-of-level handling). `--log file.jsonl` records the full state sent, Jev's answers,
the action and the observation for every step, plus every supervisor plan. Ctrl+C
prints a summary (steps, game time, latency, tokens/cost, deaths, levels played,
plans) and the device resumes real-time play.

### What Jev sees and decides

The 51-ray depth fan is collapsed into **7 sectors** (`min/max` wall distance
each) plus detected **openings** (rays much longer than their neighbours), the
`visible` list split into `enemies_in_view` (monsters) and `hazards_in_view`
(barrels: obstacles to go around, never targets to centre), `door_ahead`, `blocked`,
a short **memory** (last 8 macros, consecutive blocked count), a 9×25 **automap
window** around the player, and the `commander` intent. Each option's *criteria*
text says **when** that macro is the right one, e.g. `use`: "USE only reaches ~64
units, right when door_ahead is very close (dist < 70) and roughly centred".
Macros: `forward`, `forward_long`, `forward_left/right`, `turn_left/right` (~30°),
`turn_left/right_small` (~9°), `turn_around`, `back`, `strafe_left/right`, `use`,
`wait`.

**Memory Jev cannot see from one frame** is put into the state as plain facts, so
Jev keeps deciding instead of being overridden:

- `door_ahead.presses` / `tics_since_press` — you pressed USE on *this* door N
  times, the last one this many game tics ago (35 tics = 1 s). A door that opens
  is gone from `door_ahead` ~28 tics after the press, so `tics_since_press < 50`
  means it is still rising (wait, do not press again — that sends it back down),
  and `>= 50` with the door still there means the press did not take (press again).
- `door_ahead.wont_open` — pressed 3 times and still shut: locked (needs a key) or
  not a real door. Treat it as a wall; it clears after 2 minutes.
- `progress.moved_last_10_steps` / `going_nowhere` (< 48 units) — how far you got
  over the last 10 decisions; going nowhere = dithering, spinning, waiting or
  pushing into something (turning in place is never `blocked`, so this is how Jev
  learns it is stuck). `dithering` — the last macros alternate a move and its
  opposite.
- `view.ahead_min_dist` — how far the marine can actually walk straight before
  hitting something, **wall OR barrel**. Barrels are not in the wall fan, so without
  this the depth reads the wall *behind* a barrel and forward looks clear while the
  marine plows into it.
- `view.rays` — the **full 51-ray depth fan** (~1.8° apart), added on top of the 7
  sectors only when it earns its ~+750 tokens: near a door or when stuck (`--view-rays
  auto`, the default; `always` / `never` force it). An A/B (`compare_view.py`) found
  the 7 sectors match the full fan on ~85 % of decisions — identical on every
  committed action (door use/wait, hold-and-fire, forward) — differing only in
  low-confidence steering near-ties; the one consistent win for the full fan is
  **lining up on a narrow doorway** (higher confidence, less hesitation), so that is
  exactly where `auto` sends it.
- `aim` (nearest monster) with `aim.on_target` — the enemy is inside the gun's hit
  cone (|bearing| ≤ 8) and within range: FIRE and hold heading, do not turn. At
  ~0.7 s decision lag a turn to "centre better" acts on a stale bearing and
  overshoots — that swing-past-and-back is what made the marine dance around an enemy.
- `way_out` {bearing, dist, side, dead_end} — the most open direction in view. When
  stuck (blocked / going nowhere), make one turn toward it and go forward instead of
  spinning; `turn_around` is only for a `dead_end`, and never twice in a row (that
  just returns you to where you started — enforced as the `no-spin` mechanic).

### Safety nets (`--nets`)

Two Doom mechanics a single frame genuinely cannot show are still enforced after
Jev's choice (default `--nets doom`, each flagged in the telemetry):
**USE reaches only 64 units** (a `use` on a far door becomes a step toward it);
**USE toggles a door** — pressing it again while it rises sends it back down (after
a `use`, another `use` within `--use-cooldown` 30 tics becomes `wait`). Everything
else — getting unstuck, not dithering, retrying a door, giving up on a locked one —
is Jev's own call from the memory facts above.

`--nets all` restores the older behaviour, where the agent also **overrides** Jev
when it looks stuck: unstick after `--unstick-after` 4 blocked moves, break
left/right dithering, a stall net (turn toward open space when `going_nowhere` with
no enemy, quiet while a door rises), USE-retry on a shut door, and a turn-away from
a `wont_open` door. `--nets none` (alias `--no-heuristics`) is pure Jev, mechanics
included. Death / end-of-level handling is always on regardless.

`--policy sample` always samples from Jev's distribution instead of taking the
argmax (more exploration); `--min-confidence` sets when `mixed` (default) samples —
but `mixed` does **not** sample while stuck (blocked or going nowhere): rolling the
dice right there is what makes the marine flail, so it takes Jev's top choice
instead. `--policy sample` still samples everywhere.

### Pacing: lockstep vs. real time

By default `/agent` is **lockstep**: the game is frozen while we think. A step
is `Jev latency (~150–300 ms) + tics × 28.6 ms` of device time, so with the default
macros (8 tics ≈ 0.23 s) the game runs at roughly **50–70 % of real time**, 2–3
decisions/s, and the marine moves in small bursts — much smoother than an LLM over
MCP (seconds per step), not yet a continuous player. One subtlety handled for you:
after our pause the device's first tick runs up to ~5 buffered game tics with the
new keys already held (vanilla Doom sync), so the agent estimates that catch-up
from how long it paused (up to 4 extra tics after a ≥150 ms pause, 0 after a very
short one; `--catchup N` forces it) and subtracts it from each macro, so turns land
where the macro says. Jev calls time out after `--jev-timeout 2` s, well under the
board's 5 s agent watchdog (past it the board would resume local play until the
next request).

**Free-running mode (`--realtime`).** Since v0.5.0 the firmware's action also
accepts `"hold": true`: the game **never pauses**, each decision just
replaces the keys being held and is answered at once, and `tics` becomes the
auto-release timeout (a vanished controller never leaves the marine running into a
wall). The agent then runs at Jev's pace — **~5–8 decisions/s at 100 % game speed**
in the simulator with a 130 ms model latency — and turns/moves last exactly as long
as the model keeps choosing them (or the macro's duration if the next decision is
late). A request without `hold` is still lockstep, so the same firmware serves both
modes. On firmware older than v0.5.0 the agent notices (replies as slow as lockstep)
and says so; it keeps working, in lockstep. The simulator supports it too:

```bash
python doom_jev_agent.py <device-ip> --realtime
python doom_jev_agent.py localhost --port 8765 --jev dry --realtime   # against the simulator
```
`tools/firmware-realtime-agent-mode.patch` is that firmware change as a patch, kept
for reference: it is already applied in `main/`.

### Troubleshooting

- **`HTTP 403 ... error_code: 1010` from Jev** — Cloudflare bot protection in front of
  the API blocked the request, *not* a bad key. `urllib`'s default User-Agent
  (`Python-urllib/x.y`) trips it; the client now sends a browser User-Agent by default,
  which gets through (verified: default UA → 403/1010, browser UA → the API answers).
  Override with `--jev-user-agent "..."` or `TYPESAFE_USER_AGENT` if needed. If it still
  returns 1010 with a browser UA, the block is on the TLS fingerprint (urllib can't
  spoof that) — reach the API through a browser-like client or a proxy.
- **`HTTP 401/403` without a Cloudflare 1010 body** — the key is wrong or not enabled.

### Cost

A step is ~1.5 k input tokens (state + questions); at 2–3 steps/s (lockstep) that
is about **$0.5–0.7 per hour** at $0.042 / Mtok (output is free), ~$1.6/h at 7
steps/s in realtime mode; `--check` prints the figure for your actual token count. The supervisor adds one
LLM call every ~10 s (~3 k tokens each).

## 4. No board handy? `doom_agent_sim.py`

A simulator of the firmware's `/agent` API, so the agents can be developed and
demoed without hardware: a two-room 2D world with a corridor, a **door** that opens
on `use` and re-closes (Doom semantics, including the USE toggle), a couple of
imps that walk toward you and bite, a barrel, an **exit** at the end of the
dead-end spur north of the second room (→ stats screen until USE → E1M2), death
(USE restarts the level); Doom-like movement (momentum, friction, slow/fast turn,
the board's lockstep catch-up), a 51-ray depth fan, `visible` in FOV + line of
sight, `door_ahead`, `blocked`, and the discovered-walls automap — same JSON as the
device, including the realtime `hold` mode.

```bash
python doom_agent_sim.py                      # ws://localhost:8765/agent (resets on each connect)
python doom_agent_sim.py --realtime -v        # ~28.6 ms per tic like the board, verbose
python doom_agent_sim.py --spawn 1964,1150,0  # scenario: start just south of the exit, facing it
python doom_agent_sim.py --spawn 2200,245,180 --hp 5   # scenario: next to an imp, nearly dead
python doom_jev_agent.py localhost --port 8765 --jev dry --steps 200
python doom_mcp_server.py                     # with DOOM_WS_URL=ws://localhost:8765/agent
python agent_client.py localhost --port 8765 --repl
```

`tests/mock_servers.py` adds a fake Jev endpoint (validates the request shape,
answers with the real response schema, injects a 429 now and then) and a fake LLM
(Anthropic Messages + OpenAI Chat), so the **whole two-level pipeline runs with no
API keys** — the docstring has the three commands (ports 8899/8898, or pass
two ports).

## Protocol (for reference)

**Action** (client → device), all fields optional:

```json
{"move":"forward|back","turn":"left|right","strafe":"left|right",
 "fire":true,"use":true,"weapon":1-7,"tics":N}
```
`tics` (default 7) = game tics to advance this step (35 tics ≈ 1 second).
With the free-running firmware patch, `"hold": true` keeps the keys held and the
game running until the next action (then `tics` = auto-release timeout, default 35).

**Observation** (device → client):

```json
{"valid":true,"x":..,"y":..,"z":..,"angle":0-359,
 "health":..,"armor":..,"weapon":"shotgun","ammo":..,"episode":..,"map":..,
 "visible":[{"name":"imp","dist":410,"bearing":-12}, ...],
 "walls":[{"bearing":-45,"dist":..}, ..., {"bearing":45,"dist":..}],
 "door_ahead":{"bearing":..,"dist":..},"use_target":null,"blocked":false}
```

`angle` is a **compass heading**: 0 = north, 90 = east, clockwise — the same frame
as the north-up automap.

All `bearing` values (in `visible`, `walls`, `door_ahead`) are degrees from where
you face: **0 = centre, negative = left, positive = right** — so `turn_right`
brings a positive-bearing thing toward the centre and `turn_left` a negative one.

`walls` gives the distance (map units) to the nearest wall/closed obstruction
along rays fanned across the field of view (the depth of the rendered view) — use
it to judge how far you can advance and to spot openings (a ray noticeably longer
than its neighbours is a passage). Rays only cover the FOV, never behind you.

`door_ahead` appears **only** when a closed door/passage is in view (the most
central one, as `{bearing, dist}`): bearing and dist point at the **middle** of the
door line (since v0.6.2; bearing can exceed ±45 when only the door's edge is in
view). Press use to open it; if it reappears after you opened it the door
re-closed, so press use again instead of walking into it.

`use_target` (since v0.6.2, always present) is what is under the crosshair within
USE reach: the firmware runs the same 64-unit trace along the facing as Doom's
`P_UseLines`, without activating anything. `"door"`, `"lift"`, `"switch"`, or
`null` = nothing (a wall comes first, or the door is beside you, not in front).
Only what a player sees on screen: an exit switch is reported as a plain
`"switch"`. `use_locked: true` = a key door (colour-marked) you have no key for.
`blocked` is true when your last move barely shifted you (you ran into a wall or a
shut door).

`visible` lists **only what the player can see on screen** — things inside the
field of view *and* in line of sight, ordered left-to-right. Enemies behind you or
hidden behind walls are **not** reported, and there are no targeting hints: the
model makes all tactical decisions itself.

**Automap** — send `{"request":"map"}` (or `{"map":true}`) and the device replies
with the ASCII automap, the same information the in-game automap shows:

```json
{"valid":true,"cols":64,"rows":32,"units_per_cell":N,"origin_x":..,"origin_y":..,
 "player":{"col":..,"row":..,"angle":..},"grid":["  ## ", "#  @#", ...]}
```
`grid` holds only the walls **already discovered** by exploring (`#`), the player
(`@`) and blanks for the rest; north is up and `player.angle` is a compass heading
(0 = north, clockwise). No monsters or items — exactly like the player's own
automap.

## Notes

- The device accepts **one** agent connection at a time.
- While an agent is connected the game is **paused between steps** (the screen
  freezes on the last frame). Disconnect and it resumes real-time play
  (keyboard/gamepad).
- After a pause the device's first tick catches up to ~5 buffered game tics with
  the new keys held (vanilla Doom sync), so a step really advances about
  `tics + 4`; `doom_jev_agent.py` compensates (`--catchup`).
- Doom's USE reaches 64 units and **toggles** a door (a second press while it is
  rising sends it back down); `door_ahead` stays present until the opening is
  56 units high.
