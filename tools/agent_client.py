#!/usr/bin/env python3
"""
Test client for the DOOM-on-SCINTIX-P4 agent WebSocket API (the `/agent`
lockstep endpoint).

It connects to the device, sends a JSON *action*, and prints the JSON
*observation* the game returns after advancing the requested tics.

Requires:  pip install websockets

Usage:
    python agent_client.py <device-ip>            # run a short demo sequence
    python agent_client.py <device-ip> --repl     # interactive control

REPL commands (optional integer = tics to advance, default 8):
    w [n]  forward      s [n]  back
    a [n]  turn left    d [n]  turn right
    q [n]  strafe left  e [n]  strafe right
    f [n]  fire         u [n]  use/open
    1..7   select weapon
    o      observe only (no movement)
    m      show the automap (ASCII, discovered walls only)
    raw {...json...}    send a raw action object
    quit / q!          exit

Action JSON (all fields optional):
    {"move":"forward|back","turn":"left|right","strafe":"left|right",
     "fire":true,"use":true,"weapon":1..7,"tics":N}
"""

import argparse
import asyncio
import json
import sys

try:
    import websockets
except ImportError:
    sys.exit("Missing dependency: pip install websockets")

DEFAULT_TICS = 8


def fmt_obs(obs: dict) -> str:
    if not obs.get("valid"):
        return "  <no game state — are you inside a level?>"
    lines = [
        f"  pos=({obs['x']},{obs['y']},{obs['z']})  angle={obs['angle']}deg"
        f"  hp={obs['health']} armor={obs['armor']}  weapon={obs['weapon']} ammo={obs['ammo']}"
        f"  E{obs['episode']}M{obs['map']}",
    ]
    walls = obs.get("walls", [])
    if walls:
        lines.append("  walls (bearing:dist): " + "  ".join(f"{w['bearing']:+d}:{w['dist']}" for w in walls))
    visible = obs.get("visible", [])
    if visible:
        lines.append(f"  in view ({len(visible)}), left-to-right:")
        for v in visible:
            lines.append(f"    {v['name']:<18} dist={v['dist']:>6}  bearing={v['bearing']:>4}")
    else:
        lines.append("  in view: nothing")
    return "\n".join(lines)


def fmt_map(m: dict) -> str:
    if not m.get("valid"):
        return "  <no map — are you inside a level?>"
    p = m.get("player", {})
    lines = [f"  automap  scale={m['units_per_cell']} units/cell  "
             f"player col={p.get('col')} row={p.get('row')} angle={p.get('angle')}deg  (north up)"]
    lines += ["  " + row for row in m.get("grid", [])]
    return "\n".join(lines)


async def send(ws, action: dict):
    await ws.send(json.dumps(action))
    resp = json.loads(await ws.recv())
    print(fmt_map(resp) if "grid" in resp else fmt_obs(resp))
    return resp


async def demo(ws):
    print("[observe]");        await send(ws, {"tics": 0})
    print("[forward x12]");    await send(ws, {"move": "forward", "tics": 12})
    print("[turn right x8]");  await send(ws, {"turn": "right", "tics": 8})
    print("[fire x6]");        await send(ws, {"fire": True, "tics": 6})
    print("[weapon 2]");       await send(ws, {"weapon": 2, "tics": 2})


SHORTHAND = {
    "w": ("move", "forward"), "s": ("move", "back"),
    "a": ("turn", "left"),    "d": ("turn", "right"),
    "q": ("strafe", "left"),  "e": ("strafe", "right"),
    "f": ("fire", True),      "u": ("use", True),
}


def parse_cmd(line: str):
    line = line.strip()
    if not line:
        return None
    if line in ("quit", "q!", "exit"):
        return "quit"
    if line.startswith("raw "):
        try:
            return json.loads(line[4:])
        except json.JSONDecodeError as exc:
            print(f"  bad json: {exc}")
            return None
    parts = line.split()
    tok = parts[0]
    tics = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else DEFAULT_TICS
    if tok == "o":
        return {"tics": 0}
    if tok == "m":
        return {"request": "map"}
    if tok.isdigit() and 1 <= int(tok) <= 7:
        return {"weapon": int(tok), "tics": 2}
    if tok in SHORTHAND:
        key, val = SHORTHAND[tok]
        return {key: val, "tics": tics}
    print("  unknown command (try: w/s/a/d/q/e/f/u, 1-7, o, m, raw {...}, quit)")
    return None


async def repl(ws):
    print("Connected. Commands: w/s/a/d=move/turn, q/e=strafe, f=fire, u=use, 1-7=weapon, "
          "o=observe, m=automap, quit.")
    loop = asyncio.get_event_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            break
        cmd = parse_cmd(line)
        if cmd is None:
            continue
        if cmd == "quit":
            break
        await send(ws, cmd)


async def main():
    ap = argparse.ArgumentParser(description="DOOM SCINTIX P4 agent WebSocket client")
    ap.add_argument("host", help="device IP or host (e.g. 192.168.1.42)")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--repl", action="store_true", help="interactive control instead of the demo")
    args = ap.parse_args()

    uri = f"ws://{args.host}:{args.port}/agent"
    print(f"connecting to {uri} ...")
    async with websockets.connect(uri) as ws:
        if args.repl:
            await repl(ws)
        else:
            await demo(ws)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
