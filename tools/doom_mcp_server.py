#!/usr/bin/env python3
"""
MCP server that lets an LLM play DOOM on the physical SCINTIX P4.

It bridges to the device's `/agent` WebSocket (lockstep): each tool call sends an
action, the game advances a few tics, and the resulting observation is returned.
A single persistent connection is held for the whole session, so the game stays
paused between the model's moves.

What the model gets is deliberately limited to *what a player would see on the
rendered screen*: the things in `visible` are only those inside the field of view
and in line of sight — nothing behind the player or hidden behind walls — and
there are no targeting hints. All tactical decisions are the model's own.

Setup:
    pip install -r tools/requirements.txt        (needs Python 3.10+)
    set DOOM_WS_URL=ws://<device-ip>/agent        # Windows  (export on *nix)
    python tools/doom_mcp_server.py

Then register this script as an MCP server (stdio transport) in your MCP client.
Get the player into a level first (New Game from the menu) before driving it.
"""

import json
import os

from websockets.sync.client import connect
from mcp.server.fastmcp import FastMCP

WS_URL = os.environ.get("DOOM_WS_URL", "ws://192.168.4.1/agent")
DEFAULT_TICS = 8  # ~0.23 s of game time (Doom runs at 35 tics/second)

mcp = FastMCP("doom-scintix-p4")
_conn = None


def _ws():
    global _conn
    if _conn is None:
        _conn = connect(WS_URL, open_timeout=10)
    return _conn


def _step(action: dict) -> dict:
    """Send one action over the persistent WebSocket and return the observation."""
    global _conn
    try:
        w = _ws()
        w.send(json.dumps(action))
        return json.loads(w.recv())
    except Exception as exc:  # drop the connection so the next call reconnects
        _conn = None
        return {"valid": False, "error": str(exc)}


_OBS = (
    " Returns the observation after the step: player state "
    "{x, y, z, angle (0-359 compass heading: 0 = north, 90 = east, clockwise — "
    "same frame as the north-up automap), health, armor, weapon, ammo, episode, map} "
    "and `visible` — the things on screen right now (inside the field of view AND "
    "in line of sight), ordered left-to-right, each {name, dist (map units), "
    "bearing (degrees from where you face: 0 = centre, negative = left, "
    "positive = right)}. Anything behind you or hidden behind walls is NOT listed. "
    "Also `walls`: the distance (map units) to the nearest wall along rays across "
    "your field of view (bearing -45..45, 0 = straight ahead) — like judging depth "
    "in the view. Use these to gauge how far you can advance and to spot openings: "
    "a ray with a much larger distance than its neighbours is a passage/door. "
    "`door_ahead` (present only when a closed door/passage is in view) = "
    "{bearing, dist (both to the MIDDLE of the door; bearing can be beyond +/-45 when "
    "only its edge is in view)} — face its middle and press use to open it, and give it time: a "
    "door takes ~30 tics to rise clear, so use with tics>=30 (or keep advancing). If "
    "door_ahead reappears after you opened it, the door re-closed, so press use again "
    "instead of walking into it. `use_target` = what is under your crosshair within "
    "USE reach (64 units straight ahead): 'door', 'lift', 'switch', 'open_door' (already "
    "open: walk through, USE would close it) or null = nothing "
    "(turn to face the door's middle first); `use_locked` = a key door you have no "
    "key for. `exit_sign` = {bearing, dist} of a wall with EXIT written on it in view "
    "(an exit sign, door or switch): the level exit is that way. `switch_ahead` = "
    "{bearing, dist} of a switch panel on a wall in view, not flipped yet: a level ends "
    "when you press its exit switch, which sits in the room behind the EXIT door (often "
    "on a side wall, not under the sign). `items` = pickups "
    "lying in view within 1024 units, nearest first, each {name, kind (health / armor / "
    "ammo / weapon / key / powerup), dist, bearing}: walk over one to pick it up (Doom "
    "leaves a medikit at full health, ammo at the maximum). `missiles` = projectiles "
    "in flight in view (imp fireballs, rockets...), each {name, dist, bearing, dz "
    "(height above your feet), incoming}; an `incoming` one is on a course that hits "
    "you, with `eta` (tics until it does) and `dodge` (left / right: the strafe that "
    "takes you off its line). The status bar: `weapons` owned, `keys` held, `ammo_all` / "
    "`ammo_max` per ammo type (bullets, shells, cells, rockets). Doom switches to the "
    "fist by itself when the gun runs dry. `hurt` = {amount, from} when you lost health "
    "since the last step: `from` = ahead / left / right / behind (the side the hit came "
    "from, as the red flash and the status-bar face show it), null = no attacker "
    "(damaging floor) - hit from a side you cannot see: turn to it. `blocked` = true "
    "when your last move made no progress in the direction you asked for (you ran "
    "into a wall or a shut door)."
)


