#include "mqtt_json_payload.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

char* mqtt_status_to_json(const device_status_t *status) {
    if (!status) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "temperature", status->temperature);
    cJSON_AddNumberToObject(root, "humidity", status->humidity);
    cJSON_AddNumberToObject(root, "led", status->led_state);
    cJSON_AddNumberToObject(root, "relay", status->relay_state);
    cJSON_AddNumberToObject(root, "uptime", status->uptime);
    cJSON_AddStringToObject(root, "status", "online");

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}
