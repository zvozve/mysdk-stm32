#ifndef __MQTT_CLIENT_H__
#define __MQTT_CLIENT_H__

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef MQTT_LOG_ENABLE
    #define MQTT_LOG_ENABLE     1
#endif
#include "SEGGER_RTT_Log.h"
#define MQTT_LOG(fmt, ...)     RTT_LOG_TAG(MQTT_LOG_ENABLE,    "MQTT",    fmt, ##__VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

// ===========================
// MQTT 配置
// ===========================
#define MQTT_MAX_PACKET_SIZE    1024
#define MQTT_MAX_TOPIC_LEN      64
#define MQTT_MAX_PAYLOAD_LEN    256
#define MQTT_MAX_CLIENT_ID_LEN  32
#define MQTT_MAX_USERNAME_LEN   32
#define MQTT_MAX_PASSWORD_LEN   32

// ===========================
// MQTT 消息类型
// ===========================
typedef enum {
    MQTT_PKT_CONNECT      = 0x10,
    MQTT_PKT_CONNACK      = 0x20,
    MQTT_PKT_PUBLISH      = 0x30,
    MQTT_PKT_PUBACK       = 0x40,
    MQTT_PKT_PUBREC       = 0x50,
    MQTT_PKT_PUBREL       = 0x60,
    MQTT_PKT_PUBCOMP      = 0x70,
    MQTT_PKT_SUBSCRIBE    = 0x80,
    MQTT_PKT_SUBACK       = 0x90,
    MQTT_PKT_UNSUBSCRIBE  = 0xA0,
    MQTT_PKT_UNSUBACK     = 0xB0,
    MQTT_PKT_PINGREQ      = 0xC0,
    MQTT_PKT_PINGRESP     = 0xD0,
    MQTT_PKT_DISCONNECT   = 0xE0,
} mqtt_packet_type_t;

// ===========================
// MQTT QoS
// ===========================
typedef enum {
    MQTT_QOS_0 = 0,
    MQTT_QOS_1 = 1,
    MQTT_QOS_2 = 2,
} mqtt_qos_t;

// ===========================
// MQTT 连接状态
// ===========================
typedef enum {
    MQTT_STATE_IDLE = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_SUBSCRIBING,
    MQTT_STATE_SUBSCRIBED,
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_ERROR,
} mqtt_state_t;

// ===========================
// MQTT 连接信息
// ===========================
typedef struct {
    char client_id[MQTT_MAX_CLIENT_ID_LEN];
    char username[MQTT_MAX_USERNAME_LEN];
    char password[MQTT_MAX_PASSWORD_LEN];
    uint16_t keepalive;
    bool clean_session;
} mqtt_connect_info_t;

// ===========================
// MQTT 接收消息
// ===========================
typedef struct {
    char topic[MQTT_MAX_TOPIC_LEN];
    uint8_t payload[MQTT_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    mqtt_qos_t qos;
    bool retained;
} mqtt_recv_msg_t;

// ===========================
// MQTT 回调函数
// ===========================
typedef struct {
    // 连接状态变化回调
    void (*on_connect)(void);
    void (*on_disconnect)(void);
    
    // 收到消息回调
    void (*on_message)(const char *topic, const uint8_t *payload, uint16_t len);
    
    // 订阅/取消订阅回调
    void (*on_subscribe)(const char *topic, uint8_t result);
    void (*on_unsubscribe)(const char *topic);
    
    // 发送完成回调
    void (*on_publish)(uint16_t packet_id);
} mqtt_callbacks_t;

// ===========================
// MQTT 发送接口（由用户实现）
// ===========================
typedef struct {
    // 发送数据到网络
    int (*send)(const uint8_t *data, uint16_t len);
    
    // 获取时间戳(ms)
    uint32_t (*get_tick)(void);
    
    // 延迟函数(ms)
    void (*delay)(uint32_t ms);
} mqtt_transport_t;

// ===========================
// MQTT 客户端结构
// ===========================
typedef struct {
    mqtt_state_t state;
    mqtt_connect_info_t connect_info;
    mqtt_callbacks_t callbacks;
    mqtt_transport_t transport;
    
    // 运行时状态
    uint16_t next_packet_id;
    uint32_t last_ping_tick;
    uint32_t last_rx_tick;
    bool is_connected;
    
    // 接收缓冲区
    uint8_t rx_buffer[MQTT_MAX_PACKET_SIZE];
    uint16_t rx_len;
    uint16_t rx_expected;
    
    // 发送缓冲区
    uint8_t tx_buffer[MQTT_MAX_PACKET_SIZE];
} mqtt_client_t;

// ===========================
// API
// ===========================
void mqtt_client_init(mqtt_client_t *client, mqtt_transport_t *transport);
void mqtt_client_set_callbacks(mqtt_client_t *client, mqtt_callbacks_t *callbacks);
void mqtt_client_set_connect_info(mqtt_client_t *client, const char *client_id, 
                                   const char *username, const char *password);

// 连接管理
int mqtt_client_connect(mqtt_client_t *client, uint16_t keepalive, bool clean_session);
int mqtt_client_disconnect(mqtt_client_t *client);

// 发布消息
int mqtt_client_publish(mqtt_client_t *client, const char *topic, 
                         const uint8_t *payload, uint16_t len, 
                         mqtt_qos_t qos, bool retained);

// 订阅/取消订阅
int mqtt_client_subscribe(mqtt_client_t *client, const char *topic, mqtt_qos_t qos);
int mqtt_client_unsubscribe(mqtt_client_t *client, const char *topic);

// 心跳保持
int mqtt_client_ping(mqtt_client_t *client);

// 数据处理（收到网络数据时调用）
int mqtt_client_process_rx(mqtt_client_t *client, const uint8_t *data, uint16_t len);

// 状态查询
mqtt_state_t mqtt_client_get_state(mqtt_client_t *client);
bool mqtt_client_is_connected(mqtt_client_t *client);

// 周期性处理（在任务循环中调用）
void mqtt_client_loop(mqtt_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* __MQTT_CLIENT_H__ */