@mcp.tool(description="Look at the current scene without moving (advances no game time)." + _OBS)
def observe() -> dict:
    return _step({"tics": 0})


@mcp.tool(description="Walk forward for `tics` game tics (35 tics = 1 second)." + _OBS)
def move_forward(tics: int = DEFAULT_TICS) -> dict:
    return _step({"move": "forward", "tics": tics})


@mcp.tool(description="Walk backward for `tics` game tics." + _OBS)
def move_back(tics: int = DEFAULT_TICS) -> dict:
    return _step({"move": "back", "tics": tics})


@mcp.tool(description="Turn (rotate) left for `tics` tics; increase tics to turn more. "
                      "Brings left-side (negative-bearing) things toward the centre." + _OBS)
def turn_left(tics: int = DEFAULT_TICS) -> dict:
    return _step({"turn": "left", "tics": tics})


@mcp.tool(description="Turn (rotate) right for `tics` tics; increase tics to turn more. "
                      "Brings right-side (positive-bearing) things toward the centre." + _OBS)
def turn_right(tics: int = DEFAULT_TICS) -> dict:
    return _step({"turn": "right", "tics": tics})


@mcp.tool(description="Strafe (sidestep) left for `tics` tics, without turning." + _OBS)
def strafe_left(tics: int = DEFAULT_TICS) -> dict:
    return _step({"strafe": "left", "tics": tics})


@mcp.tool(description="Strafe (sidestep) right for `tics` tics, without turning." + _OBS)
def strafe_right(tics: int = DEFAULT_TICS) -> dict:
    return _step({"strafe": "right", "tics": tics})


@mcp.tool(description="Fire the current weapon for `tics` tics (hold to keep firing). Useful "
                      "reach: fist / chainsaw ~80, shotgun ~400, pistol ~500, chaingun ~600 - "
                      "farther shots mostly miss." + _OBS)
def fire(tics: int = DEFAULT_TICS) -> dict:
    return _step({"fire": True, "tics": tics})


@mcp.tool(description="Press Use: open doors, flip switches, operate lifts in front of you." + _OBS)
def use_(tics: int = DEFAULT_TICS) -> dict:
    return _step({"use": True, "tics": tics})


@mcp.tool(description="Select a weapon by slot (1=fist/chainsaw, 2=pistol, 3=shotgun, "
                      "4=chaingun, 5=rocket, 6=plasma, 7=BFG), if owned." + _OBS)
def select_weapon(slot: int) -> dict:
    return _step({"weapon": slot, "tics": 2})


@mcp.tool(description=(
    "Get the in-game automap as ASCII art — the SAME information the player's own "
    "automap shows: only the walls already discovered by exploring, plus your "
    "position. North is up. In `grid`: '#' = a discovered wall (or a closed door / a "
    "ledge too high to climb), ':' = a step edge you can walk over (stairs), '~' = "
    "the edge of a damaging floor (nukage/slime: costs health while you stand on it, but "
    "it is not a wall - crossing a short stretch is often the way on), 'E' = a wall with "
    "EXIT on it that you have seen (the level exit is there), '@' = you, ' ' = "
    "not yet discovered / open floor. Areas you haven't explored are blank, and no "
    "monsters or items are shown (the automap doesn't reveal them). Returns "
    "{grid:[rows], cols, rows, units_per_cell (map units per character), "
    "origin_x, origin_y (map coords of the top-left cell), player:{col,row,angle "
    "(compass heading, 0 = north/up, clockwise)}}. Advances no game time."))
def get_map() -> dict:
    return _step({"request": "map"})


if __name__ == "__main__":
    mcp.run()
