#!/usr/bin/env python3
"""
Jev plays DOOM on the SCINTIX P4.

A bridge between the firmware's `/agent` WebSocket (lockstep: send an action,
the game advances N tics, get a structured observation back) and TypeSafe's
Jev — a "System One" model that takes structured state plus typed questions
and returns a calibrated decision in ~100-300 ms instead of prose.

Two levels of control:

  --mode jev          Level 1: Jev alone. Every step the observation is
                      compressed into a small JSON state and Jev answers two
                      questions in one call: which movement macro to run
                      (Choice) and whether to hold FIRE (Noul).

  --mode supervised   Level 2: same reflex loop, plus a slow "System 2" LLM
                      supervisor (Claude by default, any OpenAI-compatible
                      endpoint, or a built-in dry heuristic) that every few
                      seconds reads the automap, the trajectory and the stats
                      and rewrites the *intent* (goal, target heading, fire
                      policy, preferred weapon). The intent is injected into
                      Jev's state; Jev keeps making the fast decisions.

Requires:  pip install websockets        (Python 3.10+)
Env:       TYPESAFE_API_KEY              Jev API key (or --jev-key)
           ANTHROPIC_API_KEY             supervisor, --supervisor anthropic
           OPENAI_API_KEY                supervisor, --supervisor openai

Usage:
    python doom_jev_agent.py <device-ip> --check              # preflight: keys, latency, board
    python doom_jev_agent.py <device-ip>                      # level 1
    python doom_jev_agent.py <device-ip> --mode supervised    # level 2
    python doom_jev_agent.py <device-ip> --jev dry            # no API key: local policy
    python doom_jev_agent.py <device-ip> --realtime           # free-running (firmware `hold` patch)
    python doom_jev_agent.py localhost --port 8765 --jev dry  # against doom_agent_sim.py

Get the game INTO A LEVEL first (New Game from the menu, keyboard) — at the
title/menu there is no live player to observe. Stop the MCP server if it is
running: the device accepts one agent connection at a time. Ctrl+C exits and
the device resumes real-time play.
"""

import argparse
import collections
import json
import math
import os
import random
import sys
import threading
import time
import http.client
import urllib.error
import urllib.request

try:
    from websockets.sync.client import connect as ws_connect
    from websockets.exceptions import ConnectionClosed, InvalidHandshake
except ImportError:  # pragma: no cover
    sys.exit("Missing dependency: pip install websockets")

# Errors that mean "the link to the board dropped" (Wi-Fi hiccup, reboot, ...)
LINK_ERRORS = (ConnectionClosed, InvalidHandshake, TimeoutError, OSError)

# ----------------------------------------------------------------------------
# Constants
# ----------------------------------------------------------------------------

_PRINT_LOCK = threading.Lock()


def say(text):
    """Thread-safe console line (the supervisor thread prints too)."""
    with _PRINT_LOCK:
        sys.stdout.write(text + "\n")
        sys.stdout.flush()


# --- console styling (for the live terminal shown next to the game) ----------
USE_COLOR = False
_ANSI = {"dim": "2", "bold": "1", "red": "31", "grn": "32", "yel": "33", "blu": "34",
         "mag": "35", "cyn": "36", "wht": "97", "bred": "91", "bgrn": "92", "byel": "93",
         "bcyn": "96", "bmag": "95"}


def c(text, *styles):
    """Wrap text in ANSI styles when colour is on, else return it unchanged."""
    if not USE_COLOR or not styles:
        return text
    codes = ";".join(_ANSI[s] for s in styles if s in _ANSI)
    return f"\x1b[{codes}m{text}\x1b[0m" if codes else text


def enable_color(mode):
    """Turn the ANSI styling on (mode 'always', or 'auto' on a TTY). On Windows,
    switch the console into virtual-terminal mode so the codes render."""
    global USE_COLOR
    on = mode == "always" or (mode == "auto" and hasattr(sys.stdout, "isatty") and sys.stdout.isatty())
    if on and os.name == "nt":
        try:
            import ctypes
            k = ctypes.windll.kernel32
            k.SetConsoleMode(k.GetStdHandle(-11), 7)   # ENABLE_VIRTUAL_TERMINAL_PROCESSING | defaults
        except Exception:
            pass
    USE_COLOR = on


TICRATE = 35                      # Doom game tics per second
JEV_DEFAULT_URL = "https://api.typesafe.ai/v1/systemone"
JEV_DEFAULT_MODEL = "jev-latest"
# urllib's default User-Agent ("Python-urllib/3.x") trips Cloudflare bot protection in
# front of the API (HTTP 403, error 1010: "blocked based on your browser's signature").
# Send an ordinary browser User-Agent so a legitimate API call is not mistaken for a bot.
JEV_DEFAULT_USER_AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                          "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36")
JEV_PRICE_PER_MTOK = 0.042        # USD per million input tokens (output is free)
ANTHROPIC_URL = os.environ.get("ANTHROPIC_BASE_URL", "https://api.anthropic.com").rstrip("/") + "/v1/messages"
ANTHROPIC_VERSION = "2023-06-01"
OPENAI_DEFAULT_BASE = "https://api.openai.com/v1"

FOV_DEG = 45                      # half FOV reported by the firmware (rays -45..45)
RAY_RANGE = 2048                  # "clear" distance when a ray hits nothing

# Doom turning: the first 6 tics of a held turn key are "slow turn" (320/65536
# of a circle per tic = 1.76 deg), then 640 (3.52 deg/tic). Keys are pressed and
# released at every step, so each step starts with the slow phase.
SLOW_TURN_TICS = 6
SLOW_TURN_DEG = 320 * 360 / 65536
FAST_TURN_DEG = 640 * 360 / 65536


def turn_tics(deg):
    """Game tics needed to turn `deg` degrees from a standing start."""
    deg = abs(deg)
    if deg <= SLOW_TURN_TICS * SLOW_TURN_DEG:
        return max(1, round(deg / SLOW_TURN_DEG))
    return SLOW_TURN_TICS + round((deg - SLOW_TURN_TICS * SLOW_TURN_DEG) / FAST_TURN_DEG)


def turn_deg(tics):
    """Degrees turned by holding a turn key for `tics` tics."""
    if tics <= SLOW_TURN_TICS:
        return tics * SLOW_TURN_DEG
    return SLOW_TURN_TICS * SLOW_TURN_DEG + (tics - SLOW_TURN_TICS) * FAST_TURN_DEG


# ----------------------------------------------------------------------------
# Macros: the reflex layer's action vocabulary
#
# Each macro maps to one firmware action (held keys) and a default number of
# tics. Descriptions are what Jev reads as the Choice criteria, so they must say
# WHEN the option is the right one, not just what it does.
# ----------------------------------------------------------------------------

MACROS = {
    "forward": dict(
        action={"move": "forward"}, tics=8,
        desc="Walk straight ahead a short distance. Right when `view.ahead_min_dist` "
             "is well above 120 (the path is clear of walls AND barrels) and nothing "
             "more urgent is in view; also up stairs (a sector with `climb`), and to get "
             "off a damaging floor when `floor.way_off` is ahead. Wrong when a barrel sits "
             "in your path (ahead_min_dist small): sidestep or turn to go around it first; "
             "wrong when `view.ahead_hurt_dist` is small and you have no radiation suit "
             "(you would walk onto nukage/slime)."),
    "forward_long": dict(
        action={"move": "forward"}, tics=16,
        desc="Walk straight ahead a long stretch. Only when `view.ahead_min_dist` is "
             "large (>= 500), no enemy is close, the path is a clear corridor "
             "or open room, and no damaging floor begins ahead (`view.ahead_hurt_dist` "
             "absent or beyond 500)."),
    "forward_left": dict(
        action={"move": "forward", "turn": "left"}, tics=8,
        desc="Walk ahead while curving left: follow a passage bending left, or "
             "approach an opening/door/enemy sitting slightly to the left "
             "(bearing about -10 to -35)."),
    "forward_right": dict(
        action={"move": "forward", "turn": "right"}, tics=8,
        desc="Walk ahead while curving right: follow a passage bending right, or "
             "approach an opening/door/enemy sitting slightly to the right "
             "(bearing about +10 to +35)."),
    "turn_left": dict(
        action={"turn": "left"}, tics=turn_tics(30),
        desc="Turn left in place about 30 degrees, no movement. Use to face an "
             "enemy or an opening on the left (bearing < -25), to turn toward "
             "best_seen when its bearing < -25 (closer than that it overshoots: use "
             "forward_left or turn_left_small), to follow a commander turn_hint of "
             "`left` when ahead is not an open path, or to look for a way out when ahead "
             "is blocked (or progress.going_nowhere) and the left side has more room. "
             "Wrong right after turning right while standing still (that is the sweep loop)."),
    "turn_right": dict(
        action={"turn": "right"}, tics=turn_tics(30),
        desc="Turn right in place about 30 degrees, no movement. Use to face an "
             "enemy or an opening on the right (bearing > 25), to turn toward "
             "best_seen when its bearing > 25 (closer than that it overshoots: use "
             "forward_right or turn_right_small), to follow a commander turn_hint of "
             "`right` when ahead is not an open path, or to look for a way out when ahead "
             "is blocked (or progress.going_nowhere) and the right side has more room. "
             "Wrong right after turning left while standing still (that is the sweep loop)."),
    "turn_left_small": dict(
        action={"turn": "left"}, tics=turn_tics(9),
        desc="Small left adjustment (~9 degrees): centre a door, or an enemy that is "
             "off-centre to the left (bearing -9 to -18). NOT when aim.on_target "
             "(bearing within +/-8): the shot already lands, so hold and fire "
             "instead - a turn overshoots."),
    "turn_right_small": dict(
        action={"turn": "right"}, tics=turn_tics(9),
        desc="Small right adjustment (~9 degrees): centre a door, or an enemy that is "
             "off-centre to the right (bearing +9 to +18). NOT when aim.on_target "
             "(bearing within +/-8): the shot already lands, so hold and fire "
             "instead - a turn overshoots."),
    "turn_around": dict(
        action={"turn": "right"}, tics=turn_tics(180),
        desc="Turn about 180 degrees. ONLY when `way_out.dead_end` is true (every "
             "direction in view is a near wall) and you did not just turn_around. If "
             "any direction is open (`way_out.dist` is large), turn toward "
             "`way_out.bearing` instead - turning around twice just spins you back."),
    "back": dict(
        action={"move": "back"}, tics=8,
        desc="Step backwards. Use when a melee enemy (pinky, imp, zombieman) "
             "is very close (< 120) to keep it at gun range, or to back out "
             "of a corner while still facing the threat."),
    "strafe_left": dict(
        action={"strafe": "left"}, tics=8,
        desc="Sidestep left without turning: dodge, or line up with a door or "
             "opening that is a little to the left (bearing -5 to -20) while "
             "keeping the view straight."),
    "strafe_right": dict(
        action={"strafe": "right"}, tics=8,
        desc="Sidestep right without turning: dodge, or line up with a door or "
             "opening that is a little to the right (bearing +5 to +20) while "
             "keeping the view straight."),
    "use": dict(
        action={"use": True}, tics=3,
        desc="Press USE: opens the door in front, lowers a lift (door_ahead.kind "
             "`lift`), flips a switch. USE only "
             "reaches about 64 units, so it is right when door_ahead is very "
             "close (dist <= 60) and roughly centred (|bearing| <= 20); if the door "
             "is farther, walk up to it first. Again when door_ahead is still there "
             "with tics_since_press >= 50 (the press did not take); never while "
             "tics_since_press < 50 (it would send the rising door back down), never "
             "on a `wont_open` door. Also try it when blocked straight ahead by a "
             "flat wall very close (a hidden door or switch)."),
    "wait": dict(
        action={}, tics=8,
        desc="Stand still for a moment (FIRE still works while waiting). Best when "
             "aim.on_target: hold your heading and shoot, do not turn. Right after "
             "`use` on a door, while door_ahead.tics_since_press < 50: doors take "
             "about a second to rise, walking into them is what gets you `blocked`. "
             "Also to let an approaching enemy come into your sights. Wrong when "
             "progress.going_nowhere and no door is rising and no enemy on target."),
}

MACRO_NAMES = list(MACROS.keys())


# ----------------------------------------------------------------------------
# Device link (the firmware's /agent WebSocket)
# ----------------------------------------------------------------------------

class DeviceLink:
    """One persistent lockstep connection to ws://<device>/agent."""

    def __init__(self, url, timeout=6.0):
        self.url = url
        self.timeout = timeout
        self.ws = None

    def connect(self):
        try:  # websockets >= 15 wants legacy=True for a bare (non-context-manager) connection
            self.ws = ws_connect(self.url, open_timeout=10, close_timeout=2, legacy=True)
        except TypeError:
            self.ws = ws_connect(self.url, open_timeout=10, close_timeout=2)

    def _rpc(self, payload):
        t0 = time.perf_counter()
        self.ws.send(json.dumps(payload))
        raw = self.ws.recv(timeout=self.timeout)
        return json.loads(raw), time.perf_counter() - t0

    def step(self, action):
        return self._rpc(action)

    def observe(self):
        return self._rpc({"tics": 0})

    def get_map(self):
        return self._rpc({"request": "map"})

    def close(self):
        if self.ws is not None:
            try:
                self.ws.close()
            except Exception:
                pass
            self.ws = None


# ----------------------------------------------------------------------------
# Observation compression (what Jev gets to read)
# ----------------------------------------------------------------------------

SECTORS = [
    ("far_left", -45, -33),
    ("left", -33, -19),
    ("centre_left", -19, -6),
    ("ahead", -6, 6),
    ("centre_right", 6, 19),
    ("right", 19, 33),
    ("far_right", 33, 45),
]

