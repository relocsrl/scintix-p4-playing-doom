#include "doom_agent.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "doom_agent";

/* Accept either a string ("forward"/"back"/"left"/"right") or a signed number. */
static int parse_axis(const cJSON *item, const char *neg, const char *pos)
{
    if (cJSON_IsString(item) && item->valuestring) {
        if (neg && strcmp(item->valuestring, neg) == 0) {
            return -1;
        }
        if (pos && strcmp(item->valuestring, pos) == 0) {
            return 1;
        }
        return 0;
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint > 0 ? 1 : (item->valueint < 0 ? -1 : 0);
    }
    return 0;
}

static void parse_action(const char *txt, size_t len, doom_agent_action_t *a)
{
    memset(a, 0, sizeof(*a));
    a->tics = 7; /* ~0.2 s of game time per step by default */

    cJSON *root = cJSON_ParseWithLength(txt, len);
    if (root == NULL) {
        return;
    }
    a->move = parse_axis(cJSON_GetObjectItemCaseSensitive(root, "move"), "back", "forward");
    a->turn = parse_axis(cJSON_GetObjectItemCaseSensitive(root, "turn"), "left", "right");
    a->strafe = parse_axis(cJSON_GetObjectItemCaseSensitive(root, "strafe"), "left", "right");
    a->fire = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "fire"));
    a->use = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "use"));

    const cJSON *weapon = cJSON_GetObjectItemCaseSensitive(root, "weapon");
    if (cJSON_IsNumber(weapon)) {
        a->weapon = weapon->valueint;
    }
    const cJSON *tics = cJSON_GetObjectItemCaseSensitive(root, "tics");
    if (cJSON_IsNumber(tics)) {
        a->tics = tics->valueint;
    }
    /* {"hold": true} = realtime mode: keys stay held, game keeps running, reply
     * at once; `tics` is then the auto-release timeout (default 1 s). */
    a->hold = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "hold"));
    if (a->hold && !cJSON_IsNumber(tics)) {
        a->tics = 35;
    }
    cJSON_Delete(root);
}

