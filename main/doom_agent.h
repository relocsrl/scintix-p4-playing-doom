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
} doom_agent_action_t;

/* A thing the player can actually SEE this frame: inside the rendered field of
 * view and not occluded by geometry. Deliberately no targeting hints — just what
 * it is and where it is, like looking at the screen. */
typedef struct {
    char name[16];    /* "zombieman", "imp", "barrel", ... */
    int  dist;        /* map units */
    int  bearing_deg; /* -45..45, relative to player facing (0 = centre of view) */
} doom_agent_thing_t;

/* Distance to the nearest wall/closed obstruction along a ray, like judging
 * depth in the rendered 3D view. Rays fan across the field of view only. */
#define DOOM_AGENT_NUM_RAYS 7
typedef struct {
    int bearing_deg;  /* -45..45 relative to facing (0 = straight ahead) */
    int dist;         /* map units to the first obstruction; clamped to the scan range if clear */
} doom_agent_ray_t;

typedef struct {
    bool valid;
    int  x, y, z;     /* map units */
    int  angle_deg;   /* 0..359, facing */
    int  health, armor, ammo;
    char weapon[16];  /* current weapon name */
    int  episode, map;
    int  num_visible;
    doom_agent_thing_t visible[DOOM_AGENT_MAX_VISIBLE];
    int  num_rays;
    doom_agent_ray_t walls[DOOM_AGENT_NUM_RAYS];
} doom_agent_obs_t;

/* ASCII automap: a bounded grid mirroring the player's in-game automap — only
 * the wall lines the player has already discovered, plus the player marker.
 * North is up. '#' = revealed wall, '@' = player, ' ' = undiscovered/empty.
 * No monsters or items (the real automap doesn't show them without cheats). */
#define DOOM_AGENT_MAP_COLS 64
#define DOOM_AGENT_MAP_ROWS 32

typedef struct {
    bool valid;
    int  rows, cols;
    int  units_per_cell;       /* map units per character cell */
    int  origin_x, origin_y;   /* map coords of the top-left cell [0][0] */
    int  player_col, player_row;
    int  angle_deg;
    char grid[DOOM_AGENT_MAP_ROWS][DOOM_AGENT_MAP_COLS + 1]; /* NUL-terminated rows */
} doom_agent_map_t;

/* Register the "/agent" WebSocket endpoint on an existing HTTP server. */
esp_err_t doom_agent_register(httpd_handle_t server);

/* Game-side hooks, implemented in doom_port.c (run on the Doom task):
 * - doom_agent_step: apply the action, advance the requested tics, capture the
 *   observation. Called from the WebSocket task; blocks until the step is done.
 * - doom_agent_set_active: enter/leave lockstep (the game pauses between steps
 *   while active; it free-runs otherwise). */
bool doom_agent_step(const doom_agent_action_t *action, doom_agent_obs_t *obs);
bool doom_agent_get_map(doom_agent_map_t *map);
void doom_agent_set_active(bool active);