STATE_LEGEND = (
    "Units: Doom map units (the marine is 32 wide, a door about 128 wide; a "
    "room is 500-1500 across). Bearings are degrees from where you face: "
    "0 = straight ahead, negative = LEFT, positive = RIGHT; the view spans "
    "-45..45. `view.sectors` is the depth of the rendered view: min_dist is the "
    "nearest wall in that sector (2048 = nothing within scan range). "
    "`view.ahead_min_dist` is how far you can actually walk straight forward before "
    "hitting something - a wall OR a barrel in your path (barrels are not walls, so "
    "a sector can read 'clear' while a barrel blocks you): when it is small, forward "
    "is blocked whatever the sectors say. "
    "Depths toward a direction you already tried to walk from here and did not move "
    "(a step too high, a ledge, a pillar or a corner the depth rays cannot show) are "
    "lowered to ~32: `view.ahead_unwalkable` = straight ahead is such a direction; do "
    "not walk into it again, turn to another open direction. "
    "Floors: a sector's `climb` = the floor rises that many units that way = stairs or "
    "a ramp up, and it IS walkable (just walk forward, the marine climbs steps by "
    "itself); a step too high to climb is reported as a wall. A sector's `hurt_dist` / "
    "`view.ahead_hurt_dist` = a damaging floor (nukage, slime, lava) begins that far that "
    "way: walking onto it costs health every second - do not walk onto it unless it is "
    "the only way on and player.radsuit_secs is present (radiation suit) or the hurting "
    "stretch is short and health is high. `floor` (present only while you STAND on a "
    "damaging floor) = {hurts: damage per second, radsuit, way_off}: get off it NOW by "
    "the shortest way - turn toward `floor.way_off.bearing` and walk forward; if way_off "
    "is null no safe floor is in view: turn (big turns, same direction) until it shows. "
    "Do not stand still, wait or fight from a damaging floor unless radsuit is true. "
    "`view.rays` (present near a door or when you are stuck) is the full depth fan: "
    "51 rays across -45..+45 degrees (~1.8 deg apart), each {b: bearing, d: wall "
    "distance}; use it to line up precisely on a narrow doorway or opening that the 7 "
    "sectors are too coarse to place. "
    "`view.openings` are rays much longer than their neighbours = a passage or "
    "doorway. `door_ahead` is a closed door/passage in view (USE reaches only "
    "~64 units: walk up to it, press `use` when dist <= 60 and roughly centred, "
    "then wait; it re-appears if the door re-closed). `door_ahead.presses` / "
    "`tics_since_press` (present once you pressed USE on this door; 35 tics = 1 s): a "
    "door that opens is gone from door_ahead ~28 tics after the press, so with "
    "tics_since_press < 50 it is still rising (wait, do not turn away, do not press "
    "again: a press while it rises sends it back down); still there with "
    "tics_since_press >= 50 = the press did not take: press `use` again. "
    "`door_ahead.wont_open` = pressed 3 times and it stayed shut (locked: needs a "
    "key, or not a real door): treat it as a wall, do NOT walk up to it again, "
    "explore elsewhere. `door_ahead.kind` = `lift`: a raised lift platform (a ledge "
    "too high to climb); it works like a door: walk up to it, `use` it when dist <= 60, "
    "it goes down (gone from door_ahead), then walk forward ONTO it at once and `wait` "
    "there: it rises back up after ~3 s carrying you to the upper level. "
    "`progress.moved_last_10_steps` = how far you got over the last 10 decisions; "
    "`progress.going_nowhere` = under 48 units: unless you are waiting for a door "
    "you just pressed, you are stuck (dithering, spinning, waiting, or pushing into "
    "something): change what you are doing. `dithering` = your last macros "
    "alternate between a move and its opposite (left/right, forward/back). "
    "`enemies_in_view` are monsters. `aim` (present when a monster is in view) is the "
    "nearest one and `aim.on_target` = it is already inside your gun's hit cone with "
    "ammo left: FIRE and hold your heading (`wait`), do NOT turn — a shot lands "
    "anywhere in the cone, and because decisions lag ~0.7 s a turn to 'centre better' "
    "acts on an old bearing and overshoots, which is what makes you swing past the "
    "enemy again and again. Only turn when the enemy is outside the cone. "
    "`hazards_in_view` are exploding barrels: "
    "obstacles, NOT targets - never turn to centre one or walk into it, go around "
    "it (a shot barrel hurts everything within ~128 units, you included). "
    "`blocked_last_move` = the last move made no progress (wall, shut door, or "
    "an obstacle). `way_out` is the most open direction in view {bearing, dist, side}: "
    "when you are stuck (blocked or progress.going_nowhere) with no enemy/door, make a "
    "single turn toward `way_out.bearing` and then go `forward` - do NOT keep turning "
    "and do NOT `turn_around` unless `way_out.dead_end` is true (every direction in "
    "view is a near wall). Never `turn_around` twice in a row: if you just did "
    "(recent_actions ends in turn_around) and you are still boxed in, pick "
    "`way_out.side` instead - a second turn_around only spins you back where you were. "
    "`best_seen` (present when you are stuck and have already looked around from this "
    "spot) is the most open direction you SAW in the last turns, including directions "
    "now behind you {heading, dist, bearing from where you face now, side}: the view "
    "only spans +/-45, so turning back and forth to 'look again' just re-discovers it. "
    "Turn toward best_seen.bearing and then go: a big turn (turn_left / turn_right) is "
    "~34 deg, so use it only while |bearing| > 25; with |bearing| 10-25 use "
    "forward_left / forward_right (walk while curving toward it) or a small turn - a "
    "big turn there overshoots to the other side and the next one brings you back, "
    "forever; with |bearing| <= 10 and ahead_min_dist clear, `forward`. Never undo the "
    "turn you just made (any left turn right after a right turn, or vice versa, while "
    "standing still): that is replaced by a curving walk (forward_left / "
    "forward_right) or by more turning the same way. "
    "`heading` is a compass heading, 0 = north, 90 = east. "
    "`commander` (when present) is the strategic layer's current intent: "
    "`turn_hint` says which way to turn to get on the requested heading. It is a "
    "direction to drift toward, not a path: never turn from an open path into a near "
    "wall because of it. `turn_hint` = `blocked` means you have looked that way from "
    "here and it is a wall (`target_heading_open_dist`): detour through an opening or "
    "best_seen instead; `paused` = you are stuck, get unstuck first and ignore the "
    "heading for now."
)


MAP_LEGEND = ("north up, '#' = wall (or closed door / ledge too high), ':' = walkable step "
              "edge (stairs), '~' = edge of a damaging floor, '@' = you, ' ' = unknown/open")


