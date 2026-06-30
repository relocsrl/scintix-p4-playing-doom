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
