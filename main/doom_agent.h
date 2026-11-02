#pragma once

/* Agent API: lets an external controller (e.g. an LLM via an MCP bridge) drive
 * the game in lockstep over a WebSocket — it sends an action, the game advances
 * a few tics, and it gets back a structured observation (position, facing,
 * health/weapon, what's in front, nearby enemies). See main/doom_agent.c for the
 * WebSocket/JSON layer and main/doom_port.c for the game-side step. */

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

#define DOOM_AGENT_MAX_VISIBLE 16

typedef struct {
    int  move;    /* -1 back, 0 none, +1 forward */
    int  turn;    /* -1 left, 0 none, +1 right   */
    int  strafe;  /* -1 left, 0 none, +1 right   */
    bool fire;
    bool use;
    int  weapon;  /* 0 = keep current, 1..7 = select */
    int  tics;    /* game tics to advance this step (clamped 0..60) */
    /* Free-running ("realtime") mode: keep these keys HELD after replying and do
     * not pause the game between requests. The observation is returned at once
     * (after one tick, so the keys register) while the game keeps running; the
     * next request swaps the held keys. `tics` becomes the auto-release timeout
     * (clamped 1..175): if no new request arrives within that many game tics the
     * keys are released, so a vanished controller never leaves the marine
     * running into a wall. A request without `hold` returns to lockstep. */
    bool hold;
} doom_agent_action_t;

/* A thing the player can actually SEE this frame: inside the rendered field of
 * view and not occluded by geometry. Deliberately no targeting hints — just what
 * it is and where it is, like looking at the screen. */
typedef struct {
    char name[16];    /* "zombieman", "imp", "barrel", ... */
    int  dist;        /* map units */
    int  bearing_deg; /* -45..45 from facing: 0 = centre, negative = left, positive = right */
} doom_agent_thing_t;

/* A pickup lying in view (inside the field of view, in line of sight, within
 * AGENT_ITEM_RANGE): what the player sees on the floor. Nearest first. */
#define DOOM_AGENT_MAX_ITEMS 8
typedef struct {
    char        name[16];    /* "medikit", "clip", "blue_keycard", "shotgun", ... */
    const char *kind;        /* "health", "armor", "ammo", "weapon", "key", "powerup" */
    int         dist;        /* map units */
    int         bearing_deg; /* -45..45 from facing, + = right */
} doom_agent_item_t;

/* A projectile in flight in view (imp fireball, rocket, ...), not the player's
 * own. `incoming` = on its current course it reaches the player's body (its
 * closest approach passes within the player's and its own radius, at a height
 * the player occupies) — what a player judges from a fireball growing on screen.
 * Nearest first. */
#define DOOM_AGENT_MAX_MISSILES 6
typedef struct {
    char        name[16];    /* "imp_fireball", "rocket", ... */
    int         dist;        /* map units */
    int         bearing_deg; /* -45..45 from facing, + = right */
    int         dz;          /* height of the projectile above the player's feet (map units) */
    bool        incoming;
    int         eta_tics;    /* incoming: game tics until it reaches the player */
    const char *dodge;       /* incoming: "left" / "right" = the sidestep that takes the player off its line */
} doom_agent_missile_t;

/* Distance to the nearest wall/closed obstruction along a ray, like judging
 * depth in the rendered 3D view. Rays fan across the field of view only. */
#define DOOM_AGENT_NUM_RAYS 51
typedef struct {
    int bearing_deg;  /* -45..45 from facing: 0 = ahead, negative = left, positive = right */
    int dist;         /* map units to the first obstruction; clamped to the scan range if clear */
    /* What the floor does along the ray, up to the obstruction (all walkable: a
     * step up of more than 24 units is itself the obstruction). */
    int rise;         /* highest floor reached above the player's floor: > 0 = stairs/ramp up */
    int hurt;         /* on a safe floor: distance to where a damaging floor (nukage, slime,
                         lava) begins; -1 = none before the obstruction */
    int safe;         /* on a damaging floor: distance to where a safe floor begins; -1 = none */
} doom_agent_ray_t;

