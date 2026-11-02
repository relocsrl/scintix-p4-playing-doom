#!/usr/bin/env python3
"""
Simulator of the SCINTIX P4 DOOM `/agent` WebSocket API — for developing and
testing agents (doom_jev_agent.py, doom_mcp_server.py, agent_client.py) with
no board attached.

It speaks the same protocol as the firmware (main/doom_agent.c + doom_port.c):
an action JSON in, an observation JSON out (51-ray depth fan, visible things in
FOV + line of sight, door_ahead, blocked), and {"request":"map"} -> the ASCII
automap of the walls discovered so far. It also emulates the optional
free-running mode of the firmware patch ({"hold": true}: keys stay held, a 35 Hz
clock keeps the game running, the reply is immediate). Under the hood is a small 2D world
(two rooms, a corridor with a door that opens on `use` and re-closes, a few
imps that walk toward you and bite, a barrel, an EXIT at the end of the dead-end
spur north of the second room -> stats screen until USE -> next map; a dead
marine restarts the level on USE) with Doom-like movement numbers
(momentum + friction, slow/fast turn, 35 tics/s). It is an approximation for
exercising the control loop, not a Doom engine.

Requires:  pip install websockets

Usage:
    python doom_agent_sim.py                 # ws://localhost:8765/agent
    python doom_agent_sim.py --realtime      # sleep ~28.6 ms per tic like the device
    python doom_agent_sim.py --port 80       # so clients can use plain ws://localhost/agent

Then, e.g.:
    python doom_jev_agent.py localhost --port 8765 --jev dry --steps 200
    python agent_client.py localhost --port 8765 --repl
"""

import argparse
import json
import math
import random
import threading
import time

try:
    from websockets.sync.server import serve
except ImportError:  # pragma: no cover
    raise SystemExit("Missing dependency: pip install websockets")

TICRATE = 35
TIC_SECONDS = 1.0 / TICRATE
RAY_RANGE = 2048
NUM_RAYS = 51
FOV = 45
PLAYER_RADIUS = 16
USE_RANGE = 64  # Doom USERANGE, from the player centre to the line
DOOR_HEIGHT = 128.0        # a typical Doom door (units)
DOOR_SPEED = 2.0           # VDOORSPEED: units per tic -> fully open in 64 tics, passable after ~28
DOOR_PASSABLE_HEIGHT = 56  # the firmware reports door_ahead while the opening is < 56 units
DOOR_STAY_OPEN_TICS = 150  # VDOORWAIT: then it closes again (~4.3 s)
CATCHUP_TICS = 4           # the real device runs a few catch-up tics after a pause

# Doom movement constants (see g_game.c / p_user.c)
FORWARD_ACCEL = 25 * 2048 / 65536      # units/tic^2 while the key is held (walk)
SIDE_ACCEL = 24 * 2048 / 65536
FRICTION = 0.90625
SLOW_TURN_TICS = 6
SLOW_TURN_DEG = 320 * 360 / 65536
FAST_TURN_DEG = 640 * 360 / 65536

MAP_COLS, MAP_ROWS = 64, 32
EXIT_Y = 1330                 # the dead-end spur north of room B ends in the exit
EXIT_X = (1900, 2028)


# ----------------------------------------------------------------------------
# Geometry
# ----------------------------------------------------------------------------

