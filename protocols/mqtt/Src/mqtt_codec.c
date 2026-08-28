#include "mqtt_codec.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

char* mqtt_status_to_json(const device_status_t *status) {  // ← 改成 device_status_t
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

/* 注意：控制命令的解析(smart_home/cmd/xxx)已移至 task_mqtt.c（任务职责），
 * 此处 codec 只保留与业务无关的序列化(device_status_t -> JSON)。 */