typedef struct {
    bool valid;
    int  x, y, z;     /* map units */
    int  angle_deg;   /* 0..359 compass heading: 0 = north, 90 = east, clockwise */
    int  health, armor, ammo;
    char weapon[16];  /* current weapon name */
    int  episode, map;
    int  num_visible;
    doom_agent_thing_t visible[DOOM_AGENT_MAX_VISIBLE];
    int  num_items;
    doom_agent_item_t items[DOOM_AGENT_MAX_ITEMS];
    int  num_missiles;
    doom_agent_missile_t missiles[DOOM_AGENT_MAX_MISSILES];
    /* The status bar: weapons owned (bit per weapontype_t, wp_fist = bit 0), keys
     * held (bit per card_t: blue/yellow/red keycard, blue/yellow/red skull) and the
     * ammo of every type (bullets, shells, cells, rockets — Doom's ammotype_t
     * order) with its current maximum (doubled by a backpack). */
    unsigned weapons_owned;
    unsigned keys_held;
    int  ammo_all[4];
    int  ammo_max[4];
    int  num_rays;
    doom_agent_ray_t walls[DOOM_AGENT_NUM_RAYS];
    /* An openable-but-currently-closed obstruction in view (a door / closed
     * passage), distinct from a solid wall; bearing and distance point at the
     * middle of the door line (it can be outside the view when only its edge is
     * seen). Tells the agent to press use again when a door has re-shut.
     * door_ahead_valid = false when none is in view. */
    bool door_ahead_valid;
    int  door_ahead_bearing; /* relative to facing, + = right; can exceed +/-45 */
    int  door_ahead_dist;    /* map units to the middle of the door */
    bool door_ahead_lift;    /* it is a raised lift (USE lowers it, step on, it rises), not a door */
    /* What is under the crosshair within USE reach (the 64-unit trace along the
     * facing that P_UseLines does): NULL = nothing (or a wall first: USE goes "oof"),
     * else "door", "lift" or "switch" - only what the player would see on screen
     * (an exit switch is a "switch"); "open_door" = a door already open (USE would
     * close it). use_locked = a key door (colour-marked) the
     * player has no key for. */
    const char *use_target;
    bool use_locked;
    /* A wall with EXIT written on it (exit sign / exit door / exit switch) that a
     * ray of the view reaches within 1024 units: bearing / dist to its middle. */
    bool exit_sign_valid;
    int  exit_sign_bearing;
    int  exit_sign_dist;
    /* A switch panel not flipped yet on a wall a ray of the view reaches within
     * 1024 units (the most central one): bearing / dist to the middle of its line. */
    bool switch_ahead_valid;
    int  switch_ahead_bearing;
    int  switch_ahead_dist;
    /* Health lost since the previous observation, and where the hit came from as
     * the player perceives it (the red flash, the status-bar face turning toward the
     * attacker, the stereo sound): "ahead", "left", "right", "behind", or NULL when
     * there is no attacker (a damaging floor, a blast whose source is gone). */
    int  hurt_amount;
    const char *hurt_from;
    /* The floor under the player: damage per hit (every 32 tics) of a nukage /
     * slime / lava floor, 0 = safe; radiation suit tics left (protects from it). */
    int  floor_damage;
    int  radsuit_tics;
    /* Set after a movement action when the player barely moved (ran into
     * something). false for pure observe/turn steps. */
    bool blocked;
} doom_agent_obs_t;

/* ASCII automap: a bounded grid mirroring the player's in-game automap — only
 * the wall lines the player has already discovered, plus the player marker.
 * North is up. '#' = revealed wall (or a closed door / a step too high to climb),
 * ':' = a step edge that can be walked over (stairs), '~' = the edge of a
 * damaging floor, 'E' = a wall with EXIT on it that the player has seen in the
 * view, '@' = player, ' ' = undiscovered/empty (flat open boundaries between
 * sectors are not drawn).
 * No monsters or items (the real automap doesn't show them without cheats). */
#define DOOM_AGENT_MAP_COLS 64
#define DOOM_AGENT_MAP_ROWS 32

typedef struct {
    bool valid;
    int  rows, cols;
    int  units_per_cell;       /* map units per character cell */
    int  origin_x, origin_y;   /* map coords of the top-left cell [0][0] */
    int  player_col, player_row;
    int  angle_deg;            /* 0..359 compass heading: 0 = north (up), clockwise */
    char grid[DOOM_AGENT_MAP_ROWS][DOOM_AGENT_MAP_COLS + 1]; /* NUL-terminated rows */
} doom_agent_map_t;

/* Register the "/agent" WebSocket endpoint on an existing HTTP server. */
esp_err_t doom_agent_register(httpd_handle_t server);

/* Game-side hooks, implemented in doom_port.c (run on the Doom task):
 * - doom_agent_step: apply the action, advance the requested tics, capture the
 *   observation. Called from the WebSocket task; blocks until the step is done.
 * - doom_agent_set_active: enter/leave agent control. In lockstep the game
 *   pauses between steps; in realtime (action.hold) it keeps running with the
 *   last keys held; with no agent it free-runs on the local controls. */
bool doom_agent_step(const doom_agent_action_t *action, doom_agent_obs_t *obs);
bool doom_agent_get_map(doom_agent_map_t *map);
void doom_agent_set_active(bool active);
