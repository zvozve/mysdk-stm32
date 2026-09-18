#ifndef __MQTT_JSON_PAYLOAD_H__
#define __MQTT_JSON_PAYLOAD_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 注意：本文件不是 MQTT 协议编解码器（codec），而是一份“业务负载序列化”示例，
 * 把 device_status_t 序列化成 JSON 字符串，依赖 middleware.cJSON。
 * 它属于应用/任务层职责，放这里仅为演示；接入工程应在自己的 app/task 代码里
 * 自带同类负载序列化，并可在 sync 后用自身实现覆盖（或删除本文件）。
 * 真正的线缆层编解码由 Paho MQTTPacket（本目录 MQTTPacket.* 等）负责。
 */

typedef struct {
    float temperature;
    float humidity;
    uint8_t led_state;
    uint8_t relay_state;
    uint32_t uptime;
} device_status_t;

char* mqtt_status_to_json(const device_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* __MQTT_JSON_PAYLOAD_H__ */
