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
        cJSON_AddNumberToObject(r, "weapon", o->weapon);
        cJSON_AddNumberToObject(r, "ammo", o->ammo);
        cJSON_AddNumberToObject(r, "episode", o->episode);
        cJSON_AddNumberToObject(r, "map", o->map);

        cJSON *front = cJSON_AddObjectToObject(r, "front");
        if (front) {
            cJSON_AddNumberToObject(front, "type", o->front_type);
            cJSON_AddNumberToObject(front, "dist", o->front_dist);
        }
        cJSON *enemies = cJSON_AddArrayToObject(r, "enemies");
        for (int i = 0; enemies && i < o->num_enemies; i++) {
            cJSON *e = cJSON_CreateObject();
            if (!e) {
                break;
            }
            cJSON_AddNumberToObject(e, "type", o->enemies[i].type);
            cJSON_AddNumberToObject(e, "dist", o->enemies[i].dist);
            cJSON_AddNumberToObject(e, "bearing", o->enemies[i].bearing_deg);
            cJSON_AddBoolToObject(e, "sight", o->enemies[i].in_sight);
            cJSON_AddItemToArray(enemies, e);
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

    doom_agent_action_t action;
    parse_action((const char *)buf, frame.len, &action);
    free(buf);

    doom_agent_obs_t obs;
    doom_agent_step(&action, &obs);

    char *json = build_obs_json(&obs);
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
