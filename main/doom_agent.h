#pragma once

/* Agent API: lets an external controller (e.g. an LLM via an MCP bridge) drive
 * the game in lockstep over a WebSocket — it sends an action, the game advances
 * a few tics, and it gets back a structured observation (position, facing,
 * health/weapon, what's in front, nearby enemies). See main/doom_agent.c for the
 * WebSocket/JSON layer and main/doom_port.c for the game-side step. */

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

#define DOOM_AGENT_MAX_ENEMIES 8

typedef struct {
    int  move;    /* -1 back, 0 none, +1 forward */
    int  turn;    /* -1 left, 0 none, +1 right   */
    int  strafe;  /* -1 left, 0 none, +1 right   */
    bool fire;
    bool use;
    int  weapon;  /* 0 = keep current, 1..7 = select */
    int  tics;    /* game tics to advance this step (clamped 0..60) */
} doom_agent_action_t;

typedef struct {
    int  type;        /* Doom mobjtype_t */
    int  dist;        /* map units */
    int  bearing_deg; /* -180..180, relative to player facing */
    bool in_sight;
} doom_agent_enemy_t;

typedef struct {
    bool valid;
    int  x, y, z;     /* map units */
    int  angle_deg;   /* 0..359 */
    int  health, armor;
    int  weapon, ammo;
    int  episode, map;
    int  front_type;  /* mobjtype in front of the player, -1 = nothing */
    int  front_dist;
    int  num_enemies;
    doom_agent_enemy_t enemies[DOOM_AGENT_MAX_ENEMIES];
} doom_agent_obs_t;

/* Register the "/agent" WebSocket endpoint on an existing HTTP server. */
esp_err_t doom_agent_register(httpd_handle_t server);

/* Game-side hooks, implemented in doom_port.c (run on the Doom task):
 * - doom_agent_step: apply the action, advance the requested tics, capture the
 *   observation. Called from the WebSocket task; blocks until the step is done.
 * - doom_agent_set_active: enter/leave lockstep (the game pauses between steps
 *   while active; it free-runs otherwise). */
bool doom_agent_step(const doom_agent_action_t *action, doom_agent_obs_t *obs);
void doom_agent_set_active(bool active);
