# SCINTIX P4 DOOM — agent tools

Tools to drive DOOM running on the SCINTIX P4 from a computer, and to let an LLM
play it via MCP. They talk to the firmware's **`/agent` WebSocket** (the
`ai-agent-api` branch), which runs the game in **lockstep**: you send an action,
the game advances a few tics, and you get back a structured observation.

| File | What it is |
|------|------------|
| `agent_client.py` | Test client / interactive REPL to drive the game by hand |
| `doom_mcp_server.py` | MCP server exposing the game as tools for an LLM |
| `requirements.txt` | Python deps (`websockets`, `mcp`) |

## 0. Prerequisites

- Firmware from the **`ai-agent-api`** branch flashed on the device.
- The device on the network — note its IP from the serial log (`got ip ...`), or
  use `192.168.4.1` if it's running the captive-portal AP (`SCINTIX-P4-XXXXXX`).
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

## Protocol (for reference)

**Action** (client → device), all fields optional:

```json
{"move":"forward|back","turn":"left|right","strafe":"left|right",
 "fire":true,"use":true,"weapon":1-7,"tics":N}
```
`tics` (default 7) = game tics to advance this step (35 tics ≈ 1 second).

**Observation** (device → client):

```json
{"valid":true,"x":..,"y":..,"z":..,"angle":0-359,
 "health":..,"armor":..,"weapon":"shotgun","ammo":..,"episode":..,"map":..,
 "visible":[{"name":"imp","dist":410,"bearing":-12}, ...]}
```

`visible` lists **only what the player can see on screen** — things inside the
field of view *and* in line of sight, ordered left-to-right. `bearing` is degrees
from where you face (0 = centre, negative = left). Enemies behind you or hidden
behind walls are **not** reported, and there are no targeting hints: the model
makes all tactical decisions itself.

**Automap** — send `{"request":"map"}` (or `{"map":true}`) and the device replies
with the ASCII automap, the same information the in-game automap shows:

```json
{"valid":true,"cols":64,"rows":32,"units_per_cell":N,"origin_x":..,"origin_y":..,
 "player":{"col":..,"row":..,"angle":..},"grid":["  ## ", "#  @#", ...]}
```
`grid` holds only the walls **already discovered** by exploring (`#`), the player
(`@`) and blanks for the rest; north is up. No monsters or items — exactly like
the player's own automap.

## Notes

- The device accepts **one** agent connection at a time.
- While an agent is connected the game is **paused between steps** (the screen
  freezes on the last frame). Disconnect and it resumes real-time play
  (keyboard/gamepad).