class Wall:
    """A one-sided solid line, or a Doom-style vertical door (two-sided line whose
    opening height decides whether it blocks: passable once it is > 56 units up)."""

    def __init__(self, x1, y1, x2, y2, door=False, name=""):
        self.x1, self.y1, self.x2, self.y2 = float(x1), float(y1), float(x2), float(y2)
        self.door = door
        self.name = name
        self.mapped = False
        # door state machine: closed -> opening -> open (waits) -> closing -> closed
        self.height = 0.0
        self.state = "closed"
        self.wait_timer = 0

    def blocking(self):
        return not (self.door and self.height > DOOR_PASSABLE_HEIGHT)

    @property
    def is_open(self):
        return self.door and self.height > DOOR_PASSABLE_HEIGHT

    def tick(self, player_inside):
        if not self.door or self.state == "closed":
            return
        if self.state == "opening":
            self.height = min(DOOR_HEIGHT, self.height + DOOR_SPEED)
            if self.height >= DOOR_HEIGHT:
                self.state = "open"
                self.wait_timer = DOOR_STAY_OPEN_TICS
        elif self.state == "open":
            self.wait_timer -= 1
            if self.wait_timer <= 0:
                self.state = "closing"
        elif self.state == "closing":
            if player_inside:
                self.state = "opening"        # Doom: a door closing on something goes back up
                return
            self.height = max(0.0, self.height - DOOR_SPEED)
            if self.height <= 0:
                self.state = "closed"

    def activate(self):
        """Doom EV_VerticalDoor semantics for a repeatable door: USE on a closed
        door opens it; USE while it is opening or open sends it back DOWN; USE while
        closing sends it back up. (So spamming USE fights the door.)"""
        if not self.door:
            return None
        if self.state == "closed":
            self.state = "opening"
            return "opening"
        if self.state in ("opening", "open"):
            self.state = "closing"
            return "closing (use pressed again!)"
        if self.state == "closing":
            self.state = "opening"
            return "reopening"
        return None


def seg_intersect(px, py, dx, dy, w):
    """Ray (px,py)+t*(dx,dy), t>=0 vs segment w. Returns t or None."""
    ex, ey = w.x2 - w.x1, w.y2 - w.y1
    den = dx * ey - dy * ex
    if abs(den) < 1e-9:
        return None
    t = ((w.x1 - px) * ey - (w.y1 - py) * ex) / den
    u = ((w.x1 - px) * dy - (w.y1 - py) * dx) / den
    if t >= 0 and 0.0 <= u <= 1.0:
        return t
    return None


def point_seg_dist(px, py, w):
    ex, ey = w.x2 - w.x1, w.y2 - w.y1
    L2 = ex * ex + ey * ey
    if L2 == 0:
        return math.hypot(px - w.x1, py - w.y1)
    t = max(0.0, min(1.0, ((px - w.x1) * ex + (py - w.y1) * ey) / L2))
    return math.hypot(px - (w.x1 + t * ex), py - (w.y1 + t * ey))


def build_world():
    """Two rooms joined by a corridor with a door, a dead-end spur, a barrel."""
    W = []

    def rect(x1, y1, x2, y2, gaps=(), name=""):
        # axis-aligned room, with optional gaps (side, a, b) removed from a wall
        sides = {
            "N": (x1, y2, x2, y2), "S": (x1, y1, x2, y1),
            "W": (x1, y1, x1, y2), "E": (x2, y1, x2, y2),
        }
        for side, (ax, ay, bx, by) in sides.items():
            g = [gp for gp in gaps if gp[0] == side]
            if not g:
                W.append(Wall(ax, ay, bx, by, name=f"{name}{side}"))
                continue
            horizontal = side in ("N", "S")
            lo, hi = (min(ax, bx), max(ax, bx)) if horizontal else (min(ay, by), max(ay, by))
            cuts = sorted((min(a, b), max(a, b)) for _, a, b in g)
            cur = lo
            for a, b in cuts:
                if a > cur:
                    W.append(Wall(cur, ay, a, ay, name=name + side) if horizontal else Wall(ax, cur, ax, a, name=name + side))
                cur = b
            if cur < hi:
                W.append(Wall(cur, ay, hi, ay, name=name + side) if horizontal else Wall(ax, cur, ax, hi, name=name + side))

    # Room A (start), opening east at y 256..384
    rect(0, 0, 768, 640, gaps=[("E", 256, 384)], name="A")
    # Corridor A->B, x 768..1536, y 256..384, open both ends
    W.append(Wall(768, 384, 1536, 384, name="corrN"))
    W.append(Wall(768, 256, 1536, 256, name="corrS"))
    # Door across the corridor at x = 1152
    W.append(Wall(1152, 256, 1152, 384, door=True, name="door1"))
    # Room B, opening west at y 256..384, north spur at x 1900..2028
    rect(1536, -128, 2560, 768, gaps=[("W", 256, 384), ("N", 1900, 2028)], name="B")
    # Dead-end spur north of room B
    W.append(Wall(1900, 768, 1900, 1400, name="spurW"))
    W.append(Wall(2028, 768, 2028, 1400, name="spurE"))
    W.append(Wall(1900, 1400, 2028, 1400, name="spurN"))
    # A pillar in room B to make the depth fan interesting
    rect(2100, 100, 2180, 180, name="pillar")
    return W