static char *build_obs_json(const doom_agent_obs_t *o)
{
    cJSON *r = cJSON_CreateObject();
    if (r == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(r, "valid", o->valid);
    if (o->valid) {
        cJSON_AddNumberToObject(r, "x", o->x);
        cJSON_AddNumberToObject(r, "y", o->y);
        cJSON_AddNumberToObject(r, "z", o->z);
        cJSON_AddNumberToObject(r, "angle", o->angle_deg);
        cJSON_AddNumberToObject(r, "health", o->health);
        cJSON_AddNumberToObject(r, "armor", o->armor);
        cJSON_AddStringToObject(r, "weapon", o->weapon);
        cJSON_AddNumberToObject(r, "ammo", o->ammo);
        cJSON_AddNumberToObject(r, "episode", o->episode);
        cJSON_AddNumberToObject(r, "map", o->map);

        /* The status bar: weapons owned, keys held, ammo of every type / its maximum. */
        static const char *const weapon_names[] = {
            "fist", "pistol", "shotgun", "chaingun", "rocket_launcher",
            "plasma_rifle", "bfg9000", "chainsaw", "super_shotgun",
        };
        static const char *const key_names[] = {
            "blue_keycard", "yellow_keycard", "red_keycard",
            "blue_skullkey", "yellow_skullkey", "red_skullkey",
        };
        static const char *const ammo_names[] = {"bullets", "shells", "cells", "rockets"};
        cJSON *weapons = cJSON_AddArrayToObject(r, "weapons");
        for (int i = 0; weapons && i < (int)(sizeof(weapon_names) / sizeof(weapon_names[0])); i++) {
            if (o->weapons_owned & (1u << i)) {
                cJSON_AddItemToArray(weapons, cJSON_CreateString(weapon_names[i]));
            }
        }
        cJSON *keys = cJSON_AddArrayToObject(r, "keys");
        for (int i = 0; keys && i < (int)(sizeof(key_names) / sizeof(key_names[0])); i++) {
            if (o->keys_held & (1u << i)) {
                cJSON_AddItemToArray(keys, cJSON_CreateString(key_names[i]));
            }
        }
        cJSON *ammo_all = cJSON_AddObjectToObject(r, "ammo_all");
        cJSON *ammo_max = cJSON_AddObjectToObject(r, "ammo_max");
        for (int i = 0; ammo_all && ammo_max && i < 4; i++) {
            cJSON_AddNumberToObject(ammo_all, ammo_names[i], o->ammo_all[i]);
            cJSON_AddNumberToObject(ammo_max, ammo_names[i], o->ammo_max[i]);
        }

        /* Only what the player can see on screen (in the field of view, in line
         * of sight), left-to-right. No targeting hints. */
        cJSON *visible = cJSON_AddArrayToObject(r, "visible");
        for (int i = 0; visible && i < o->num_visible; i++) {
            cJSON *t = cJSON_CreateObject();
            if (!t) {
                break;
            }
            cJSON_AddStringToObject(t, "name", o->visible[i].name);
            cJSON_AddNumberToObject(t, "dist", o->visible[i].dist);
            cJSON_AddNumberToObject(t, "bearing", o->visible[i].bearing_deg);
            cJSON_AddItemToArray(visible, t);
        }

        /* Pickups lying in view (nearest first) and projectiles in flight in view. */
        cJSON *items = cJSON_AddArrayToObject(r, "items");
        for (int i = 0; items && i < o->num_items; i++) {
            cJSON *t = cJSON_CreateObject();
            if (!t) {
                break;
            }
            cJSON_AddStringToObject(t, "name", o->items[i].name);
            cJSON_AddStringToObject(t, "kind", o->items[i].kind);
            cJSON_AddNumberToObject(t, "dist", o->items[i].dist);
            cJSON_AddNumberToObject(t, "bearing", o->items[i].bearing_deg);
            cJSON_AddItemToArray(items, t);
        }
        cJSON *missiles = cJSON_AddArrayToObject(r, "missiles");
        for (int i = 0; missiles && i < o->num_missiles; i++) {
            const doom_agent_missile_t *m = &o->missiles[i];
            cJSON *t = cJSON_CreateObject();
            if (!t) {
                break;
            }
            cJSON_AddStringToObject(t, "name", m->name);
            cJSON_AddNumberToObject(t, "dist", m->dist);
            cJSON_AddNumberToObject(t, "bearing", m->bearing_deg);
            cJSON_AddNumberToObject(t, "dz", m->dz);
            cJSON_AddBoolToObject(t, "incoming", m->incoming);
            if (m->incoming) {
                cJSON_AddNumberToObject(t, "eta", m->eta_tics);
                cJSON_AddStringToObject(t, "dodge", m->dodge ? m->dodge : "right");
            }
            cJSON_AddItemToArray(missiles, t);
        }

        /* Distance to the nearest wall along rays across the field of view. */
        cJSON *walls = cJSON_AddArrayToObject(r, "walls");
        for (int i = 0; walls && i < o->num_rays; i++) {
            cJSON *w = cJSON_CreateObject();
            if (!w) {
                break;
            }
            cJSON_AddNumberToObject(w, "bearing", o->walls[i].bearing_deg);
            cJSON_AddNumberToObject(w, "dist", o->walls[i].dist);
            /* Floor facts, only when there is something to say (keeps the reply small). */
            if (o->walls[i].rise > 0) {
                cJSON_AddNumberToObject(w, "rise", o->walls[i].rise);
            }
            if (o->walls[i].hurt >= 0) {
                cJSON_AddNumberToObject(w, "hurt", o->walls[i].hurt);
            }
            if (o->walls[i].safe >= 0) {
                cJSON_AddNumberToObject(w, "safe", o->walls[i].safe);
            }
            cJSON_AddItemToArray(walls, w);
        }
        /* Damaging floor under the player (absent = safe) and the radiation suit. */
        if (o->floor_damage > 0) {
            cJSON_AddNumberToObject(r, "floor_damage", o->floor_damage);
        }
        if (o->radsuit_tics > 0) {
            cJSON_AddNumberToObject(r, "radsuit_tics", o->radsuit_tics);
        }

        /* Nearest closed door / passage in view (absent when none). Lets the agent
         * tell a re-shut door (press use again) from a solid wall (go around). */
        if (o->door_ahead_valid) {
            cJSON *d = cJSON_AddObjectToObject(r, "door_ahead");
            if (d) {
                cJSON_AddNumberToObject(d, "bearing", o->door_ahead_bearing);
                cJSON_AddNumberToObject(d, "dist", o->door_ahead_dist);
                if (o->door_ahead_lift) {
                    cJSON_AddStringToObject(d, "kind", "lift");
                }
            }
        }
        /* What is under the crosshair within USE reach; always present (null =
         * nothing) so a client can tell this firmware from one without the probe. */
        if (o->use_target) {
            cJSON_AddStringToObject(r, "use_target", o->use_target);
        } else {
            cJSON_AddNullToObject(r, "use_target");
        }
        if (o->use_locked) {
            cJSON_AddBoolToObject(r, "use_locked", true);
        }
        /* An EXIT sign readable in the view (absent when none). */
        if (o->exit_sign_valid) {
            cJSON *e = cJSON_AddObjectToObject(r, "exit_sign");
            if (e) {
                cJSON_AddNumberToObject(e, "bearing", o->exit_sign_bearing);
                cJSON_AddNumberToObject(e, "dist", o->exit_sign_dist);
            }
        }
        /* Hit since the last observation (absent when not): amount and side. */
        if (o->hurt_amount > 0 || o->hurt_from) {
            cJSON *h = cJSON_AddObjectToObject(r, "hurt");
            if (h) {
                cJSON_AddNumberToObject(h, "amount", o->hurt_amount);
                if (o->hurt_from) {
                    cJSON_AddStringToObject(h, "from", o->hurt_from);
                } else {
                    cJSON_AddNullToObject(h, "from");
                }
            }
        }
        /* A switch not flipped yet on a wall in view (absent when none). */
        if (o->switch_ahead_valid) {
            cJSON *s = cJSON_AddObjectToObject(r, "switch_ahead");
            if (s) {
                cJSON_AddNumberToObject(s, "bearing", o->switch_ahead_bearing);
                cJSON_AddNumberToObject(s, "dist", o->switch_ahead_dist);
            }
        }
        /* True when a requested move barely shifted the player (ran into something). */
        cJSON_AddBoolToObject(r, "blocked", o->blocked);
    }
    char *out = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return out;
}

static char *build_map_json(const doom_agent_map_t *m)
{
    cJSON *r = cJSON_CreateObject();
    if (r == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(r, "valid", m->valid);
    if (m->valid) {
        cJSON_AddNumberToObject(r, "cols", m->cols);
        cJSON_AddNumberToObject(r, "rows", m->rows);
        cJSON_AddNumberToObject(r, "units_per_cell", m->units_per_cell);
        cJSON_AddNumberToObject(r, "origin_x", m->origin_x);
        cJSON_AddNumberToObject(r, "origin_y", m->origin_y);
        cJSON *p = cJSON_AddObjectToObject(r, "player");
        if (p) {
            cJSON_AddNumberToObject(p, "col", m->player_col);
            cJSON_AddNumberToObject(p, "row", m->player_row);
            cJSON_AddNumberToObject(p, "angle", m->angle_deg);
        }
        cJSON *grid = cJSON_AddArrayToObject(r, "grid");
        for (int i = 0; grid && i < m->rows; i++) {
            cJSON_AddItemToArray(grid, cJSON_CreateString(m->grid[i]));
        }
    }
    char *out = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return out;
}

static esp_err_t agent_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake completed; a controller is now driving the game. */
        ESP_LOGI(TAG, "agent connected (lockstep)");
        doom_agent_set_active(true);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0); /* length probe */
    if (err != ESP_OK) {
        doom_agent_set_active(false);
        return err;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        doom_agent_set_active(false);
        return ESP_OK;
    }
    if (frame.len == 0 || frame.len > 1024) {
        return ESP_OK;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    /* A map request {"request":"map"} or {"map":true} returns the ASCII automap;
     * anything else is an action that advances the game and returns an observation. */
    bool want_map = false;
    cJSON *probe = cJSON_ParseWithLength((const char *)buf, frame.len);
    if (probe != NULL) {
        const cJSON *req_item = cJSON_GetObjectItemCaseSensitive(probe, "request");
        const cJSON *map_item = cJSON_GetObjectItemCaseSensitive(probe, "map");
        want_map = (cJSON_IsString(req_item) && req_item->valuestring &&
                    strcmp(req_item->valuestring, "map") == 0) || cJSON_IsTrue(map_item);
        cJSON_Delete(probe);
    }

    char *json = NULL;
    if (want_map) {
        doom_agent_map_t map;
        doom_agent_get_map(&map);
        json = build_map_json(&map);
    } else {
        doom_agent_action_t action;
        parse_action((const char *)buf, frame.len, &action);
        doom_agent_obs_t obs;
        doom_agent_step(&action, &obs);
        json = build_obs_json(&obs);
    }
    free(buf);

    if (json != NULL) {
        httpd_ws_frame_t resp = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = strlen(json),
        };
        httpd_ws_send_frame(req, &resp);
        free(json);
    }
    return ESP_OK;
}

esp_err_t doom_agent_register(httpd_handle_t server)
{
    static const httpd_uri_t uri = {
        .uri = "/agent",
        .method = HTTP_GET,
        .handler = agent_ws_handler,
        .is_websocket = true,
    };
    return httpd_register_uri_handler(server, &uri);
}