def summarize_walls(walls):
    """Collapse the 51-ray fan into 7 sectors plus notable openings."""
    sectors = []
    for name, lo, hi in SECTORS:
        rays = [w for w in walls if lo <= w["bearing"] <= hi] if lo == -45 else \
               [w for w in walls if lo < w["bearing"] <= hi]
        if not rays:
            sectors.append({"sector": name, "min_dist": RAY_RANGE, "max_dist": RAY_RANGE})
            continue
        dists = [w["dist"] for w in rays]
        s = {"sector": name, "min_dist": min(dists), "max_dist": max(dists)}
        # floor facts from the firmware (absent on older firmware / the simulator)
        climb = max((w.get("rise", 0) for w in rays), default=0)
        if climb >= FLOOR_CLIMB_MIN:
            s["climb"] = climb                  # stairs / ramp up that way (walkable)
        hurt = [w["hurt"] for w in rays if w.get("hurt") is not None and w["hurt"] >= 0]
        if hurt:
            s["hurt_dist"] = min(hurt)          # a damaging floor begins this far that way
        sectors.append(s)

    openings = []
    n = len(walls)
    for i, w in enumerate(walls):
        neigh = [walls[j]["dist"] for j in (i - 3, i - 2, i + 2, i + 3) if 0 <= j < n]
        if not neigh:
            continue
        ref = sorted(neigh)[len(neigh) // 2]
        if w["dist"] >= 400 and w["dist"] >= 1.8 * max(ref, 1):
            openings.append({"bearing": w["bearing"], "dist": w["dist"]})
    # merge adjacent bearings into one opening (report the centre)
    merged = []
    for o in openings:
        if merged and abs(o["bearing"] - merged[-1]["bearings"][-1]) <= 4:
            merged[-1]["bearings"].append(o["bearing"])
            merged[-1]["dist"] = max(merged[-1]["dist"], o["dist"])
        else:
            merged.append({"bearings": [o["bearing"]], "dist": o["dist"]})
    openings = [{"bearing": round(sum(m["bearings"]) / len(m["bearings"])),
                 "dist": m["dist"]} for m in merged][:4]

    ahead = [w["dist"] for w in walls if -6 <= w["bearing"] <= 6]
    return sectors, openings, (min(ahead) if ahead else RAY_RANGE)


def map_window(map_json, rows=9, cols=25):
    """Crop the ASCII automap around the player (north up)."""
    if not map_json or not map_json.get("valid"):
        return None
    grid = map_json.get("grid", [])
    p = map_json.get("player", {})
    pr, pc = p.get("row", 0), p.get("col", 0)
    out = []
    for r in range(pr - rows // 2, pr + rows // 2 + 1):
        line = ""
        for c in range(pc - cols // 2, pc + cols // 2 + 1):
            if 0 <= r < len(grid) and 0 <= c < len(grid[r]):
                line += grid[r][c]
            else:
                line += " "
        out.append(line)
    return out


def map_coverage(map_json):
    if not map_json or not map_json.get("valid"):
        return 0
    return sum(row.count("#") + row.count(":") + row.count("~") for row in map_json.get("grid", []))


def heading_error(current, target):
    """Signed degrees from `current` to `target` compass headings; + = turn right."""
    return ((target - current + 540) % 360) - 180


# ----------------------------------------------------------------------------
# Short-term memory (what the reflex layer remembers between steps)
# ----------------------------------------------------------------------------

class Memory:
    def __init__(self):
        self.recent = collections.deque(maxlen=8)      # macro names
        self.consecutive_blocked = 0
        self.same_streak = 0
        self.steps = 0
        self.game_tics = 0
        self.positions = collections.deque(maxlen=400)  # (t, x, y, hp)
        self.hp_history = collections.deque(maxlen=400)
        self.coverage_history = collections.deque(maxlen=400)  # (t, cells)
        self.blocked_total = 0
        self.fallbacks = 0
        self.heuristic_overrides = 0
        self.uses = 0
        self.doors_seen = 0
        self.last_use_step = -100
        self.last_use_tic = None
        self.enemies_seen = collections.Counter()
        self.last_weapon_applied = None
        self.jev_latency = collections.deque(maxlen=200)
        self.jev_tokens = 0
        self.jev_calls = 0
        self.jev_errors = 0
        self.blocked_window = collections.deque(maxlen=20)   # blocked flag of the last 20 steps
        self.deaths = 0
        self.levels = []            # "E1M1", ... in the order they were played
        self.frozen_steps = 0       # consecutive steps where nothing at all changed
        self.unfreeze_presses = 0
        self.reconnects = 0
        self.device_latency = collections.deque(maxlen=20)
        self.last_stall_step = -1000    # step of the last stall-net override (cooldown)
        self.door_work = None     # the door last pressed with USE in reach: {"x","y","level","tries","tic"}
        self.dead_doors = []      # doors still shut after DOOR_MAX_TRIES presses: {"x","y","tic","level"}
        # Panorama: depth seen in each compass direction from (about) the current spot,
        # across steps. The view is only +/-45 deg, so a marine turning in place to look
        # for a way out forgets what was behind it after every turn and sweeps
        # left-right forever; this lets it remember "heading 60 was open for 700 units".
        self.pano = {}            # compass bin -> nearest depth seen there (wall or barrel)
        self.pano_anchor = None   # (x, y) the panorama was taken from
        # Walk blocks: compass headings where a straight forward move came back `blocked`
        # although the depth fan looked open (a step too high, a ledge, a solid
        # decoration, a corner caught by the marine's 32-unit body - none of which the
        # wall rays show). Without this the view kept saying "465 units open" in that
        # direction, best_seen / way_out pointed right back at it, and Jev walked into
        # the same invisible obstacle forever (turn away, turn back, forward, BLOCKED).
        self.walk_blocks = []     # {"x","y","heading","level","step"}

    def note_walk_block(self, macro, obs):
        """Remember the heading of a forward move that went nowhere."""
        if not obs.get("valid") or not obs.get("blocked") or macro not in FORWARD_MOVES:
            return
        for e in hostiles(obs.get("visible", [])):    # a monster in the way is not terrain
            if abs(e["bearing"]) <= 25 and e["dist"] <= 200:
                return
        lvl = (obs.get("episode"), obs.get("map"))
        self.walk_blocks = [b for b in self.walk_blocks
                            if not (b["level"] == lvl and abs(heading_error(b["heading"], obs["angle"])) < 3
                                    and math.hypot(b["x"] - obs["x"], b["y"] - obs["y"]) < 16)]
        self.walk_blocks.append({"x": obs["x"], "y": obs["y"], "heading": obs["angle"],
                                 "level": lvl, "step": self.steps})
        del self.walk_blocks[:-WALK_BLOCK_KEEP]

    def mask_walk_blocks(self, obs):
        """Lower the ray depths toward headings already found unwalkable from about
        here, in place, so the view, sectors, openings, way_out and the panorama all
        agree with what walking there actually did. Sets obs['walk_blocked_ahead']."""
        obs["walk_blocked_ahead"] = False
        if not obs.get("valid") or not obs.get("walls") or not self.walk_blocks:
            return
        lvl = (obs.get("episode"), obs.get("map"))
        self.walk_blocks = [b for b in self.walk_blocks if self.steps - b["step"] <= WALK_BLOCK_STEPS]
        near = [b for b in self.walk_blocks
                if b["level"] == lvl and math.hypot(b["x"] - obs["x"], b["y"] - obs["y"]) <= WALK_BLOCK_RADIUS]
        if not near:
            return
        for w in obs["walls"]:
            h = obs["angle"] + w["bearing"]
            if any(abs(heading_error(b["heading"], h)) <= WALK_BLOCK_HALF for b in near):
                w["dist"] = min(w["dist"], WALK_BLOCK_DIST)
                if abs(w["bearing"]) <= 6:
                    obs["walk_blocked_ahead"] = True

    def note_view(self, obs):
        """Fold this frame's depth fan into the panorama (reset after moving away)."""
        if not obs.get("valid") or not obs.get("walls"):
            return
        x, y = obs["x"], obs["y"]
        if self.pano_anchor is None or math.hypot(x - self.pano_anchor[0], y - self.pano_anchor[1]) > PANO_RESET_DIST:
            self.pano = {}
            self.pano_anchor = (x, y)
        frame = {}
        for w in obs["walls"]:
            b = int(((obs["angle"] + w["bearing"]) % 360) // PANO_BIN)
            frame[b] = min(frame.get(b, RAY_RANGE), w["dist"])
        for t in obs.get("visible", []):      # a barrel blocks its direction too
            if t["name"] in NON_HOSTILE:
                b = int(((obs["angle"] + t["bearing"]) % 360) // PANO_BIN)
                if b in frame:
                    frame[b] = min(frame[b], t["dist"])
        self.pano.update(frame)               # latest look wins per direction

    def pano_depth(self, heading):
        return self.pano.get(int((heading % 360) // PANO_BIN))

    def best_seen(self, obs):
        """The most open direction already looked at from here, relative to now; None
        until the panorama covers more than the current view."""
        if len(self.pano) * PANO_BIN <= 100:
            return None
        b, d = max(self.pano.items(), key=lambda kv: kv[1])
        heading = b * PANO_BIN + PANO_BIN / 2
        rel = round(heading_error(obs["angle"], heading))
        side = "ahead" if abs(rel) <= 10 else ("left" if rel < 0 else "right")
        return {"heading": round(heading), "dist": d, "bearing": rel, "side": side,
                "looked_around_deg": len(self.pano) * PANO_BIN}

    def note_step(self, macro, obs, latency, usage):
        self.steps += 1
        if self.recent and self.recent[-1] == macro:
            self.same_streak += 1
        else:
            self.same_streak = 1
        self.recent.append(macro)
        self.note_walk_block(macro, obs)
        self.mask_walk_blocks(obs)
        self.note_view(obs)
        if obs.get("valid"):
            now = time.time()
            self.positions.append((now, obs["x"], obs["y"], obs["health"]))
            self.hp_history.append((now, obs["health"]))
            if obs.get("blocked"):
                self.consecutive_blocked += 1
                self.blocked_total += 1
            else:
                self.consecutive_blocked = 0
            self.blocked_window.append(bool(obs.get("blocked")))
            if obs.get("door_ahead"):
                self.doors_seen += 1
            for e in obs.get("visible", []):
                self.enemies_seen[e["name"]] += 1
        if macro == "use":
            self.uses += 1
            self.last_use_step = self.steps
        if latency is not None:
            self.jev_latency.append(latency)
        if usage:
            self.jev_tokens += int(usage.get("input_tokens", 0) or 0)

    def distance_travelled(self, seconds):
        cutoff = time.time() - seconds
        pts = [p for p in self.positions if p[0] >= cutoff]
        if len(pts) < 2:
            return 0.0
        d = 0.0
        for a, b in zip(pts, pts[1:]):
            d += math.hypot(b[1] - a[1], b[2] - a[2])
        return d

    def net_displacement(self, steps):
        """Straight-line distance between the positions `steps` steps apart (None
        until there are that many)."""
        if steps <= 0 or len(self.positions) < steps:
            return None
        pts = list(self.positions)[-steps:]
        return math.hypot(pts[-1][1] - pts[0][1], pts[-1][2] - pts[0][2])

    def stalled(self, steps, radius):
        """True when the marine's net displacement over the last `steps` steps is
        under `radius` units: dithering, spinning or pushing in place."""
        d = self.net_displacement(steps)
        return d is not None and d < radius

    def coverage_growth(self, seconds):
        cutoff = time.time() - seconds
        pts = [c for c in self.coverage_history if c[0] >= cutoff]
        if len(pts) < 2:
            return None
        return pts[-1][1] - pts[0][1]

    def hp_drop(self, seconds):
        cutoff = time.time() - seconds
        pts = [h for h in self.hp_history if h[0] >= cutoff]
        if len(pts) < 2:
            return 0
        return max(h[1] for h in pts) - pts[-1][1]


# ----------------------------------------------------------------------------
# Intent: the System 2 -> System 1 interface
# ----------------------------------------------------------------------------

class Intent:
    def __init__(self, goal, target_heading=None, fire_policy="normal",
                 preferred_weapon=None, note="", source="default"):
        self.goal = goal
        self.target_heading = target_heading
        self.fire_policy = fire_policy
        self.preferred_weapon = preferred_weapon
        self.note = note
        self.source = source
        self.created = time.time()

    def to_state(self, heading, stuck=False, target_depth=None):
        """`target_depth`: how far the marine has seen it can walk on the target heading
        from here (None = not looked that way yet). A heading is a compass direction, not
        a path: from inside a room it often points straight into a wall. Then (and while
        stuck) the hint must not keep pulling the marine back to face that wall - that
        is the turn-to-wall / turn-to-open / turn-to-wall sweep."""
        d = {"goal": self.goal, "fire_policy": self.fire_policy}
        if self.target_heading is not None and heading is not None:
            err = heading_error(heading, self.target_heading)
            d["target_heading"] = self.target_heading
            d["heading_error_deg"] = err
            if target_depth is not None:
                d["target_heading_open_dist"] = target_depth
            if target_depth is not None and target_depth < TARGET_OPEN:
                d["turn_hint"] = "blocked"      # wall that way: detour through an opening
            elif stuck:
                d["turn_hint"] = "paused"       # get unstuck first (best_seen / way_out)
            else:
                d["turn_hint"] = "on_course" if abs(err) <= 12 else ("right" if err > 0 else "left")
        if self.note:
            d["note"] = self.note
        return d


DEFAULT_INTENT = Intent(
    goal="Explore the level to find the exit: keep moving into unexplored space, "
         "open doors, kill enemies that are in your sights, avoid walking into walls.",
    source="default")


# ----------------------------------------------------------------------------
# Jev (System One) client
# ----------------------------------------------------------------------------

class JevError(Exception):
    pass


class JevFatal(JevError):
    """An error retrying cannot fix: bad key (401/403) or a request the API rejects (4xx)."""


def build_questions(intent):
    fire_true = ("An enemy in enemies_in_view is roughly centred (|bearing| <= 10) "
                 "and within 1500 units, and ammo is not 0.")
    fire_false = ("No enemy in enemies_in_view is centred, a barrel in hazards_in_view "
                  "is centred and closer than 200 units (its blast would hurt you), "
                  "or ammo is 0.")
    if intent.fire_policy == "aggressive":
        fire_true = ("An enemy is anywhere near the centre (|bearing| <= 16) and "
                     "within 2000 units, and ammo is not 0. Shoot early.")
    elif intent.fire_policy == "conservative":
        fire_true = ("An enemy is well centred (|bearing| <= 6) and within 900 units, "
                     "and ammo is not 0. Save ammo otherwise.")
    return {
        "action": {
            "type": "choice",
            "instructions": (
                "You are the reflexes of the DOOM marine. Pick the ONE macro to run for "
                "the next ~0.3 s given the state. Priorities: (0) if `floor` is present "
                "(you stand on a damaging floor) and floor.radsuit is false, getting off it "
                "comes first: turn toward floor.way_off and walk forward, shooting only "
                "what is already in your sights; (1) do not repeat a macro "
                "that was just `blocked_last_move` — change direction instead; "
                "(2) if `aim.on_target`, do NOT turn: `wait` (hold heading) and let "
                "FIRE do the work - turning here overshoots (lag). If an enemy is in "
                "view but off-centre, turn to centre it (small turn when close to "
                "centred), keep distance from "
                "melee enemies closer than 120; barrels (hazards_in_view) are NOT "
                "enemies: never turn to centre one, and if one is right ahead, sidestep "
                "or turn to go around it; (3) if door_ahead is present and not "
                "`wont_open` (a wont_open door is a wall: turn away from it), walk up to "
                "it (forward / small turns to centre it) until dist <= 60, then `use`; "
                "while door_ahead.tics_since_press < 50 `wait` facing it; if it is still "
                "there at tics_since_press >= 50, `use` again; once door_ahead is gone the "
                "door is open: walk through it (`forward`); (4) if progress.going_nowhere "
                "or dithering, and no enemy in view and no door rising: do something "
                "different - turn toward `best_seen` if present (else the longest open "
                "sector or the best `opening`) and then go forward; keep turning the SAME "
                "way until it is ahead, never reverse mid-turn; (5) otherwise make "
                "progress toward the commander goal / turn_hint (only `left` / `right`; "
                "ignore `blocked` / `paused`), else toward the longest open sector or "
                "the best `opening`; (6) prefer forward motion when `ahead_min_dist` is "
                "clear (>= 300): walking an open path beats turning toward a heading. "
                "Avoid oscillating left/right (see recent_actions): a run of turns one "
                "way followed by a run the other way, standing still, is the loop to avoid."),
            "criteria": {name: m["desc"] for name, m in MACROS.items()},
        },
        "fire": {
            "type": "noul",
            "instructions": "Should the marine hold the FIRE button during this step?",
            "criteria": {"true": fire_true, "false": fire_false},
        },
    }


class JevClient:
    """Minimal HTTP client for POST /v1/systemone (no SDK dependency)."""

    def __init__(self, url, api_key, model=JEV_DEFAULT_MODEL, timeout=2.0,
                 user_agent=JEV_DEFAULT_USER_AGENT):
        if not api_key:
            raise JevError("no TypeSafe API key: set TYPESAFE_API_KEY or use --jev-key / --jev dry")
        self.url = url
        self.api_key = api_key
        self.model = model
        self.timeout = timeout
        self.user_agent = user_agent
        self.name = f"jev:{model}"

    def decide(self, state, questions):
        body = json.dumps({"model": self.model, "state": state, "questions": questions}).encode()
        req = urllib.request.Request(
            self.url, data=body, method="POST",
            headers={"Authorization": f"Bearer {self.api_key}",
                     "Content-Type": "application/json",
                     "Accept": "application/json",
                     "User-Agent": self.user_agent,
                     "Accept-Language": "en-US,en;q=0.9"})
        last_err = None
        for attempt in range(2):
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    data = json.loads(resp.read().decode())
                answers = data.get("answers") if isinstance(data, dict) else None
                if not isinstance(answers, dict) or "action" not in answers:
                    raise JevError(f"unexpected response: {json.dumps(data)[:200]}")
                return answers, data.get("usage") or {}
            except urllib.error.HTTPError as e:
                detail = ""
                try:
                    detail = e.read().decode()[:300]
                except Exception:
                    pass
                if e.code in (429, 529) and attempt == 0:
                    time.sleep(0.3)
                    continue
                if e.code == 403 and ("1010" in detail or "cloudflare" in detail.lower()):
                    raise JevFatal(
                        "HTTP 403 Cloudflare (error 1010): the request was blocked by bot "
                        "protection in front of the API, not by your key. This client already "
                        "sends a browser User-Agent; if it still fails the block is on the TLS "
                        "fingerprint. Try --jev-user-agent \"...\", or reach the API through a "
                        "browser-like client / proxy.")
                if 400 <= e.code < 500 and e.code != 429:
                    raise JevFatal(f"HTTP {e.code}: {detail or e.reason}")
                raise JevError(f"HTTP {e.code}: {detail or e.reason}")
            except (ValueError, http.client.HTTPException) as e:   # truncated / non-JSON body (proxy page...)
                raise JevError(f"bad response: {e!r}")
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                last_err = JevError(f"network: {e}")
                if attempt == 0 and not isinstance(e, TimeoutError) and "timed out" not in str(e):
                    continue            # retry a refused/reset connection once, not a timeout
                raise last_err
        raise last_err or JevError("no answer")  # pragma: no cover


class DryJev:
    """A stand-in for Jev with the same answer format: a transparent local
    policy, for testing the loop without an API key (and as a fallback)."""

    name = "dry"

    def decide(self, state, questions):
        view = state["view"]
        sect = {s["sector"]: s["min_dist"] for s in view["sectors"]}
        ahead = view.get("ahead_min_dist", sect["ahead"])   # walkable clearance (wall or barrel)
        enemies = state.get("enemies_in_view", [])
        door = state.get("door_ahead")
        if door and door.get("wont_open"):
            door = None                 # a door that did not open is just a wall
        recent = state.get("recent_actions", [])
        blocked = state.get("blocked_last_move", False)
        n_blocked = state.get("consecutive_blocked", 0)
        cmd = state.get("commander", {})
        hint = cmd.get("turn_hint")

        fire = 0.05
        choice = None
        centred = [e for e in enemies if abs(e["bearing"]) <= 10 and e["name"] != "barrel"]
        if centred and state["player"]["ammo"] != 0:
            fire = 0.9
        targets = [e for e in enemies if e["name"] != "barrel"]
        fl = state.get("floor")
        if fl and not fl.get("radsuit"):
            # standing on nukage/slime: getting off it beats everything else
            wo = fl.get("way_off")
            if wo is None:
                choice = "turn_right"            # no safe floor in view: keep looking, one way
            elif abs(wo["bearing"]) <= 10:
                choice = "forward"
            elif abs(wo["bearing"]) <= 25:
                choice = "forward_left" if wo["bearing"] < 0 else "forward_right"
            else:
                choice = "turn_left" if wo["bearing"] < 0 else "turn_right"
        elif targets:
            e = min(targets, key=lambda e: e["dist"])
            if abs(e["bearing"]) > 18:
                choice = "turn_left" if e["bearing"] < 0 else "turn_right"
            elif abs(e["bearing"]) > AIM_CONE:
                choice = "turn_left_small" if e["bearing"] < 0 else "turn_right_small"
            elif e["dist"] < 120:
                choice = "back"          # centred but in melee range: back off, keep firing
            elif e["dist"] <= AIM_HOLD_DIST:
                choice = "wait"          # centred and in range: hold and fire (a turn overshoots)
            else:
                choice = "forward"       # centred but far: close in, firing while advancing
        elif door and abs(door["bearing"]) <= 25 and (door["dist"] <= 60 or n_blocked < 2):
            # (a far door behind something that keeps blocking us: the rules below)
            if door["dist"] <= 60:
                rising = door.get("presses") and door.get("tics_since_press", 0) < 50
                choice = "wait" if rising or (recent and recent[-1] == "use") else "use"
            elif abs(door["bearing"]) > 8 and door["dist"] < 300:
                choice = "turn_left_small" if door["bearing"] < 0 else "turn_right_small"
            else:
                choice = "forward"
        elif state.get("progress", {}).get("going_nowhere") or state.get("dithering") or n_blocked >= 2:
            # stuck: head for the most open direction already seen from here (best_seen,
            # may be behind us), else the most open one in view (way_out). turn_around
            # only for a real dead end, and never twice in a row (that just spins back).
            wo = state.get("best_seen") or state.get("way_out") or {}
            b = wo.get("bearing", 0)
            just_turned = recent and recent[-1] == "turn_around"
            if "looked_around_deg" in wo and wo.get("dist", 0) >= 200:
                if abs(b) <= 10 and ahead >= 120 and not blocked:
                    choice = "forward"
                elif abs(b) <= 18:
                    choice = "turn_left_small" if b < 0 else "turn_right_small"
                elif abs(b) >= 150 and not just_turned:
                    choice = "turn_around"
                else:
                    choice = "turn_left" if b < 0 else "turn_right"
            elif wo.get("dead_end") and not just_turned:
                choice = "turn_around"
            elif wo.get("side") == "ahead" and ahead >= 120 and not blocked:
                choice = "forward"
            elif abs(b) <= 18:
                choice = "turn_left_small" if b < 0 else "turn_right_small"
            else:
                choice = "turn_left" if b < 0 else "turn_right"
        elif hint in ("left", "right") and ahead < 400:
            choice = "turn_" + hint
        elif ahead >= 600 and not blocked:
            choice = "forward_long"
        elif ahead >= 120 and not blocked:
            if hint == "left" and sect["centre_left"] > 250:
                choice = "forward_left"
            elif hint == "right" and sect["centre_right"] > 250:
                choice = "forward_right"
            else:
                choice = "forward"
        else:
            left = max(sect["left"], sect["far_left"], sect["centre_left"])
            right = max(sect["right"], sect["far_right"], sect["centre_right"])
            if view.get("openings"):
                o = view["openings"][0]
                choice = "turn_left" if o["bearing"] < 0 else "turn_right"
            else:
                choice = "turn_left" if left > right else "turn_right"

        probs = {n: 0.4 / (len(MACRO_NAMES) - 1) for n in MACRO_NAMES}
        probs[choice] = 0.6
        return ({"action": {"type": "choice", "choice": choice, "probabilities": probs, "confidence": 0.6},
                 "fire": {"type": "noul", "noul": fire}},
                {"input_tokens": 0})


# ----------------------------------------------------------------------------
# State builder
# ----------------------------------------------------------------------------

NON_HOSTILE = {"barrel"}   # shootable things the firmware reports that are obstacles, not targets


def hostiles(visible):
    """The things in view that are actual enemies (barrels are obstacles, not targets)."""
    return [e for e in visible if e["name"] not in NON_HOSTILE]


# Door memory. Jev sees one moment at a time, so the agent remembers the door it
# pressed USE on and tells Jev (door_ahead.presses / tics_since_press / wont_open).
# A working Doom door is passable (and door_ahead gone) ~28 tics after a USE, so a
# door still shut DOOR_RETRY_TICS after a press in reach did not open. A press does
# not always take (seen on the board: the first one, still sliding in, did nothing;
# the second opened it), so press again. After DOOR_MAX_TRIES it is locked (needs a
# key) or not a real door (window, ledge): remembered for DEAD_DOOR_TICS.
DOOR_REACH = 64            # USE reaches this far (map units)
DOOR_PRESS_DIST = 80       # a press from up to here counts: the marine's momentum carries it into reach
DOOR_SAME = 64             # two door sightings this close are the same door
DOOR_RETRY_TICS = 50       # > 28 with a margin: pressing while it still rises would send it back down
DOOR_MAX_TRIES = 3
DOOR_FORGET_TICS = 10 * TICRATE  # a door pressed this long ago, or DOOR_FORGET_DIST away, is not "being worked"
DOOR_FORGET_DIST = 256
DEAD_DOOR_TICS = 120 * TICRATE   # then it gets another chance

# Progress, as told to Jev: net displacement over the last PROGRESS_STEPS decisions;
# under GOING_NOWHERE units the marine is going nowhere (dithering, spinning, waiting,
# pushing in place - turning in place is never `blocked`).
PROGRESS_STEPS = 10
GOING_NOWHERE = 48

# Panorama (Memory.note_view): compass bins of PANO_BIN deg, forgotten after moving
# PANO_RESET_DIST units. A direction counts as walkable toward the commander's heading
# only if it is open at least TARGET_OPEN units.
PANO_BIN = 10
PANO_RESET_DIST = 64
TARGET_OPEN = 160

# Walk blocks (Memory.note_walk_block): a forward move that came back `blocked` marks
# its heading unwalkable within WALK_BLOCK_RADIUS units of that spot (+/-WALK_BLOCK_HALF
# deg, depth lowered to WALK_BLOCK_DIST) for WALK_BLOCK_STEPS decisions.
WALK_BLOCK_RADIUS = 48
WALK_BLOCK_HALF = 12
WALK_BLOCK_DIST = 32
WALK_BLOCK_STEPS = 150
WALK_BLOCK_KEEP = 24

# Floor facts (firmware rays' rise / hurt / safe, obs floor_damage / radsuit_tics):
# a rise below FLOOR_CLIMB_MIN is noise (a lip, a raised floor patch), not stairs.
FLOOR_CLIMB_MIN = 16


def floor_of(obs):
    """`floor` block for the state: present only when the marine stands on a damaging
    floor. `way_off` = nearest safe floor in view (None = none in view: turn to look),
    using the rays' `safe` distance."""
    dmg = obs.get("floor_damage", 0) or 0
    if dmg <= 0:
        return None
    suit = (obs.get("radsuit_tics", 0) or 0) > 0
    exits = [w for w in obs.get("walls", []) if w.get("safe") is not None and w["safe"] >= 0]
    way_off = None
    if exits:
        w = min(exits, key=lambda w: (w["safe"], abs(w["bearing"])))
        side = "ahead" if abs(w["bearing"]) <= 10 else ("left" if w["bearing"] < 0 else "right")
        way_off = {"bearing": round(w["bearing"]), "dist": round(w["safe"]), "side": side}
    return {"hurts": dmg, "radsuit": suit, "way_off": way_off}


def hurt_ahead(obs, bearing=6):
    """Distance straight ahead to where a damaging floor begins (None = none)."""
    d = [w["hurt"] for w in obs.get("walls", [])
         if abs(w["bearing"]) <= bearing and w.get("hurt") is not None and w["hurt"] >= 0]
    return min(d) if d else None

# Aiming: a hitscan shot lands anywhere within this bearing cone, and at ~700 ms Jev
# latency a turn to "centre better" acts on a stale bearing and overshoots. So once the
# enemy is centred AND close enough to be worth engaging, hold and fire instead of
# turning. Centred but far: keep moving (you fire while advancing) rather than freezing.
AIM_CONE = 8
AIM_HOLD_DIST = 700


def level_of(obs):
    return f"E{obs.get('episode')}M{obs.get('map')}"


def door_world_pos(obs):
    """Map coordinates (x east, y north) of obs['door_ahead']: heading is a compass
    angle and bearing + = right = clockwise, so the door lies at heading + bearing."""
    door = obs.get("door_ahead")
    if not door or not obs.get("valid"):
        return None
    a = math.radians(obs["angle"] + door["bearing"])
    return obs["x"] + door["dist"] * math.sin(a), obs["y"] + door["dist"] * math.cos(a)


def _near(p, rec):
    return math.hypot(p[0] - rec["x"], p[1] - rec["y"]) <= DOOR_SAME


def is_dead_door(obs, mem):
    """True when door_ahead is a door that stayed shut after a USE in reach, recently."""
    p = door_world_pos(obs)
    if p is None:
        return False
    lvl = level_of(obs)
    return any(d["level"] == lvl and mem.game_tics - d["tic"] < DEAD_DOOR_TICS and _near(p, d)
               for d in mem.dead_doors)


def use_reaches(door, mem):
    """Would a USE pressed now reach this door? USE reaches DOOR_REACH units. Up to
    DOOR_PRESS_DIST only if the marine is walking toward it (the momentum of the last
    forward move carries it into reach during the press); standing still at 78 units
    the press hits nothing - on the board that made 3 out-of-reach presses mark a
    normal door `wont_open`, and the marine then swung left/right in front of it for
    25 decisions."""
    if not door:
        return False
    if door["dist"] <= DOOR_REACH:
        return True
    walking = bool(mem.recent) and mem.recent[-1] in WALK_TOWARD and mem.consecutive_blocked == 0
    return walking and door["dist"] <= DOOR_PRESS_DIST


def note_door_attempt(obs, mem):
    """A USE was just pressed: if it was aimed at door_ahead from within reach, count it
    against that door (to press again, or give up, if the door stays shut)."""
    door = obs.get("door_ahead")
    if not (use_reaches(door, mem) and abs(door["bearing"]) <= 25):
        return
    p = door_world_pos(obs)
    if p is None:
        return
    w = mem.door_work
    if w and w["level"] == level_of(obs) and _near(p, w):
        w["tries"] += 1
        w["tic"] = mem.game_tics
    else:
        mem.door_work = {"x": p[0], "y": p[1], "level": level_of(obs), "tries": 1, "tic": mem.game_tics}


def bearing_to(obs, x, y):
    """Bearing (deg, + = right) from the player to the map point (x, y)."""
    b = math.degrees(math.atan2(x - obs["x"], y - obs["y"])) - obs["angle"]
    return (b + 180) % 360 - 180


def door_status(obs, mem):
    """Where the marine is with the door it last pressed USE on:
    None      no such door, or it is not in view (it opened, or we looked away);
    "working" still shut, but give it time to rise (waiting here is not being stuck);
    "retry"   still shut DOOR_RETRY_TICS after the press: the press did not take;
    "dead"    still shut after DOOR_MAX_TRIES presses: now remembered as not opening."""
    w = mem.door_work
    if not w:
        return None
    if w["level"] != level_of(obs) or mem.game_tics - w["tic"] > DOOR_FORGET_TICS or \
            math.hypot(obs["x"] - w["x"], obs["y"] - w["y"]) > DOOR_FORGET_DIST:
        mem.door_work = None
        return None
    p = door_world_pos(obs)
    if p is None or not _near(p, w):
        # Gone from view. Looking right at where it was: it opened, done with it.
        # Looked away: keep counting, so turning away between presses of a locked
        # door does not reset the count (that is the loop this is meant to end).
        if abs(bearing_to(obs, w["x"], w["y"])) <= 30:
            mem.door_work = None
        return None
    if mem.game_tics - w["tic"] < DOOR_RETRY_TICS:
        return "working"
    if w["tries"] < DOOR_MAX_TRIES:
        return "retry"
    mem.dead_doors.append({"x": w["x"], "y": w["y"], "tic": mem.game_tics, "level": w["level"]})
    mem.door_work = None
    return "dead"


SECTOR_BEARING = {name: (lo + hi) / 2 for name, lo, hi in SECTORS}


def way_out(sectors, openings):
    """The most open direction in view, for getting unstuck: a long opening if there
    is one, else the sector with the most room. `dead_end` = nothing in view is far."""
    best = max(openings, key=lambda o: o["dist"]) if openings else None
    if not best:
        s = max(sectors, key=lambda s: s["max_dist"])
        best = {"bearing": SECTOR_BEARING[s["sector"]], "dist": s["max_dist"]}
    b = best["bearing"]
    side = "ahead" if abs(b) <= 10 else ("left" if b < 0 else "right")
    dead_end = all(s["min_dist"] < 150 for s in sectors) and best["dist"] < 200
    return {"bearing": round(b), "dist": round(best["dist"]), "side": side, "dead_end": dead_end}


def obstacle_ahead(walls_ahead, hazards, bearing=15):
    """How far the marine can actually walk straight before hitting something: the
    nearest wall dead ahead, but also a barrel in the walking corridor (barrels are
    not in the wall fan, so without this `ahead` reads the wall *behind* the barrel
    and forward looks clear while the marine plows into it)."""
    d = walls_ahead
    for h in hazards:
        if abs(h["bearing"]) <= bearing and h["dist"] < d:
            d = h["dist"]
    return d


def view_of(obs, sectors, openings, ahead, mem, rays_mode):
    """The `view` block. Always the 7-sector summary + openings + ahead_min_dist. The
    full 51-ray fan is worth its ~+750 tokens only for lining up on a narrow doorway or
    threading a tight spot (an A/B showed the 7 sectors match the full fan ~85% of the
    time, differing mainly there), so with rays_mode 'auto' it is added only near a door
    or when stuck; 'always' / 'never' force it."""
    view = {"ahead_min_dist": ahead, "sectors": sectors, "openings": openings}
    if obs.get("walk_blocked_ahead"):
        view["ahead_unwalkable"] = True
    h = hurt_ahead(obs)
    if h is not None:
        view["ahead_hurt_dist"] = h
    detailed = rays_mode == "always" or (rays_mode == "auto" and (
        obs.get("door_ahead") or obs.get("blocked") or mem.stalled(PROGRESS_STEPS, GOING_NOWHERE)))
    if detailed:
        view["rays"] = []
        for w in obs.get("walls", []):
            r = {"b": w["bearing"], "d": w["dist"]}
            if w.get("hurt") is not None and w["hurt"] >= 0:
                r["hurt"] = w["hurt"]
            view["rays"].append(r)
    return view


def build_state(obs, mem, intent, map_win, rays_mode="auto"):
    sectors, openings, ahead = summarize_walls(obs.get("walls", []))
    visible = obs.get("visible", [])
    enemies = [{"name": e["name"], "dist": e["dist"], "bearing": e["bearing"]}
               for e in hostiles(visible)]
    hazards = [{"name": e["name"], "dist": e["dist"], "bearing": e["bearing"]}
               for e in visible if e["name"] in NON_HOSTILE]
    ahead = obstacle_ahead(ahead, hazards)
    door = obs.get("door_ahead")
    if door:
        door = dict(door)
        w = mem.door_work
        p = door_world_pos(obs)
        if w and p is not None and w["level"] == level_of(obs) and _near(p, w):
            door["presses"] = w["tries"]
            door["tics_since_press"] = mem.game_tics - w["tic"]
        if is_dead_door(obs, mem):
            door["wont_open"] = True
        door.setdefault("kind", "door")
    moved = mem.net_displacement(PROGRESS_STEPS)
    going_nowhere = moved is not None and moved < GOING_NOWHERE
    dithering = is_oscillating(mem.recent) or (going_nowhere and is_sweeping(mem.recent))
    stuck = going_nowhere or dithering or mem.consecutive_blocked >= 2
    best = mem.best_seen(obs) if stuck else None
    target_depth = mem.pano_depth(intent.target_heading) if intent.target_heading is not None else None
    aim = None
    if enemies:
        tgt = min(enemies, key=lambda e: abs(e["bearing"]))
        aim = {"enemy": tgt["name"], "bearing": tgt["bearing"], "dist": tgt["dist"],
               "on_target": abs(tgt["bearing"]) <= AIM_CONE and tgt["dist"] <= AIM_HOLD_DIST
                            and obs["ammo"] != 0}
    state = {
        "legend": STATE_LEGEND,
        "player": {
            "health": obs["health"], "armor": obs["armor"],
            "weapon": obs["weapon"], "ammo": obs["ammo"],
            "heading": obs["angle"],
            "level": f"E{obs['episode']}M{obs['map']}",
        },
        "view": view_of(obs, sectors, openings, ahead, mem, rays_mode),
        "way_out": way_out(sectors, openings),
        "enemies_in_view": enemies,
        "aim": aim,
        "hazards_in_view": hazards,
        "door_ahead": door,
        "blocked_last_move": bool(obs.get("blocked")),
        "consecutive_blocked": mem.consecutive_blocked,
        "recent_actions": list(mem.recent),
        "same_action_streak": mem.same_streak,
        "dithering": dithering,
        "progress": {"moved_last_10_steps": None if moved is None else round(moved),
                     "going_nowhere": going_nowhere},
        "steps": mem.steps,
        "commander": intent.to_state(obs["angle"], stuck=stuck, target_depth=target_depth),
    }
    if best:
        state["best_seen"] = best
    fl = floor_of(obs)
    if fl:
        state["floor"] = fl
    if (obs.get("radsuit_tics", 0) or 0) > 0:
        state["player"]["radsuit_secs"] = round(obs["radsuit_tics"] / TICRATE)
    if map_win:
        state["automap_window"] = {
            "legend": MAP_LEGEND,
            "rows": map_win,
        }
    return state


# ----------------------------------------------------------------------------
# Policy: turn Jev's answers into one firmware action
# ----------------------------------------------------------------------------

def pick_macro(answers, policy, min_conf, rng, allow_sample=True):
    a = answers["action"]
    probs = a.get("probabilities") or {}
    choice = a.get("choice")
    conf = float(a.get("confidence", 1.0) or 0.0)
    valid = {k: float(v) for k, v in probs.items() if k in MACROS}
    if choice not in MACROS:
        choice = max(valid, key=valid.get) if valid else "wait"
    sampled = False
    # `mixed` (default) samples from Jev's distribution when it is unsure, for
    # exploration - but NOT while stuck (blocked / going nowhere): rolling the dice
    # right there is what makes the marine flail (a random turn_around 180 twice = a
    # ~360 spin). There we take Jev's own top choice. `--policy sample` still always
    # samples (an explicit opt-in to exploration).
    if valid and (policy == "sample" or (policy == "mixed" and allow_sample and conf < min_conf)):
        # never let a random draw pick turn_around: a 180 flip is a last resort, wrong
        # unless it is Jev's actual top choice; sampling into it is what spins the marine
        pool = {n: w for n, w in valid.items() if n != "turn_around" or choice == "turn_around"}
        names = list(pool)
        weights = [max(pool[n], 0.0) for n in names]
        if sum(weights) > 0:
            choice = rng.choices(names, weights=weights, k=1)[0]
            sampled = True
    return choice, conf, sampled


def fire_probability(answers):
    f = answers.get("fire") or {}
    try:
        return float(f.get("noul", 0.0))
    except (TypeError, ValueError):
        return 0.0


TURN_MACROS_LEFT = {"turn_left", "turn_left_small", "forward_left"}
TURN_MACROS_RIGHT = {"turn_right", "turn_right_small", "forward_right"}
OPPOSITES = {  # macro -> the macro that undoes it (dithering pairs)
    **{m: "R" for m in TURN_MACROS_LEFT}, **{m: "L" for m in TURN_MACROS_RIGHT},
    "strafe_left": "sR", "strafe_right": "sL", "forward": "B", "back": "F",
}
SIDE_OF = {**{m: "L" for m in TURN_MACROS_LEFT}, **{m: "R" for m in TURN_MACROS_RIGHT},
           "strafe_left": "sL", "strafe_right": "sR", "forward": "F", "back": "B"}
MOVE_MACROS = {m for m, d in MACROS.items() if d["action"].get("move") or d["action"].get("strafe")}
STRAIGHT_MOVES = {m for m in MOVE_MACROS if not MACROS[m]["action"].get("turn")}
FORWARD_MOVES = {m for m in STRAIGHT_MOVES if MACROS[m]["action"].get("move") == "forward"
                 and not MACROS[m]["action"].get("strafe")}
WALK_TOWARD = {m for m in MOVE_MACROS if MACROS[m]["action"].get("move") == "forward"}
TURN_SIDE = {"turn_left": "left", "turn_left_small": "left",      # turns in place (no move)
             "turn_right": "right", "turn_right_small": "right"}
NO_UNDO_ROOM = 64      # clearance ahead to walk out of a turn-undo loop (the marine is 32 wide)


def is_oscillating(recent, n=4):
    """True when the last n macros alternate between a move and its opposite
    (left/right turns, left/right strafes, forward/back): a dithering loop."""
    if len(recent) < n:
        return False
    last = list(recent)[-n:]
    if any(m not in SIDE_OF for m in last):
        return False
    sides = [SIDE_OF[m] for m in last]
    return all(OPPOSITES[a] == b for a, b in zip(last, sides[1:]))


TURN_IN_PLACE = {"turn_left": "L", "turn_left_small": "L", "turn_right": "R",
                 "turn_right_small": "R", "turn_around": "R"}


def is_sweeping(recent, n=6):
    """True when the last n macros are all turns in place, in both directions: the
    marine is sweeping its view left and right (RRR LLLL RRR...) without going anywhere.
    is_oscillating only sees strict L/R/L/R alternation and misses this."""
    last = list(recent)[-n:]
    if len(last) < n or any(m not in TURN_IN_PLACE for m in last):
        return False
    return len({TURN_IN_PLACE[m] for m in last}) == 2


def break_oscillation(obs, mem):
    """Replacement macro when the reflexes dither left/right in front of something."""
    door = obs.get("door_ahead")
    ahead = min((w["dist"] for w in obs.get("walls", []) if -6 <= w["bearing"] <= 6), default=RAY_RANGE)
    if door and door["dist"] < 80 and abs(door["bearing"]) <= 25 and not is_dead_door(obs, mem):
        return "use"
    if ahead >= 100:
        return "forward"
    return unstick_macro(obs, mem)


def unstick_macro(obs, mem, allow_use=True):
    """Safety net when Jev keeps walking into the same wall (or goes nowhere):
    try a close door, else turn toward the most open direction (or go, if that is
    straight ahead). `allow_use=False` where pressing USE again is not wanted."""
    sectors, openings, ahead = summarize_walls(obs.get("walls", []))
    sect = {s["sector"]: s["max_dist"] for s in sectors}
    door = obs.get("door_ahead")
    if allow_use and door and door["dist"] < 80 and abs(door["bearing"]) <= 25 and \
            mem.recent and mem.recent[-1] != "use" and not is_dead_door(obs, mem):
        return "use"
    # nothing in the way straight ahead (e.g. the door in front just opened): go, do
    # not turn away from it. Barrels are not in `walls`, so check things too.
    thing_ahead = any(t["dist"] < 128 and abs(t["bearing"]) <= 15 for t in obs.get("visible", []))
    if openings:
        o = max(openings, key=lambda o: o["dist"])
        if abs(o["bearing"]) <= 10 and not thing_ahead and not obs.get("blocked"):
            return "forward"
        return "turn_left" if o["bearing"] < 0 else "turn_right"
    left = max(sect["left"], sect["far_left"])
    right = max(sect["right"], sect["far_right"])
    if max(left, right) < 200:
        return "turn_around"
    return "turn_left" if left > right else "turn_right"


def macro_to_action(macro, fire, speed, weapon=None, catchup=0, realtime=False):
    """Build the firmware action for a macro.

    `catchup`: after the pause in which we were thinking, the device's first
    tick runs up to ~5 buffered game tics with the new keys already held
    (vanilla Doom sync: BuildNewTic buffers at most 5 tics). So a step advances
    about `tics + catchup` tics; we subtract the expected catch-up so turns land
    where the macro says.

    `realtime`: firmware free-running mode (`hold`): the keys stay held while
    the game keeps running and are swapped by the next decision; `tics` is then
    the auto-release timeout, so it is the macro's full duration (no catch-up)."""
    m = MACROS[macro]
    action = dict(m["action"])
    if realtime:
        action["hold"] = True
        tics = max(1, min(175, round(m["tics"] * speed)))
    else:
        tics = max(1, min(60, round(m["tics"] * speed) - catchup))
    action["tics"] = tics
    if fire:
        action["fire"] = True
    if weapon:
        action["weapon"] = int(weapon)
    return action


# ----------------------------------------------------------------------------
# Supervisor (System 2)
# ----------------------------------------------------------------------------

SUPERVISOR_SYSTEM = (
    "You are the strategic supervisor (System 2) of a fast reflex controller "
    "(System 1, TypeSafe Jev) playing DOOM (shareware E1M1..E1M9) on real hardware, "
    "a SCINTIX P4 board. The reflex layer decides ~3-5 times per second from the "
    "rendered view only (a depth fan, enemies in sight, a door flag). It cannot "
    "remember the map or plan. You can: you see the automap of what has been "
    "discovered, the recent trajectory and statistics. Your job is to set a compact "
    "INTENT that the reflex layer reads at every step. Be decisive and concrete. "
    "Compass headings: 0 = north (up on the map), 90 = east, 180 = south, 270 = west. "
    "target_heading is a straight compass direction, not a path: if the marine is "
    "against a wall that way, pick the heading of the opening/doorway it must go "
    "through first (e.g. the gap in a wall line on the map), or null while it is stuck "
    "so it can get free locally. Barrels are hazards, not targets: never tell the "
    "reflexes to shoot barrels or walk into them, only to go around them. "
    "Map glyphs: '#' = wall (also a closed door or a ledge too high to climb), ':' = a "
    "step edge the marine CAN walk over (rows of ':' = stairs: a way up, not a wall), "
    "'~' = the edge of a damaging floor (nukage/slime pool: avoid crossing it without a "
    "radiation suit), '@' = the marine, ' ' = unexplored or open floor. "
    "Respond with ONLY a JSON object, no prose, with keys: "
    "goal (string, <= 160 chars, imperative, what the reflexes should do now, e.g. "
    "'Head east along the corridor, open the door at its end, then clear the room'), "
    "target_heading (integer 0-359 or null: the compass direction to travel to reach "
    "unexplored space or the objective; null if the reflexes should just react locally), "
    "fire_policy ('aggressive' | 'normal' | 'conservative'), "
    "preferred_weapon (integer 1-7 or null; 2 pistol, 3 shotgun, 4 chaingun), "
    "note (string, <= 120 chars: your reasoning in one line)."
)


def trajectory_summary(mem, seconds=30):
    cutoff = time.time() - seconds
    pts = [p for p in mem.positions if p[0] >= cutoff]
    if not pts:
        return "no trajectory yet"
    step = max(1, len(pts) // 8)
    sampled = pts[::step]
    path = " -> ".join(f"({int(x)},{int(y)})" for _, x, y, _ in sampled)
    return (f"last {seconds}s: {len(pts)} steps, moved {mem.distance_travelled(seconds):.0f} units, "
            f"path {path}")


def render_map(map_json, heading=None):
    if not map_json or not map_json.get("valid"):
        return "(automap not available)"
    p = map_json.get("player", {})
    head = (f"automap {map_json['cols']}x{map_json['rows']}, {map_json['units_per_cell']} units/cell, "
            f"north up; you are '@' at col {p.get('col')} row {p.get('row')}, "
            f"heading {p.get('angle', heading)} deg (compass)")
    return head + "\n" + "\n".join(map_json.get("grid", []))


class Supervisor(threading.Thread):
    """Slow planning loop. Runs in the background and updates `holder.intent`."""

    def __init__(self, provider, model, api_key, base_url, holder, every, log, verbose):
        super().__init__(daemon=True)
        self.provider = provider
        self.model = model
        self.api_key = api_key
        self.base_url = base_url
        self.holder = holder            # shared object with .intent, .snapshot(), .lock
        self.every = every
        self.log = log
        self.verbose = verbose
        self.stop_event = threading.Event()
        self.wake = threading.Event()
        self.calls = 0
        self.errors = 0
        self.last_plan_t = 0.0
        self.last_plan_hp = None
        self.last_plan_level = None
        self.last_plan_cov = None
        self.name_str = f"{provider}:{model}" if provider != "dry" else "dry"

    # -- triggers ------------------------------------------------------------
    def should_replan(self, snap):
        now = time.time()
        if snap is None or not snap["obs"].get("valid"):
            return False, None
        if now - self.last_plan_t >= self.every:
            return True, "periodic"
        if now - self.last_plan_t < max(3.0, self.every / 3):
            return False, None
        mem = snap["mem"]
        obs = snap["obs"]
        level = (obs["episode"], obs["map"])
        if self.last_plan_level is not None and level != self.last_plan_level:
            return True, "new level"
        if mem["consecutive_blocked"] >= 5 or mem["blocked_recent"] >= 8:
            return True, "stuck"
        if self.last_plan_hp is not None and self.last_plan_hp - obs["health"] >= 25:
            return True, "taking damage"
        growth = mem["coverage_growth_20s"]
        if growth is not None and growth == 0 and mem["distance_20s"] < 150 and now - self.last_plan_t > 15:
            return True, "no progress"
        return False, None

    # -- main loop -----------------------------------------------------------
    def run(self):
        while not self.stop_event.is_set():
            self.wake.wait(timeout=0.5)
            self.wake.clear()
            snap = self.holder.snapshot()
            go, why = self.should_replan(snap)
            if not go:
                continue
            self.plan(snap, why)

    def plan(self, snap, why):
        obs = snap["obs"]
        self.last_plan_t = time.time()
        self.last_plan_hp = obs["health"]
        self.last_plan_level = (obs["episode"], obs["map"])
        t0 = time.perf_counter()
        try:
            if self.provider == "dry":
                intent = self.dry_plan(snap)
            else:
                intent = self.llm_plan(snap, why)
            self.calls += 1
            intent.source = self.name_str
            self.holder.set_intent(intent)
            dt = time.perf_counter() - t0
            if self.verbose:
                th = intent.target_heading if intent.target_heading is not None else "-"
                say(f"  [supervisor {why}, {dt:.1f}s] goal: {intent.goal}  | heading {th} | "
                      f"fire {intent.fire_policy} | weapon {intent.preferred_weapon or '-'} | {intent.note}")
            if self.log:
                self.log.write({"t": time.time(), "kind": "plan", "why": why, "latency": dt,
                                "intent": intent.__dict__})
        except Exception as e:  # never kill the game loop
            self.errors += 1
            say(f"  [supervisor error] {e}")

    # -- providers -----------------------------------------------------------
    def user_prompt(self, snap, why):
        obs = snap["obs"]
        mem = snap["mem"]
        cur = snap["intent"]
        sectors, openings, ahead = summarize_walls(obs.get("walls", []))
        lines = [
            f"Replan trigger: {why}.",
            f"Player: health {obs['health']}, armor {obs['armor']}, weapon {obs['weapon']}, ammo {obs['ammo']}, "
            f"level E{obs['episode']}M{obs['map']}, position ({obs['x']},{obs['y']}), heading {obs['angle']} deg.",
            "View depth by sector (min/max dist): " + ", ".join(
                f"{s['sector']} {s['min_dist']}/{s['max_dist']}" for s in sectors) + ".",
            "Openings in view: " + (", ".join(f"bearing {o['bearing']} dist {o['dist']}" for o in openings) or "none") + ".",
            "Enemies in view: " + (", ".join(f"{e['name']} dist {e['dist']} bearing {e['bearing']}"
                                            for e in obs.get("visible", [])) or "none") + ".",
            f"Door ahead: {obs.get('door_ahead') or 'none'}. Blocked last move: {obs.get('blocked')}, "
            f"consecutive blocked: {mem['consecutive_blocked']}, blocked in the last 20 steps: {mem['blocked_recent']}.",
            f"Recent macros: {mem['recent']}.",
            f"Stats: {mem['steps']} steps, blocked {mem['blocked_total']} times, used doors/switches {mem['uses']} times, "
            f"enemies seen {dict(mem['enemies_seen'])}, hp drop last 20s {mem['hp_drop_20s']}, "
            f"map cells discovered {mem['coverage']} (growth last 20s: {mem['coverage_growth_20s']}).",
            trajectory_summary_from_snap(snap),
            f"Current intent (from {cur['source']}, {time.time() - cur['created']:.0f}s old): goal='{cur['goal']}', "
            f"target_heading={cur['target_heading']}, fire_policy={cur['fire_policy']}, "
            f"preferred_weapon={cur['preferred_weapon']}.",
            "",
            render_map(snap["map"], obs["angle"]),
            "",
            "Decide the new intent. Return only the JSON object.",
        ]
        return "\n".join(lines)

    def llm_plan(self, snap, why):
        prompt = self.user_prompt(snap, why)
        if self.provider == "anthropic":
            text = self.call_anthropic(prompt)
        elif self.provider == "openai":
            text = self.call_openai(prompt)
        else:
            raise RuntimeError(f"unknown provider {self.provider}")
        return parse_intent(text, fallback=snap["intent"])

    def call_anthropic(self, prompt):
        if not self.api_key:
            raise RuntimeError("no ANTHROPIC_API_KEY (or --supervisor-key)")
        # Current Claude models think (adaptive) by default, and the thinking counts
        # against max_tokens: with the old 400 the budget ran out before any text and the
        # reply came back empty ("supervisor returned no JSON: ''"). Room to think + low
        # effort (a short JSON reply does not need deep reasoning; keeps latency down).
        body = {
            "model": self.model,
            "max_tokens": 4000,
            "output_config": {"effort": "low"},
            "system": SUPERVISOR_SYSTEM,
            "messages": [{"role": "user", "content": prompt}],
        }
        req = urllib.request.Request(
            ANTHROPIC_URL, data=json.dumps(body).encode(), method="POST",
            headers={"x-api-key": self.api_key, "anthropic-version": ANTHROPIC_VERSION,
                     "content-type": "application/json"})
        data = http_json(req, timeout=60)
        parts = data.get("content", [])
        text = "".join(p.get("text", "") for p in parts if p.get("type") == "text")
        if not text.strip():
            raise RuntimeError(f"supervisor reply had no text (stop_reason={data.get('stop_reason')})")
        return text

    def call_openai(self, prompt):
        if not self.api_key:
            raise RuntimeError("no OPENAI_API_KEY (or --supervisor-key)")
        body = {
            "model": self.model,
            "messages": [{"role": "system", "content": SUPERVISOR_SYSTEM},
                         {"role": "user", "content": prompt}],
            "max_tokens": 400,
            "temperature": 0.3,
        }
        req = urllib.request.Request(
            self.base_url.rstrip("/") + "/chat/completions", data=json.dumps(body).encode(), method="POST",
            headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"})
        data = http_json(req, timeout=60)
        return data["choices"][0]["message"]["content"]

    def dry_plan(self, snap):
        """No LLM: point the reflexes toward the least-explored direction on the
        automap (the compass ray that runs farthest through unknown cells)."""
        m = snap["map"]
        if not m or not m.get("valid"):
            return Intent(DEFAULT_INTENT.goal, None, "normal", None, "no map yet", "dry")
        grid = m["grid"]
        rows, cols = m["rows"], m["cols"]
        pr, pc = m["player"]["row"], m["player"]["col"]
        best, best_score = None, -1
        for heading in range(0, 360, 45):
            rad = math.radians(heading)
            dr, dc = -math.cos(rad), math.sin(rad)   # row 0 = north
            score = 0
            r, c = float(pr), float(pc)
            for _ in range(max(rows, cols)):
                r += dr
                c += dc
                ri, ci = int(round(r)), int(round(c))
                if not (0 <= ri < rows and 0 <= ci < cols):
                    score += 6      # unknown territory beyond the revealed extent
                    break
                ch = grid[ri][ci]
                if ch == "#":
                    break
                score += 1
            if score > best_score:
                best, best_score = heading, score
        goal = (f"Explore: travel roughly {compass_name(best)} (heading {best}) toward unexplored space; "
                f"open any door on the way and shoot enemies in your sights.")
        return Intent(goal, best, "normal", None, f"frontier score {best_score}", "dry")


def trajectory_summary_from_snap(snap):
    return snap["mem"]["trajectory"]


def compass_name(h):
    names = ["north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"]
    return names[int(((h % 360) + 22.5) // 45) % 8]


def http_json(req, timeout):
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        detail = ""
        try:
            detail = e.read().decode()[:300]
        except Exception:
            pass
        raise RuntimeError(f"HTTP {e.code}: {detail or e.reason}")


def parse_intent(text, fallback):
    """Extract the JSON object from an LLM reply and validate it."""
    start, end = text.find("{"), text.rfind("}")
    if start < 0 or end <= start:
        raise RuntimeError(f"supervisor returned no JSON: {text[:120]!r}")
    data = json.loads(text[start:end + 1])
    goal = str(data.get("goal") or fallback["goal"])[:200]
    th = data.get("target_heading")
    try:
        th = int(th) % 360 if th is not None else None
    except (TypeError, ValueError):
        th = None
    fp = str(data.get("fire_policy") or "normal").lower()
    if fp not in ("aggressive", "normal", "conservative"):
        fp = "normal"
    pw = data.get("preferred_weapon")
    try:
        pw = int(pw) if pw is not None and 1 <= int(pw) <= 7 else None
    except (TypeError, ValueError):
        pw = None
    note = str(data.get("note") or "")[:160]
    return Intent(goal, th, fp, pw, note)


class SharedState:
    """What the game loop publishes for the supervisor, and the intent it gets back."""

    def __init__(self, intent):
        self.lock = threading.Lock()
        self.intent = intent
        self.intent_version = 0
        self._snap = None

    def publish(self, obs, mem, map_json):
        snap = {
            "obs": obs,
            "map": map_json,
            "mem": {
                "consecutive_blocked": mem.consecutive_blocked,
                "blocked_recent": sum(mem.blocked_window),
                "recent": list(mem.recent),
                "steps": mem.steps,
                "blocked_total": mem.blocked_total,
                "uses": mem.uses,
                "enemies_seen": dict(mem.enemies_seen),
                "hp_drop_20s": mem.hp_drop(20),
                "coverage": mem.coverage_history[-1][1] if mem.coverage_history else 0,
                "coverage_growth_20s": mem.coverage_growth(20),
                "distance_20s": mem.distance_travelled(20),
                "trajectory": trajectory_summary(mem),
            },
            "intent": dict(self.intent.__dict__),
        }
        with self.lock:
            self._snap = snap

    def snapshot(self):
        with self.lock:
            return self._snap

    def set_intent(self, intent):
        with self.lock:
            self.intent = intent
            self.intent_version += 1

    def get_intent(self):
        with self.lock:
            return self.intent, self.intent_version


# ----------------------------------------------------------------------------
# Telemetry
# ----------------------------------------------------------------------------

class JsonlLog:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8")
        self.lock = threading.Lock()

    def write(self, rec):
        with self.lock:
            self.f.write(json.dumps(rec, ensure_ascii=False) + "\n")
            self.f.flush()

    def close(self):
        self.f.close()


# The macro Jev picked, shown as a compass-style glyph + short name, for the live line.
ACTION_GLYPH = {
    "forward": "^", "forward_long": "^^", "forward_left": "<^", "forward_right": "^>",
    "turn_left": "<-", "turn_right": "->", "turn_left_small": "<", "turn_right_small": ">",
    "turn_around": "<->", "back": "v", "strafe_left": "[<", "strafe_right": ">]",
    "use": "()", "wait": "..",
}
_BARS = ".:-=+*#"      # 7 heights, ASCII (renders anywhere); every cell stays visible
SECTOR_ORDER = ["far_left", "left", "centre_left", "ahead", "centre_right", "right", "far_right"]


def radar(obs):
    """A 7-cell depth bar of what Jev sees left->right (taller/greener = more open),
    the centre cell = straight ahead. Purely for the on-camera terminal."""
    sect = {s: RAY_RANGE for s in SECTOR_ORDER}
    for name, lo, hi in SECTORS:
        rays = [w["dist"] for w in obs.get("walls", [])
                if (lo <= w["bearing"] <= hi if lo == -45 else lo < w["bearing"] <= hi)]
        if rays:
            sect[name] = min(rays)
    cells = []
    for i, name in enumerate(SECTOR_ORDER):
        d = sect[name]
        h = max(0, min(len(_BARS) - 1, int(d / 130)))   # near->far mapped to '.'..'#'
        col = "bred" if d < 120 else ("byel" if d < 350 else "bgrn")
        cells.append(c(_BARS[h], col, "bold") if i == 3 else c(_BARS[h], col))
    return c("[", "dim") + "".join(cells) + c("]", "dim")


def radar_key():
    """One-line legend for the live step line (printed once at start)."""
    bar = c("[", "dim") + c(".", "bred") + c("-", "byel") + c("=", "byel") + c("#", "bgrn") + c("]", "dim")
    return (f"{bar} depth L..R (" + c("near", "bred") + "->" + c("far", "bgrn") + ", centre=ahead)  "
            + c("ah", "grn") + " clear-ahead  " + c("=> ^ macro", "bcyn", "bold") + " decision  "
            + c("FIRE", "bred", "bold") + "  c=confidence  then what's in view ("
            + c("AIM", "bred", "bold") + "=on target)")


def fmt_step(mem, obs, macro, conf, fire_p, fired, latency, flags, intent):
    visible = obs.get("visible", [])
    enemies = hostiles(visible)
    hazards = [e for e in visible if e["name"] in NON_HOSTILE]
    wall_ahead = min((w["dist"] for w in obs.get("walls", []) if -6 <= w["bearing"] <= 6), default=0)
    ahead = obstacle_ahead(wall_ahead, hazards)   # what Jev sees: clearance incl. barrels
    moved = mem.net_displacement(PROGRESS_STEPS)

    # Fixed-width columns first (so they line up vertically on video); the
    # variable-length "what's in view" tags trail at the end, where ragged is fine.
    step_s = c(f"{mem.steps:4d}", "dim")
    hp_s = c(f"hp{obs['health']:<3d}", "grn" if obs["health"] > 40 else "bred")
    ah_s = c(f"ah{ahead:<4d}", "grn" if ahead >= 350 else ("yel" if ahead >= 120 else "red"))
    glyph = ACTION_GLYPH.get(macro, "?")
    act = c(f"{glyph:>3} {macro:<16}", "bcyn", "bold")
    fire_s = c("FIRE", "bred", "bold") if fired else c(" -- ", "dim")
    conf_s = c(f"c{conf:4.2f}", "dim")
    mv_s = c(f"mv{('--' if moved is None else round(moved)):>4}", "dim")
    ms_s = c(f"{latency * 1000:4.0f}ms", "dim")

    tags = []
    if enemies:
        e = min(enemies, key=lambda e: abs(e["bearing"]))
        on_tgt = abs(e["bearing"]) <= AIM_CONE and e["dist"] <= AIM_HOLD_DIST and obs["ammo"] != 0
        t = f"{e['name']} {e['dist']}@{e['bearing']:+d}"
        tags.append(c("AIM " + t, "bred", "bold") if on_tgt else c("en " + t, "byel"))
    if hazards:
        h = hazards[0]
        tags.append(c(f"barrel {h['dist']}@{h['bearing']:+d}", "bmag"))
    door = obs.get("door_ahead")
    if door:
        ds = f"door {door['dist']}@{door['bearing']:+d}"
        w = mem.door_work
        p = door_world_pos(obs)
        if w and p is not None and _near(p, w):
            ds += f"(x{w['tries']})"
        if is_dead_door(obs, mem):
            ds += "!"
        tags.append(c(ds, "red" if is_dead_door(obs, mem) else "bcyn"))
    if obs.get("blocked"):
        tags.append(c("BLOCKED", "bred", "bold"))
    tag_s = ("  " + "  ".join(tags)) if tags else ""
    flag_s = ("  " + c(" ".join(flags), "mag")) if flags else ""
    return (f"{step_s} {hp_s} {radar(obs)} {ah_s}  {c('=>', 'dim')} {act} {fire_s} {conf_s}  "
            f"{mv_s} {ms_s}{tag_s}{flag_s}")


# ----------------------------------------------------------------------------
# Main loop
# ----------------------------------------------------------------------------

def wait_for_level(link, quiet):
    obs, _ = link.observe()
    if obs.get("valid"):
        return obs
    say("No live player: start a game (New Game from the menu with the keyboard). Waiting...")
    while True:
        time.sleep(1.0)
        obs, _ = link.observe()
        if obs.get("valid"):
            say("In a level. Go.")
            return obs
        if not quiet:
            with _PRINT_LOCK:
                sys.stdout.write(".")
                sys.stdout.flush()


def expected_catchup(setting, pause_s):
    """Extra game tics the board runs with the new keys held at the start of a
    lockstep step. After a pause of n tics, the first doomgeneric_Tick runs
    min(5, max(1, n)) buffered tics (vanilla sync), i.e. up to 4 more than one.
    `setting` >= 0 forces a fixed value; -1 = estimate from the pause."""
    if setting >= 0:
        return setting
    elapsed = int((pause_s + 0.01) * TICRATE)       # + ~10 ms network
    return min(4, max(0, elapsed - 1))


def sample_observation():
    """A plausible observation (corridor ahead, opening on the left, an imp) for
    the --check probe, in the firmware's exact format."""
    walls = []
    for i in range(51):
        b = int((i - 25) * 90 / 50)                  # C integer division truncates toward zero
        d = 900 if -8 <= b <= 8 else (1400 if -30 <= b <= -22 else 260)
        walls.append({"bearing": b, "dist": d})
    return {"valid": True, "x": 1056, "y": -3232, "z": 0, "angle": 90, "health": 100, "armor": 0,
            "weapon": "pistol", "ammo": 50, "episode": 1, "map": 1,
            "visible": [{"name": "imp", "dist": 640, "bearing": -18}],
            "walls": walls, "blocked": False}


def sample_map():
    grid = [" " * 64 for _ in range(32)]
    grid[14] = " " * 20 + "#" * 24 + " " * 20
    grid[18] = " " * 20 + "#" * 10 + " " * 6 + "#" * 8 + " " * 20
    grid[16] = " " * 26 + "@" + " " * 37
    return {"valid": True, "cols": 64, "rows": 32, "units_per_cell": 48, "origin_x": 0, "origin_y": 0,
            "player": {"col": 26, "row": 16, "angle": 90}, "grid": grid}


def probe_jev(jev, calls=3):
    """One-shot health check of the Jev endpoint with a sample state. Returns
    (ok, message). Raises JevFatal for a bad key / rejected request."""
    obs = sample_observation()
    state = build_state(obs, Memory(), DEFAULT_INTENT, map_window(sample_map()))
    questions = build_questions(DEFAULT_INTENT)
    lat, last, tokens = [], None, 0
    for _ in range(calls):
        t0 = time.perf_counter()
        answers, usage = jev.decide(state, questions)
        lat.append(time.perf_counter() - t0)
        tokens = int((usage or {}).get("input_tokens", 0) or 0)
        a = answers.get("action", {})
        if a.get("choice") not in MACROS:
            return False, f"unexpected action answer: {json.dumps(a)[:200]}"
        last = answers
    lat.sort()
    a, f = last["action"], last.get("fire", {})
    return True, (f"{jev.name} OK: latency median {lat[len(lat) // 2] * 1000:.0f} ms "
                  f"(min {lat[0] * 1000:.0f}, max {lat[-1] * 1000:.0f}); sample answer: action={a.get('choice')} "
                  f"confidence={a.get('confidence')} fire={f.get('noul')}; {tokens} input tokens per call "
                  f"(~${tokens * 3 * 3600 / 1e6 * JEV_PRICE_PER_MTOK:.2f}/h at 3 decisions/s)")


def reconnect(link, mem, max_attempts=10):
    for attempt in range(1, max_attempts + 1):
        link.close()
        time.sleep(min(float(attempt), 5.0))
        say(f"  [link] reconnecting to {link.url} (attempt {attempt}/{max_attempts}) ...")
        try:
            link.connect()
            mem.reconnects += 1
            say("  [link] reconnected")
            return True
        except LINK_ERRORS as e:
            say(f"  [link] still down: {e}")
    return False


def connect_help(ws_url, err):
    hint = ""
    if isinstance(err, InvalidHandshake):
        hint = ("\n  - the board answered but refused the WebSocket: is it running the `ai-agent-api` "
                "firmware (the one with /agent)?")
    return (f"error: cannot open {ws_url}: {err}{hint}\n"
            f"  - is the board on the network and the IP right? (serial log: 'Connected to Wi-Fi with IP ...';\n"
            f"    on the setup access point SCINTIX-P4-XXXXXX it is 192.168.4.1)\n"
            f"  - only one agent connection at a time: close the MCP server / agent_client.py first")


def run(args):
    rng = random.Random(args.seed)
    ws_url = args.ws_url or f"ws://{args.host}:{args.port}/agent"
    link = DeviceLink(ws_url)

    # ---- Jev (System One) -----------------------------------------------------
    if args.jev == "dry":
        jev = DryJev()
    else:
        try:
            jev = JevClient(args.jev_url, args.jev_key or os.environ.get("TYPESAFE_API_KEY"),
                            args.jev_model, timeout=args.jev_timeout, user_agent=args.jev_user_agent)
        except JevError as e:
            sys.exit(f"error: {e}")
    dry_fallback = DryJev()

    # ---- supervisor (System Two) ------------------------------------------------
    shared = SharedState(DEFAULT_INTENT)
    supervisor = None
    if args.mode == "supervised":
        if args.supervisor == "anthropic":
            key = args.supervisor_key or os.environ.get("ANTHROPIC_API_KEY")
            model = args.supervisor_model or "claude-sonnet-5"
        elif args.supervisor == "openai":
            key = args.supervisor_key or os.environ.get("OPENAI_API_KEY") or os.environ.get("SUPERVISOR_API_KEY")
            model = args.supervisor_model
            if not model:
                sys.exit("error: --supervisor openai needs --supervisor-model <model name> (or env SUPERVISOR_MODEL)")
        else:
            key, model = None, "dry"
        if args.supervisor != "dry" and not key:
            env = "ANTHROPIC_API_KEY" if args.supervisor == "anthropic" else "OPENAI_API_KEY"
            sys.exit(f"error: --mode supervised with --supervisor {args.supervisor} needs an API key: "
                     f"set {env} or pass --supervisor-key (or use --supervisor dry)")
        supervisor = Supervisor(args.supervisor, model, key, args.supervisor_base_url,
                                shared, args.supervisor_every, None, not args.quiet)

    # ---- preflight: check the cloud side before touching the board ----------------
    if args.jev == "live":
        try:
            ok, msg = probe_jev(jev, calls=3 if args.check else 1)
            say(("  [check] " if ok else "  [check] PROBLEM: ") + msg)
            if not ok and args.check:
                sys.exit(2)
        except JevFatal as e:
            sys.exit(f"error: Jev rejected the request: {e}\n  - 401/403: the TypeSafe key is wrong or not enabled "
                     f"(early access?)\n  - 422: request format — please send me the message above")
        except JevError as e:
            if args.check:
                sys.exit(f"error: Jev unreachable: {e}")
            say(f"  [check] WARNING: Jev unreachable ({e}); starting anyway on the local fallback policy, "
                f"retrying Jev with backoff")
    if supervisor and args.check:
        if args.supervisor == "dry":
            say("  [check] supervisor dry: no LLM to check")
        else:
            m = Memory()
            m.coverage_history.append((time.time(), 40))
            shared.publish(sample_observation(), m, sample_map())
            t0 = time.perf_counter()
            try:
                intent = supervisor.llm_plan(shared.snapshot(), "check")
                say(f"  [check] supervisor {supervisor.name_str} OK in {time.perf_counter() - t0:.1f}s: "
                    f"goal='{intent.goal}' heading={intent.target_heading} fire={intent.fire_policy}")
            except Exception as e:
                sys.exit(f"error: supervisor {supervisor.name_str} failed: {e}")
            shared.set_intent(DEFAULT_INTENT)
    if args.check:
        try:
            link.connect()
            obs, dt = link.observe()
            link.close()
            if obs.get("valid"):
                say(f"  [check] board OK ({dt * 1000:.0f} ms): E{obs['episode']}M{obs['map']} hp={obs['health']} "
                    f"weapon={obs['weapon']} {len(obs.get('walls', []))} rays, {len(obs.get('visible', []))} things "
                    f"in view")
            else:
                say(f"  [check] board OK ({dt * 1000:.0f} ms) but no live player: start a New Game before playing")
        except LINK_ERRORS as e:
            sys.exit(connect_help(ws_url, e))
        say("  [check] all good")
        return

    log = JsonlLog(args.log) if args.log else None
    if supervisor:
        supervisor.log = log

    say(f"connecting to {ws_url} ...")
    try:
        link.connect()
    except LINK_ERRORS as e:
        sys.exit(connect_help(ws_url, e))

    mem = Memory()
    map_json = None
    t_start = time.time()
    try:
        obs = wait_for_level(link, args.quiet)
        map_json, _ = link.get_map()
        mem.coverage_history.append((time.time(), map_coverage(map_json)))
        mem.levels.append(f"E{obs['episode']}M{obs['map']}")
        shared.publish(obs, mem, map_json)
        if supervisor:
            supervisor.start()
            supervisor.wake.set()
        say(f"mode={args.mode}  decisions={jev.name}  supervisor={supervisor.name_str if supervisor else 'none'}  "
            f"policy={args.policy} min_conf={args.min_confidence} nets={args.nets} speed={args.speed}  "
            f"pacing={'realtime (free-running)' if args.realtime else 'lockstep'}")
        say(f"player at ({obs['x']},{obs['y']}) heading {obs['angle']} E{obs['episode']}M{obs['map']} "
            f"hp={obs['health']} weapon={obs['weapon']} ammo={obs['ammo']}")
        if not args.quiet:
            say(c("  line: ", "dim") + radar_key())

        t_start = time.time()
        jev_retry_at = 0.0          # >0: Jev failed, don't call it again before this time
        jev_backoff = 1.0
        last_reply_t = time.perf_counter()
        dead, dead_since = False, 0.0
        realtime_warned = False
        frozen_hint_t = 0.0
        last_map_print_t = 0.0
        while True:
            if args.steps and mem.steps >= args.steps:
                break
            if args.duration and time.time() - t_start >= args.duration:
                break
            try:
                intent, _ = shared.get_intent()
                step_t0 = time.perf_counter()
                flags = []
                answers, usage, conf, fire_p, fire = None, {}, 1.0, 0.0, False
                latency, jev_latency = 0.0, None
                state = None

                # ---- game-level situations the reflexes should not handle ---------
                system_macro = None
                if obs["health"] <= 0:
                    if not dead:
                        dead = True
                        mem.deaths += 1
                        dead_since = time.time()
                        say(f"  [game] the marine died (death #{mem.deaths}); pressing USE in a moment to restart "
                            f"the level")
                    # wait ~1 s, then alternate USE / wait (a press needs a release in between)
                    if time.time() - dead_since < 1.0 or (mem.recent and mem.recent[-1] == "use"):
                        system_macro = "wait"
                    else:
                        system_macro = "use"
                    flags.append("respawn")
                elif mem.frozen_steps >= args.frozen_after:
                    # Nothing changes whatever we press: end-of-level stats screen (needs USE),
                    # or a menu is open / the game is paused (needs a human on the keyboard).
                    system_macro = "wait" if (mem.recent and mem.recent[-1] == "use") else "use"
                    if system_macro == "use":
                        mem.unfreeze_presses += 1
                    flags.append("unfreeze")
                    if mem.unfreeze_presses >= 4 and time.time() - frozen_hint_t > 15:
                        frozen_hint_t = time.time()
                        say("  [game] the game looks frozen: if a menu is open press Esc on the board's keyboard; "
                            "at the end-of-level screen USE should advance it")
                else:
                    dead = False

                door_st = door_status(obs, mem)
                if door_st == "dead":
                    say(f"  [game] the door ahead stayed shut after {DOOR_MAX_TRIES} presses of USE (locked, or "
                        f"not a real door): telling Jev it won't open (door_ahead.wont_open) for a while")
                    flags.append("door-noted")

                if system_macro is not None:
                    macro = system_macro
                else:
                    # ---- decide (Jev) ------------------------------------------------
                    map_win = map_window(map_json) if map_json else None
                    state = build_state(obs, mem, intent, map_win, args.view_rays)
                    questions = build_questions(intent)
                    t0 = time.perf_counter()
                    if jev_retry_at and time.time() < jev_retry_at:
                        answers, usage = dry_fallback.decide(state, questions)
                        mem.fallbacks += 1
                        flags.append("FALLBACK")
                    else:
                        try:
                            answers, usage = jev.decide(state, questions)
                            mem.jev_calls += 1
                            if jev_retry_at:
                                say("  [jev] back online")
                            jev_retry_at = 0.0
                            jev_backoff = 1.0
                        except JevError as e:
                            mem.jev_errors += 1
                            mem.fallbacks += 1
                            flags.append("FALLBACK")
                            if isinstance(e, JevFatal):
                                say(f"  [jev error] {e} -- not retrying (bad key or rejected request); playing on "
                                    f"the local fallback policy for the rest of the run")
                                jev_retry_at = float("inf")
                            else:
                                say(f"  [jev error] {e} -- local fallback policy, retrying Jev in {jev_backoff:.0f}s")
                                jev_retry_at = time.time() + jev_backoff
                                jev_backoff = min(jev_backoff * 2, 15.0)
                            answers, usage = dry_fallback.decide(state, questions)
                    latency = time.perf_counter() - t0
                    jev_latency = latency if "FALLBACK" not in flags else None

                    stuck_now = bool(obs.get("blocked")) or mem.stalled(PROGRESS_STEPS, GOING_NOWHERE)
                    macro, conf, sampled = pick_macro(answers, args.policy, args.min_confidence, rng,
                                                      allow_sample=not stuck_now)
                    if sampled:
                        flags.append("sampled")
                    fire_p = fire_probability(answers)
                    fire = fire_p >= args.fire_threshold and obs["ammo"] != 0

                    # Safety nets (deterministic, always flagged in the telemetry). With the
                    # default `--nets doom` only the two Doom mechanics a single frame
                    # cannot show (USE reach, USE toggling a rising door) are enforced;
                    # everything else is Jev's call, from the memory facts in its state
                    # (door_ahead.presses / tics_since_press / wont_open, progress,
                    # dithering). `--nets all` adds the older behaviour nets below, which
                    # override Jev instead; `--nets none` is pure Jev.
                    mech = args.nets in ("doom", "all")
                    extra = args.nets == "all"
                    # - two turn_arounds in a row put you back where you started (a 360
                    #   spin that goes nowhere): the second one is a no-op, so head for
                    #   the most open direction instead. A Doom mechanic, not strategy.
                    if mech and macro == "turn_around" and mem.recent and mem.recent[-1] == "turn_around":
                        macro = unstick_macro(obs, mem, allow_use=False)
                        if macro == "turn_around":     # boxed in: don't 180 again, probe forward/sideways
                            wall_ahead = min((w["dist"] for w in obs.get("walls", []) if -6 <= w["bearing"] <= 6),
                                             default=RAY_RANGE)
                            macro = "forward" if wall_ahead >= 120 and not obs.get("blocked") else "turn_right"
                        mem.heuristic_overrides += 1
                        flags.append("no-spin")
                    # - a turn in place straight after a turn the other way undoes it (same
                    #   class as two turn_arounds). On the board a big turn is ~34 deg, so
                    #   with the open direction in between (bearing +13 -> turn right -> -21
                    #   -> turn left -> +13 ...) the marine never gets within +/-10 to walk:
                    #   468 of 1024 decisions in one L2 run. Mixing big and small turns
                    #   (R, l, L, r, ...) is the same loop. So: walk while curving the way
                    #   Jev wants if there is room ahead, else keep turning the way it was
                    #   already turning (a full look round in ~11 steps, no back-and-forth).
                    prev = mem.recent[-1] if mem.recent else None
                    if mech and macro in TURN_SIDE and prev in TURN_SIDE and TURN_SIDE[macro] != TURN_SIDE[prev] \
                            and not hostiles(obs.get("visible", [])) and door_st not in ("working", "retry"):
                        hazards = [t for t in obs.get("visible", []) if t["name"] in NON_HOSTILE]
                        clear = obstacle_ahead(min((w["dist"] for w in obs.get("walls", []) if -6 <= w["bearing"] <= 6),
                                                   default=RAY_RANGE), hazards)
                        if clear >= NO_UNDO_ROOM and not obs.get("blocked"):
                            macro = f"forward_{TURN_SIDE[macro]}"
                        else:
                            macro = f"turn_{TURN_SIDE[prev]}"
                        mem.heuristic_overrides += 1
                        flags.append("no-undo")
                    # - repeated blocked moves -> turn toward the most open direction
                    if extra and mem.consecutive_blocked >= args.unstick_after and macro in MOVE_MACROS:
                        macro = unstick_macro(obs, mem)
                        mem.heuristic_overrides += 1
                        flags.append("unstick")
                    # - left/right (or forward/back) dithering with no enemy in view -> commit
                    #   (in a fight, dodging back and forth is legitimate; a barrel is not a
                    #   fight - trying to "centre" one is exactly how the marine gets stuck)
                    no_enemy = not hostiles(obs.get("visible", []))
                    if extra and macro in SIDE_OF and no_enemy and \
                            is_oscillating(list(mem.recent) + [macro]):
                        macro = break_oscillation(obs, mem)
                        mem.heuristic_overrides += 1
                        flags.append("anti-osc")
                    # - going nowhere for a while (dithering, spinning, waiting or pushing in
                    #   place) with no enemy in view -> turn toward the most open direction
                    #   (or USE a never-tried door right ahead), then give Jev a full window
                    #   before trying again. Not while working a door: waiting for one to
                    #   rise is not being stuck, and turning away just as it opens is how
                    #   the marine ends up walking back and forth.
                    if door_st in ("working", "retry"):
                        mem.last_stall_step = mem.steps     # and a full window once the door is open
                    if extra and no_enemy and door_st not in ("working", "retry") and \
                            args.stall_steps > 0 and mem.steps - mem.last_stall_step >= args.stall_steps and \
                            mem.stalled(args.stall_steps, args.stall_radius):
                        macro = unstick_macro(obs, mem)
                        mem.last_stall_step = mem.steps
                        mem.heuristic_overrides += 1
                        flags.append("stall")
                    # - the door we pressed USE on is still shut DOOR_RETRY_TICS later: the
                    #   press did not take -> press again (or step back into reach first)
                    if extra and no_enemy and door_st == "retry":
                        d = obs["door_ahead"]
                        if d["dist"] <= DOOR_REACH and abs(d["bearing"]) <= 20:
                            macro = "use"
                        else:
                            macro = "forward" if abs(d["bearing"]) <= 12 else \
                                ("forward_left" if d["bearing"] < 0 else "forward_right")
                        mem.heuristic_overrides += 1
                        flags.append("door-retry")
                    # - a door already found shut after a USE in reach (locked, or not a real
                    #   door): going back to press USE on it is how the marine loops -> turn away
                    if extra and macro == "use" and is_dead_door(obs, mem):
                        macro = unstick_macro(obs, mem, allow_use=False)
                        mem.heuristic_overrides += 1
                        flags.append("dead-door")
                    # - USE only reaches 64 units: a `use` on a far door becomes a step toward it
                    door = obs.get("door_ahead")
                    if mech and macro == "use" and door and not use_reaches(door, mem) and \
                            not is_dead_door(obs, mem):
                        macro = "forward" if abs(door["bearing"]) <= 12 else \
                            ("forward_left" if door["bearing"] < 0 else "forward_right")
                        mem.heuristic_overrides += 1
                        flags.append("approach-door")
                    # - Doom's USE toggles doors: pressing it again while the door is still
                    #   rising sends it back DOWN. After a `use`, hold off for ~30 game tics.
                    if mech and macro == "use" and mem.last_use_tic is not None and \
                            (mem.game_tics - mem.last_use_tic) < args.use_cooldown:
                        macro = "wait"
                        flags.append("door-wait")

                weapon = None
                if system_macro is None and intent.preferred_weapon and \
                        intent.preferred_weapon != mem.last_weapon_applied:
                    weapon = intent.preferred_weapon
                    mem.last_weapon_applied = weapon
                    flags.append(f"weapon={weapon}")

                catchup = 0 if args.realtime else expected_catchup(args.catchup, time.perf_counter() - last_reply_t)
                action = macro_to_action(macro, fire, args.speed, weapon, catchup, args.realtime)

                # ---- act -----------------------------------------------------------
                t_sent = time.perf_counter()
                new_obs, dev_latency = link.step(action)
                last_reply_t = time.perf_counter()
                if args.realtime and not args.rt_pipelined and new_obs.get("valid"):
                    # In realtime the reply comes one frame after the new keys went down,
                    # i.e. BEFORE the macro has done anything. Deciding the next step from
                    # it means Jev always sees the world one macro late: after a 30-deg
                    # turn it still sees the old heading, turns again (60 deg), then walks
                    # off at the wrong angle - the double turns / L-L-R-R swing seen on the
                    # board. So let the macro play out (its hold expires on the board) and
                    # take a fresh frame with an empty hold: still realtime, and its
                    # `blocked` refers to the macro that just ran.
                    remain = action["tics"] / TICRATE - (time.perf_counter() - t_sent)
                    if remain > 0:
                        time.sleep(remain + 1.0 / TICRATE)
                    new_obs, _ = link.step({"hold": True, "tics": 1})
                    last_reply_t = time.perf_counter()
                if not new_obs.get("valid"):
                    say("player gone (menu / title screen?). Waiting for a live player...")
                    obs = wait_for_level(link, args.quiet)
                    last_reply_t = time.perf_counter()
                    continue
                if macro == "use":
                    mem.last_use_tic = mem.game_tics
                    note_door_attempt(obs, mem)     # obs = what the USE was aimed at
                if args.realtime:
                    mem.game_tics = int((time.time() - t_start) * TICRATE)   # the game runs on the wall clock
                    if action["tics"] >= 6:
                        mem.device_latency.append(dev_latency)
                    if not realtime_warned and len(mem.device_latency) >= 8:
                        med = sorted(mem.device_latency)[len(mem.device_latency) // 2]
                        if med > 0.12:
                            realtime_warned = True
                            say(f"  [warning] the board answers like lockstep (median {med * 1000:.0f} ms per step): "
                                f"is the realtime firmware patch flashed? Without it --realtime still works, "
                                f"just in lockstep")
                else:
                    mem.game_tics += action["tics"] + catchup + 1

                # frozen-game detector: nothing at all changed although we pressed something.
                # A straight move that came back `blocked` proves nothing (walking into a
                # wall or a barrel changes nothing either): only turns, USE, etc. count.
                same = all(new_obs.get(k) == obs.get(k) for k in ("x", "y", "angle", "health", "ammo", "armor"))
                pushing = new_obs.get("blocked") and macro in STRAIGHT_MOVES
                if same and macro != "wait" and not pushing:
                    mem.frozen_steps += 1
                elif not same:
                    if mem.frozen_steps >= args.frozen_after:
                        say("  [game] the game moves again")
                    mem.frozen_steps = 0
                    mem.unfreeze_presses = 0

                level = f"E{new_obs['episode']}M{new_obs['map']}"
                if mem.levels and level != mem.levels[-1]:
                    mem.levels.append(level)
                    say(f"  [game] now in {level}")
                    map_json = None

                mem.note_step(macro, new_obs, jev_latency, usage)

                # ---- automap refresh ---------------------------------------------
                if args.map_every and (mem.steps % args.map_every == 0 or map_json is None):
                    map_json, _ = link.get_map()
                    last_reply_t = time.perf_counter()
                    mem.coverage_history.append((time.time(), map_coverage(map_json)))
                # print the automap on a wall-clock cadence (--show-map-secs) or, failing
                # that, every N steps (--show-map-every)
                if not args.quiet and map_json is not None:
                    if args.show_map_secs > 0:
                        due = time.time() - last_map_print_t >= args.show_map_secs
                    else:
                        due = args.show_map_every and mem.steps % args.show_map_every == 0
                    if due:
                        say(render_map(map_json, new_obs["angle"]))
                        last_map_print_t = time.time()

                obs = new_obs
                shared.publish(obs, mem, map_json)
                if supervisor:
                    supervisor.wake.set()

                # ---- telemetry -----------------------------------------------------
                if not args.quiet:
                    say(fmt_step(mem, obs, macro, conf, fire_p, fire, latency, flags, intent))
                if log:
                    log.write({"t": time.time(), "kind": "step", "step": mem.steps, "state": state,
                               "answers": answers, "macro": macro, "action": action, "flags": flags,
                               "jev_latency": latency, "device_latency": dev_latency, "obs": obs,
                               "intent": intent.__dict__})
                if args.step_delay:
                    time.sleep(args.step_delay)
                if args.max_rate > 0:
                    # never flood the board (matters with a fast local policy or in realtime mode)
                    elapsed = time.perf_counter() - step_t0
                    if elapsed < 1.0 / args.max_rate:
                        time.sleep(1.0 / args.max_rate - elapsed)

            except LINK_ERRORS as e:
                say(f"  [link] lost the board: {e!r}")
                if mem.reconnects >= 20 or not reconnect(link, mem):
                    say("  [link] giving up")
                    break
                try:
                    obs = wait_for_level(link, args.quiet)
                    map_json = None
                    last_reply_t = time.perf_counter()
                except LINK_ERRORS:
                    pass   # the next iteration will notice and reconnect again

    except KeyboardInterrupt:
        say("\ninterrupted")
    finally:
        if supervisor:
            supervisor.stop_event.set()
        link.close()
        if log:
            log.close()
        wall = time.time() - t_start
        lat = (sum(mem.jev_latency) / len(mem.jev_latency)) if mem.jev_latency else 0
        rate = mem.steps / wall if wall > 0 else 0
        pct = 100 * mem.game_tics / TICRATE / wall if wall > 0 else 0
        print("\n--- summary ---")
        print(f"steps {mem.steps}  wall {wall:.0f}s  ({rate:.1f} decisions/s)  game tics {mem.game_tics} "
              f"(~{mem.game_tics / TICRATE:.0f}s of game time, {pct:.0f}% of wall time)")
        print(f"decisions {jev.name}: {mem.jev_calls} calls, {mem.jev_errors} errors, avg latency {lat*1000:.0f} ms, "
              f"{mem.jev_tokens} input tokens (~${mem.jev_tokens / 1e6 * JEV_PRICE_PER_MTOK:.3f})")
        print(f"blocked {mem.blocked_total}  safety-net overrides {mem.heuristic_overrides}  fallback decisions "
              f"{mem.fallbacks}  uses {mem.uses}  doors that did not open {len(mem.dead_doors)}  "
              f"deaths {mem.deaths}  reconnects {mem.reconnects}  "
              f"levels {' > '.join(mem.levels) or '-'}")
        print(f"enemies seen (step-count) {dict(mem.enemies_seen)}")
        if supervisor:
            print(f"supervisor {supervisor.name_str}: {supervisor.calls} plans, {supervisor.errors} errors")
        if mem.coverage_history:
            print(f"automap cells discovered: {mem.coverage_history[0][1]} -> {mem.coverage_history[-1][1]}")


def main():
    ap = argparse.ArgumentParser(description="Jev (TypeSafe System One) plays DOOM on the SCINTIX P4",
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("Usage:")[1] if "Usage:" in __doc__ else None)
    ap.add_argument("host", nargs="?", default="192.168.4.1", help="device IP/host (default 192.168.4.1 = portal AP)")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--ws-url", help="full WebSocket URL (overrides host/port)")
    ap.add_argument("--mode", choices=["jev", "supervised"], default="jev",
                    help="jev = level 1 (Jev alone); supervised = level 2 (Jev + LLM supervisor)")

    g = ap.add_argument_group("Jev (System One)")
    g.add_argument("--jev", choices=["live", "dry"], default="live", help="live = TypeSafe API; dry = local policy, no key")
    g.add_argument("--jev-url", default=os.environ.get("TYPESAFE_API_URL", JEV_DEFAULT_URL))
    g.add_argument("--jev-model", default=os.environ.get("TYPESAFE_MODEL", JEV_DEFAULT_MODEL))
    g.add_argument("--jev-key", help="TypeSafe API key (default: env TYPESAFE_API_KEY)")
    g.add_argument("--jev-timeout", type=float, default=2.0,
                   help="HTTP timeout per decision, seconds (keep it well under the board's 5 s agent watchdog)")
    g.add_argument("--jev-user-agent", default=os.environ.get("TYPESAFE_USER_AGENT", JEV_DEFAULT_USER_AGENT),
                   help="User-Agent header for the API request (the default is a browser UA; "
                        "urllib's own UA trips Cloudflare bot protection, HTTP 403 error 1010)")
    g.add_argument("--policy", choices=["argmax", "sample", "mixed"], default="mixed",
                   help="argmax = Jev's choice; sample = sample from its probabilities; "
                        "mixed = argmax unless confidence < --min-confidence (default)")
    g.add_argument("--min-confidence", type=float, default=0.35)
    g.add_argument("--view-rays", choices=["auto", "always", "never"], default="auto",
                   help="send the full 51-ray depth fan (in view.rays) on top of the 7 sectors: "
                        "auto = only near a door or when stuck (default; the extra detail helps line up "
                        "on narrow doorways, ~+750 tokens only then), always, or never")
    g.add_argument("--fire-threshold", type=float, default=0.5, help="hold FIRE when P(fire) >= this")

    s = ap.add_argument_group("Supervisor (System Two, --mode supervised)")
    s.add_argument("--supervisor", choices=["anthropic", "openai", "dry"], default="anthropic")
    s.add_argument("--supervisor-model", default=os.environ.get("SUPERVISOR_MODEL"),
                   help="model id (default claude-sonnet-5 for anthropic; required for openai)")
    s.add_argument("--supervisor-key", help="API key (default: env ANTHROPIC_API_KEY / OPENAI_API_KEY)")
    s.add_argument("--supervisor-base-url", default=os.environ.get("OPENAI_BASE_URL", OPENAI_DEFAULT_BASE),
                   help="OpenAI-compatible base URL (for --supervisor openai; local servers work too)")
    s.add_argument("--supervisor-every", type=float, default=10.0, help="seconds between periodic replans")

    p = ap.add_argument_group("Pacing and safety")
    p.add_argument("--realtime", action="store_true",
                   help="firmware free-running mode (needs the `hold` patch on the device / the simulator): the game "
                        "never pauses, each decision replaces the held keys and is answered at once; play speed = 100%%")
    p.add_argument("--rt-pipelined", action="store_true",
                   help="realtime only: decide from the frame returned right after sending the keys (the pre-2026-11 "
                        "behaviour: faster, but every decision sees the world one macro late, so turns double up). "
                        "Default: let each macro play out and decide from a fresh frame")
    p.add_argument("--speed", type=float, default=1.0, help="multiplier on each macro's tics (1.0 = defaults)")
    p.add_argument("--catchup", type=int, default=-1,
                   help="lockstep only: game tics the board catches up with the new keys after our thinking pause, "
                        "subtracted from each macro's tics so turns land right (-1 = estimate from the pause, "
                        "the default; 0..4 = fixed)")
    p.add_argument("--step-delay", type=float, default=0.0, help="extra sleep per step, seconds (slow-motion)")
    p.add_argument("--max-rate", type=float, default=10.0, help="cap on decisions per second (0 = uncapped)")
    p.add_argument("--nets", choices=["doom", "all", "none"], default="doom",
                   help="deterministic safety nets over Jev's choice. doom (default) = only the Doom mechanics a frame "
                        "cannot show: a `use` on a far door becomes a step toward it, a second `use` while the door "
                        "rises becomes `wait`; all = also override Jev when stuck (unstick, anti-oscillation, stall, "
                        "door retry, locked door); none = pure Jev. Death / end-of-level handling is always on")
    p.add_argument("--no-heuristics", dest="nets", action="store_const", const="none", help="same as --nets none")
    p.add_argument("--unstick-after", type=int, default=4,
                   help="--nets all: consecutive blocked moves before the unstick net kicks in")
    p.add_argument("--stall-steps", type=int, default=PROGRESS_STEPS,
                   help="--nets all: decisions without real progress (see --stall-radius), no enemy in view, before "
                        "the stall net turns the marine toward open space; also its cooldown. 0 = off")
    p.add_argument("--stall-radius", type=float, default=float(GOING_NOWHERE),
                   help="--nets all: net displacement (map units) over --stall-steps below which the marine counts "
                        "as stalled")
    p.add_argument("--use-cooldown", type=int, default=30,
                   help="game tics after a `use` during which another `use` becomes `wait` (a door needs ~28 tics to rise clear; "
                        "pressing USE again while it rises sends it back down)")
    p.add_argument("--frozen-after", type=int, default=8,
                   help="steps with no change at all before pressing USE (end-of-level screen) and warning")
    p.add_argument("--map-every", type=int, default=6, help="refresh the automap every N steps (0 = never)")
    p.add_argument("--show-map-every", type=int, default=30, help="print the automap every N steps (0 = never)")
    p.add_argument("--show-map-secs", type=float, default=0.0,
                   help="print the automap on a wall-clock cadence, every N seconds (0 = off, use --show-map-every "
                        "instead); handy in realtime mode where steps/second vary")

    r = ap.add_argument_group("Run control")
    r.add_argument("--check", action="store_true",
                   help="preflight only: test the Jev key/latency, the supervisor LLM (if --mode supervised) and "
                        "the board connection, then exit")
    r.add_argument("--steps", type=int, default=0, help="stop after N steps (0 = run until Ctrl+C)")
    r.add_argument("--duration", type=float, default=0, help="stop after S seconds")
    r.add_argument("--seed", type=int, default=None)
    r.add_argument("--log", help="append per-step JSONL telemetry to this file")
    r.add_argument("--quiet", action="store_true")
    r.add_argument("--color", choices=["auto", "always", "never"], default="auto",
                   help="colour the live step line (radar of what Jev sees + the macro it chose): "
                        "auto = on when the output is a terminal (default), always, or never")
    args = ap.parse_args()
    for stream in (sys.stdout, sys.stderr):      # Windows consoles/pipes: never crash on a character
        try:
            stream.reconfigure(errors="replace")
        except Exception:
            pass
    enable_color(args.color)

    if args.show_map_every and args.map_every and args.show_map_every % args.map_every:
        args.show_map_every = args.map_every * max(1, round(args.show_map_every / args.map_every))
    run(args)


if __name__ == "__main__":
    main()