class Thing:
    def __init__(self, name, x, y, hp, speed=0.0, melee=0, radius=20):
        self.name = name
        self.x, self.y = float(x), float(y)
        self.hp = hp
        self.speed = speed        # units/tic toward the player when it has LOS
        self.melee = melee        # damage per bite (every 35 tics, within 64 units)
        self.radius = radius
        self.bite_timer = 0
        self.alive = True


# ----------------------------------------------------------------------------
# World state
# ----------------------------------------------------------------------------

class World:
    def __init__(self, seed=1, realtime=False, verbose=False, catchup=CATCHUP_TICS, menu_secs=0.0,
                 spawn=None, hp=100):
        self.lock = threading.RLock()
        self.spawn = spawn          # (x, y, compass heading) or None for the level start
        self.start_hp = hp
        self.seed = seed
        self.realtime = realtime
        self.verbose = verbose
        self.catchup = catchup
        self.menu_secs = menu_secs
        self.map = 1
        self.deaths = 0
        self.last_reply_t = time.perf_counter()
        self.reset(menu=True)

    def reset(self, menu=False):
        """Fresh level: walls undiscovered, monsters alive, player at the start.
        menu=True also replays the --menu-secs "title screen" phase (new connection)."""
        self.rng = random.Random(self.seed)
        self.walls = build_world()
        self.things = [
            Thing("imp", 2200, 200, 60, speed=1.4, melee=6),
            Thing("imp", 2300, 520, 60, speed=1.4, melee=6),
            Thing("zombieman", 1000, 330, 20, speed=1.0, melee=4),
            Thing("barrel", 1900, 300, 20, radius=10),
        ]
        self.reset_player()
        self.gametic = 0
        self.steps = 0
        self.level_start = time.time() + (self.menu_secs if menu else 0)   # before this: no live player
        self.intermission = False   # end-of-level stats screen: frozen until USE/FIRE is pressed
        # realtime ("hold") mode: keys stay held while a 35 Hz ticker runs the game
        self.realtime_mode = False
        self.hold_keys = {}
        self.hold_until_tic = 0
        self.rt_prev_keys = {}
        self.rt_prev_pos = (self.px, self.py, self.doom_angle)
        self.rt_prev_tic = 0

    def in_level(self):
        return time.time() >= self.level_start

    def reset_player(self):
        self.px, self.py = 200.0, 320.0
        self.doom_angle = 0.0             # degrees, 0 = east, counter-clockwise (Doom)
        if self.spawn and self.map == 1 and self.deaths == 0:   # test scenarios: first life of E1M1 only
            self.px, self.py = float(self.spawn[0]), float(self.spawn[1])
            self.doom_angle = (90 - self.spawn[2]) % 360
        self.vx = self.vy = 0.0
        self.turnheld = 0
        self.health = self.start_hp if (self.map == 1 and self.deaths == 0) else 100
        self.armor = 0
        self.ammo = 50
        self.weapon = "pistol"
        self.fire_timer = 0
        self.use_latched = False
        self.attack_latched = False

    # -- helpers -----------------------------------------------------------
    def compass(self):
        return int((450 - round(self.doom_angle)) % 360)

    def blocking_walls(self):
        return [w for w in self.walls if w.blocking()]

    def collide(self, x, y):
        for w in self.blocking_walls():
            if point_seg_dist(x, y, w) < PLAYER_RADIUS:
                return True
        for t in self.things:
            if t.alive and math.hypot(t.x - x, t.y - y) < PLAYER_RADIUS + t.radius:
                return True
        return False

    def try_move(self, dx, dy):
        nx, ny = self.px + dx, self.py + dy
        if not self.collide(nx, ny):
            self.px, self.py = nx, ny
            return True
        if not self.collide(nx, self.py):
            self.px = nx
            self.vy = 0
            return True
        if not self.collide(self.px, ny):
            self.py = ny
            self.vx = 0
            return True
        self.vx = self.vy = 0
        return False

    def los(self, x, y):
        dx, dy = x - self.px, y - self.py
        dist = math.hypot(dx, dy)
        if dist == 0:
            return True
        for w in self.blocking_walls():
            t = seg_intersect(self.px, self.py, dx, dy, w)
            if t is not None and t < 1.0:
                return False
        return True

    def bearing_to(self, x, y):
        """Signed degrees, positive = RIGHT of the facing (matches firmware)."""
        ang = math.degrees(math.atan2(y - self.py, x - self.px))
        rel = (ang - self.doom_angle + 540) % 360 - 180   # + = left in Doom terms
        return -rel

    # -- one game tic ------------------------------------------------------
    def tic(self, keys):
        self.gametic += 1
        use_edge = bool(keys.get("use")) and not self.use_latched
        fire_edge = bool(keys.get("fire")) and not self.attack_latched
        if self.intermission or self.health <= 0:
            # Frozen world: the stats screen (WI_checkForAccelerate: USE or FIRE, edge)
            # or a dead marine (P_DeathThink: USE -> PST_REBORN, the level reloads).
            self.use_latched = bool(keys.get("use"))
            self.attack_latched = bool(keys.get("fire"))
            if self.intermission and (use_edge or fire_edge):
                self.map += 1
                if self.verbose:
                    print(f"  [sim] intermission over -> E1M{self.map}", flush=True)
                self.reset()
            elif self.health <= 0 and use_edge:
                if self.verbose:
                    print("  [sim] reborn: level restarts", flush=True)
                self.reset()
            if self.realtime:
                time.sleep(TIC_SECONDS)
            return
        self.attack_latched = bool(keys.get("fire"))
        # turning (Doom accelerative turn)
        turn = 0
        if keys.get("turn") == "left":
            turn = 1
        elif keys.get("turn") == "right":
            turn = -1
        if turn:
            self.turnheld += 1
            step = SLOW_TURN_DEG if self.turnheld <= SLOW_TURN_TICS else FAST_TURN_DEG
            self.doom_angle = (self.doom_angle + turn * step) % 360
        else:
            self.turnheld = 0
        # thrust
        fwd = {"forward": 1, "back": -1}.get(keys.get("move"), 0)
        side = {"right": 1, "left": -1}.get(keys.get("strafe"), 0)
        a = math.radians(self.doom_angle)
        if fwd:
            self.vx += fwd * FORWARD_ACCEL * math.cos(a)
            self.vy += fwd * FORWARD_ACCEL * math.sin(a)
        if side:
            self.vx += side * SIDE_ACCEL * math.sin(a)
            self.vy -= side * SIDE_ACCEL * math.cos(a)
        self.try_move(self.vx, self.vy)
        self.vx *= FRICTION
        self.vy *= FRICTION
        if abs(self.vx) < 0.05:
            self.vx = 0.0
        if abs(self.vy) < 0.05:
            self.vy = 0.0
        # use: like P_UseLines, trace 64 units along the view direction and
        # activate the first door line crossed (no "facing the centre" test)
        if keys.get("use") and not self.use_latched:
            self.use_latched = True   # Doom fires USE once per press (BT_USE edge), not every tic
            a_rad = math.radians(self.doom_angle)
            dx, dy = math.cos(a_rad), math.sin(a_rad)
            best_t, best_w = None, None
            for w in self.walls:
                t = seg_intersect(self.px, self.py, dx, dy, w)
                if t is not None and t <= USE_RANGE and (best_t is None or t < best_t):
                    best_t, best_w = t, w
            if best_w is not None and best_w.door:
                res = best_w.activate()
                if res and self.verbose:
                    print(f"  [sim] door {res}", flush=True)
        if not keys.get("use"):
            self.use_latched = False
        # doors
        for w in self.walls:
            if w.door:
                w.tick(point_seg_dist(self.px, self.py, w) < 40)
        # fire
        if self.fire_timer > 0:
            self.fire_timer -= 1
        if keys.get("fire") and self.ammo > 0 and self.fire_timer == 0:
            self.fire_timer = 10           # one pistol shot per 10 tics
            self.ammo -= 1
            self.shoot()
        # monsters
        for t in self.things:
            if not t.alive or t.speed == 0:
                continue
            d = math.hypot(t.x - self.px, t.y - self.py)
            if d < 1200 and self.los(t.x, t.y):
                if d > 48:
                    nx = t.x + (self.px - t.x) / d * t.speed
                    ny = t.y + (self.py - t.y) / d * t.speed
                    if not any(point_seg_dist(nx, ny, w) < t.radius for w in self.blocking_walls()):
                        t.x, t.y = nx, ny
                if d < 64:
                    t.bite_timer += 1
                    if t.bite_timer >= 35:
                        t.bite_timer = 0
                        self.health = max(0, self.health - t.melee)
                        if self.verbose:
                            print(f"  [sim] {t.name} bites, hp={self.health}", flush=True)
                        if self.health <= 0:
                            self.deaths += 1
                            if self.verbose:
                                print("  [sim] the marine died", flush=True)
        if EXIT_X[0] < self.px < EXIT_X[1] and self.py >= EXIT_Y and not self.intermission:
            self.intermission = True
            if self.verbose:
                print(f"  [sim] EXIT reached: E1M{self.map} complete, stats screen (press USE)", flush=True)
        if self.realtime:
            time.sleep(TIC_SECONDS)

    def shoot(self):
        best, best_b = None, 999
        for t in self.things:
            if not t.alive:
                continue
            b = self.bearing_to(t.x, t.y)
            d = math.hypot(t.x - self.px, t.y - self.py)
            half = math.degrees(math.atan2(t.radius, max(d, 1))) + 2.0
            if abs(b) <= half and self.los(t.x, t.y) and abs(b) < best_b:
                best, best_b = t, abs(b)
        if best is not None:
            best.hp -= self.rng.choice((5, 10, 15))
            if best.hp <= 0:
                best.alive = False
                if self.verbose:
                    print(f"  [sim] {best.name} killed", flush=True)

    # -- observation -----------------------------------------------------------
    def cast(self, bearing_right):
        """Distance and is_door for a ray at `bearing_right` (+ = right)."""
        ang = math.radians(self.doom_angle - bearing_right)
        dx, dy = math.cos(ang), math.sin(ang)
        best_t, best_w = None, None
        for w in self.blocking_walls():
            t = seg_intersect(self.px, self.py, dx, dy, w)
            if t is not None and (best_t is None or t < best_t):
                best_t, best_w = t, w
        if best_t is None or best_t > RAY_RANGE:
            return RAY_RANGE, None
        best_w.mapped = True
        return int(best_t), best_w if best_w.door else None

    def use_probe(self):
        """What a USE pressed now would hit: the first line within USE_RANGE along the
        facing (the same trace as the `use` key above and as P_UseLines)."""
        a_rad = math.radians(self.doom_angle)
        dx, dy = math.cos(a_rad), math.sin(a_rad)
        best_t, best_w = None, None
        for w in self.walls:
            t = seg_intersect(self.px, self.py, dx, dy, w)
            if t is not None and t <= USE_RANGE and (best_t is None or t < best_t):
                best_t, best_w = t, w
        return "door" if best_w is not None and best_w.door else None

    def observation(self, blocked=False):
        walls = []
        door_wall, door_b = None, None
        mid = (NUM_RAYS - 1) // 2
        for i in range(NUM_RAYS):
            bearing = int((i - mid) * 2 * FOV / (NUM_RAYS - 1))   # C division truncates toward zero
            dist, hit_door = self.cast(bearing)
            walls.append({"bearing": bearing, "dist": dist})
            if hit_door is not None and (door_b is None or abs(bearing) < abs(door_b)):
                door_wall, door_b = hit_door, bearing
        door = None
        if door_wall is not None:
            # like the firmware: bearing / dist to the middle of the door line
            mx, my = (door_wall.x1 + door_wall.x2) / 2, (door_wall.y1 + door_wall.y2) / 2
            door = {"bearing": int(round(self.bearing_to(mx, my))),
                    "dist": int(math.hypot(mx - self.px, my - self.py)), "usable": True}
        visible = []
        for t in self.things:
            if not t.alive:
                continue
            b = self.bearing_to(t.x, t.y)
            if abs(b) > FOV:
                continue
            if not self.los(t.x, t.y):
                continue
            visible.append({"name": t.name, "dist": int(math.hypot(t.x - self.px, t.y - self.py)),
                            "bearing": int(round(b))})
        visible.sort(key=lambda v: v["bearing"])
        return {
            "valid": True,
            "x": int(self.px), "y": int(self.py), "z": 0,
            "angle": self.compass(),
            "health": self.health, "armor": self.armor,
            "weapon": self.weapon, "ammo": self.ammo,
            "episode": 1, "map": self.map,
            "visible": visible[:16],
            "walls": walls,
            **({"door_ahead": door} if door else {}),
            "use_target": self.use_probe(),
            "blocked": blocked,
        }

    def automap(self):
        mapped = [w for w in self.walls if w.mapped]
        px, py = int(self.px), int(self.py)
        minx = maxx = px
        miny = maxy = py
        for w in mapped:
            for x, y in ((w.x1, w.y1), (w.x2, w.y2)):
                minx, maxx = min(minx, int(x)), max(maxx, int(x))
                miny, maxy = min(miny, int(y)), max(maxy, int(y))
        upc = max((maxx - minx) // (MAP_COLS - 1) + 1, (maxy - miny) // (MAP_ROWS - 1) + 1, 16)
        grid = [[" "] * MAP_COLS for _ in range(MAP_ROWS)]

        def plot(c1, r1, c2, r2):
            dx, sx = abs(c2 - c1), 1 if c1 < c2 else -1
            dy, sy = -abs(r2 - r1), 1 if r1 < r2 else -1
            err = dx + dy
            while True:
                if 0 <= c1 < MAP_COLS and 0 <= r1 < MAP_ROWS:
                    grid[r1][c1] = "#"
                if c1 == c2 and r1 == r2:
                    break
                e2 = 2 * err
                if e2 >= dy:
                    err += dy
                    c1 += sx
                if e2 <= dx:
                    err += dx
                    r1 += sy

        for w in mapped:
            plot((int(w.x1) - minx) // upc, (maxy - int(w.y1)) // upc,
                 (int(w.x2) - minx) // upc, (maxy - int(w.y2)) // upc)
        pc, pr = (px - minx) // upc, (maxy - py) // upc
        if 0 <= pr < MAP_ROWS and 0 <= pc < MAP_COLS:
            grid[pr][pc] = "@"
        return {
            "valid": True, "cols": MAP_COLS, "rows": MAP_ROWS, "units_per_cell": upc,
            "origin_x": minx, "origin_y": maxy,
            "player": {"col": pc, "row": pr, "angle": self.compass()},
            "grid": ["".join(r) for r in grid],
        }

    # -- realtime ("hold") mode ---------------------------------------------------
    def ticker(self, stop_event):
        """Background 35 Hz game clock, active only while an agent holds keys."""
        next_t = time.perf_counter()
        while not stop_event.is_set():
            next_t += TIC_SECONDS
            with self.lock:
                if self.realtime_mode and self.in_level():
                    keys = self.hold_keys if self.gametic < self.hold_until_tic else {}
                    saved, self.realtime = self.realtime, False   # never double-sleep
                    self.tic(keys)
                    self.realtime = saved
            delay = next_t - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_t = time.perf_counter()

    def hold_step(self, action, keys, tics):
        """Firmware realtime mode: swap the held keys, run one tic, reply at once.
        `blocked` refers to the PREVIOUS held action, between the two requests."""
        if not self.realtime_mode:
            self.realtime_mode = True
            self.rt_prev_keys = {}
            self.rt_prev_pos = (self.px, self.py, self.doom_angle)
            self.rt_prev_tic = self.gametic
            if self.verbose:
                print("  [sim] agent: realtime (free-running) mode", flush=True)
        # threshold scales with the tics elapsed since the previous request (a marine
        # starting from rest covers only ~0.78*n*(n+1)/2 units in n tics), like the firmware
        elapsed = self.gametic - self.rt_prev_tic
        blocked = self.compute_blocked(self.rt_prev_keys, *self.rt_prev_pos, thr=max(3, min(16, elapsed * elapsed // 3)))
        self.hold_keys = keys
        self.hold_until_tic = self.gametic + max(1, min(175, tics if tics > 0 else 35))
        saved, self.realtime = self.realtime, False
        self.tic(keys)
        self.realtime = saved
        self.rt_prev_keys = keys
        self.rt_prev_pos = (self.px, self.py, self.doom_angle)
        self.rt_prev_tic = self.gametic
        self.steps += 1
        self.last_reply_t = time.perf_counter()
        obs = self.observation(blocked)
        if self.verbose:
            print(f"[sim {self.steps:4d}] HOLD tics={tics:2d} keys={compact_keys(keys)} -> pos=({obs['x']},{obs['y']}) "
                  f"hd={obs['angle']:3d} hp={obs['health']} ammo={obs['ammo']} vis={len(obs['visible'])} "
                  f"door={'Y' if obs.get('door_ahead') else '-'} blk={'Y' if blocked else '-'}", flush=True)
        return obs

    def leave_realtime(self):
        if self.realtime_mode:
            self.realtime_mode = False
            self.hold_keys = {}
            self.use_latched = False
            if self.verbose:
                print("  [sim] agent: back to lockstep mode", flush=True)

    def compute_blocked(self, keys, bx, by, start_angle, thr=16):
        if not (keys.get("move") or keys.get("strafe")):
            return False
        dx, dy = self.px - bx, self.py - by
        a = math.radians(start_angle)
        fwd = dx * math.cos(a) + dy * math.sin(a)
        side = dx * math.sin(a) - dy * math.cos(a)
        blocked = True
        if keys.get("move") == "forward" and fwd >= thr:
            blocked = False
        if keys.get("move") == "back" and fwd <= -thr:
            blocked = False
        if keys.get("strafe") and abs(side) >= thr:
            blocked = False
        return blocked

    # -- one agent step ---------------------------------------------------------
    def step(self, action):
        tics = max(0, min(60, int(action.get("tics", 7) or 0)))
        keys = {
            "move": parse_axis(action.get("move"), "back", "forward", ("back", "forward")),
            "turn": parse_axis(action.get("turn"), "left", "right", ("left", "right")),
            "strafe": parse_axis(action.get("strafe"), "left", "right", ("left", "right")),
            "fire": bool(action.get("fire")),
            "use": bool(action.get("use")),
        }
        w = action.get("weapon")
        if isinstance(w, int) and 1 <= w <= 7:
            self.weapon = {1: "fist", 2: "pistol", 3: "shotgun", 4: "chaingun", 5: "rocket_launcher",
                           6: "plasma_rifle", 7: "bfg9000"}[w] if w == 2 else self.weapon  # only the pistol is owned
        if action.get("hold") is True:
            return self.hold_step(action, keys, tics)
        self.leave_realtime()
        bx, by = self.px, self.py
        start_angle = self.doom_angle
        # The board's first doomgeneric_Tick after a pause runs min(5, max(1, n))
        # buffered tics for a pause of n tics (vanilla sync), with the new keys held.
        pause_tics = int((time.perf_counter() - self.last_reply_t) * TICRATE)
        extra = min(self.catchup, max(0, pause_tics - 1))
        if tics > 0:
            for _ in range(tics + extra):
                self.tic(keys)
            self.tic({})  # flush tick with keys released
        else:
            for _ in range(1 + extra):  # observe: the flush tick alone, with its catch-up
                self.tic({})
        self.turnheld = 0
        # blocked, as the firmware computes it: displacement projected on facing/side axes
        blocked = self.compute_blocked(keys, bx, by, start_angle)
        self.steps += 1
        obs = self.observation(blocked)
        self.last_reply_t = time.perf_counter()
        if self.verbose:
            print(f"[sim {self.steps:4d}] tics={tics:2d}+{extra} keys={compact_keys(keys)} -> pos=({obs['x']},{obs['y']}) "
                  f"hd={obs['angle']:3d} hp={obs['health']} ammo={obs['ammo']} vis={len(obs['visible'])} "
                  f"door={'Y' if obs.get('door_ahead') else '-'} blk={'Y' if blocked else '-'}", flush=True)
        return obs


def parse_axis(v, neg, pos, names):
    if isinstance(v, str):
        return v if v in names else None
    if isinstance(v, (int, float)):
        return pos if v > 0 else (neg if v < 0 else None)
    return None


def compact_keys(keys):
    parts = [f"{k}={v}" for k, v in keys.items() if v]
    return ",".join(parts) or "-"


# ----------------------------------------------------------------------------
# WebSocket server
# ----------------------------------------------------------------------------

def make_handler(world, persist):
    def handler(ws):
        print(f"[sim] agent connected from {ws.remote_address}", flush=True)
        with world.lock:
            if not persist:
                world.map = 1
                world.deaths = 0
                world.reset(menu=True)
        try:
            for message in ws:
                try:
                    req = json.loads(message)
                except json.JSONDecodeError:
                    req = {}
                if not isinstance(req, dict):
                    req = {}
                with world.lock:
                    if req.get("request") == "map" or req.get("map") is True:
                        reply = world.automap() if world.in_level() else {"valid": False}
                    elif not world.in_level():
                        reply = {"valid": False}   # title screen / menu: nothing to observe
                    else:
                        reply = world.step(req)
                ws.send(json.dumps(reply))
        finally:
            with world.lock:
                world.leave_realtime()
            print("[sim] agent disconnected (real-time play would resume)", flush=True)
    return handler


def main():
    ap = argparse.ArgumentParser(description="Simulator of the SCINTIX P4 DOOM /agent WebSocket API")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--realtime", action="store_true", help="sleep 1/35 s per game tic like the real device")
    ap.add_argument("--catchup", type=int, default=CATCHUP_TICS,
                    help="max extra tics run with the keys held at the start of a lockstep step after a pause "
                         "(the board's catch-up; the actual value depends on the pause, like on the board)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--menu-secs", type=float, default=0.0,
                    help="pretend to sit at the title/menu (valid:false) for this long after each (re)start")
    ap.add_argument("--spawn", help="test scenario: start the first life at X,Y,HEADING (compass degrees), "
                                    "e.g. 1964,1150,0 = just south of the exit, facing it")
    ap.add_argument("--hp", type=int, default=100, help="test scenario: health of the first life")
    ap.add_argument("--persist", action="store_true",
                    help="keep the world state across agent connections (default: reset on each connect)")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    spawn = tuple(float(v) for v in args.spawn.split(",")) if args.spawn else None
    world = World(seed=args.seed, realtime=args.realtime, verbose=args.verbose, catchup=args.catchup,
                  menu_secs=args.menu_secs, spawn=spawn, hp=args.hp)
    print(f"[sim] DOOM /agent simulator on ws://{args.host}:{args.port}/agent "
          f"(realtime={args.realtime}, catchup={args.catchup} tics)", flush=True)
    stop = threading.Event()
    threading.Thread(target=world.ticker, args=(stop,), daemon=True).start()
    with serve(make_handler(world, args.persist), args.host, args.port) as server:
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    stop.set()


if __name__ == "__main__":
    main()
