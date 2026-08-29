#include "mqtt_client.h"
#include <string.h>
#include <stdlib.h>

#include "SEGGER_RTT_Log.h"

// ===========================
// 辅助宏
// ===========================
#define MQTT_HEADER_SIZE        1
#define MQTT_MAX_REMAIN_LEN     4

// 如果外部没有定义 mqtt_tick_callback，这里提供一个弱定义
// 实际由 task_mqtt.c 中的 transport.get_tick 提供

// ===========================
// 内部函数声明
// ===========================
static uint32_t mqtt_encode_remaining_length(uint8_t *buf, uint32_t len);
static uint32_t mqtt_decode_remaining_length(const uint8_t *buf, uint32_t *len);
static uint16_t mqtt_encode_string(uint8_t *buf, const char *str);
static uint16_t mqtt_calc_packet_id(mqtt_client_t *client);

// ===========================
// 初始化
// ===========================
void mqtt_client_init(mqtt_client_t *client, mqtt_transport_t *transport) {
    if (!client || !transport) return;
    
    memset(client, 0, sizeof(mqtt_client_t));
    memcpy(&client->transport, transport, sizeof(mqtt_transport_t));
    client->state = MQTT_STATE_IDLE;
    client->next_packet_id = 1;
    client->rx_len = 0;
    client->rx_expected = 0;
    
    MQTT_LOG("Client initialized");
}

void mqtt_client_set_callbacks(mqtt_client_t *client, mqtt_callbacks_t *callbacks) {
    if (!client || !callbacks) return;
    memcpy(&client->callbacks, callbacks, sizeof(mqtt_callbacks_t));
    MQTT_LOG("Callbacks registered");
}

void mqtt_client_set_connect_info(mqtt_client_t *client, const char *client_id,
                                   const char *username, const char *password) {
    if (!client) return;
    
    if (client_id) {
        strncpy(client->connect_info.client_id, client_id, MQTT_MAX_CLIENT_ID_LEN - 1);
        client->connect_info.client_id[MQTT_MAX_CLIENT_ID_LEN - 1] = '\0';
        MQTT_LOG("Client ID: %s", client->connect_info.client_id);
    }
    if (username) {
        strncpy(client->connect_info.username, username, MQTT_MAX_USERNAME_LEN - 1);
        client->connect_info.username[MQTT_MAX_USERNAME_LEN - 1] = '\0';
        MQTT_LOG("Username: %s", client->connect_info.username);
    }
    if (password) {
        strncpy(client->connect_info.password, password, MQTT_MAX_PASSWORD_LEN - 1);
        client->connect_info.password[MQTT_MAX_PASSWORD_LEN - 1] = '\0';
    }
}

// ===========================
// 编码/解码工具 (保持不变)
// ===========================
static uint32_t mqtt_encode_remaining_length(uint8_t *buf, uint32_t len) {
    uint32_t idx = 0;
    do {
        uint8_t byte = len & 0x7F;
        len >>= 7;
        if (len > 0) byte |= 0x80;
        buf[idx++] = byte;
    } while (len > 0);
    return idx;
}

static uint32_t mqtt_decode_remaining_length(const uint8_t *buf, uint32_t *len) {
    uint32_t multiplier = 1;
    uint32_t value = 0;
    uint32_t idx = 0;
    uint8_t byte;
    
    do {
        if (idx >= MQTT_MAX_REMAIN_LEN) return 0;
        byte = buf[idx++];
        value += (byte & 0x7F) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128) return 0;
    } while (byte & 0x80);
    
    *len = value;
    return idx;
}

static uint16_t mqtt_encode_string(uint8_t *buf, const char *str) {
    uint16_t len = strlen(str);
    buf[0] = (len >> 8) & 0xFF;
    buf[1] = len & 0xFF;
    memcpy(buf + 2, str, len);
    return len + 2;
}

static uint16_t mqtt_calc_packet_id(mqtt_client_t *client) {
    uint16_t id = client->next_packet_id++;
    if (client->next_packet_id == 0) client->next_packet_id = 1;
    return id;
}

// ===========================
// 构建 CONNECT 包 (保持不变)
// ===========================
static int mqtt_build_connect(mqtt_client_t *client, uint8_t *buf, uint16_t *len) {
    uint32_t idx = 0;
    uint8_t *pkt = buf;
    uint32_t remain_len = 0;
    uint32_t remain_idx;
    
    idx += 1;
    remain_idx = idx;
    idx += MQTT_MAX_REMAIN_LEN;
    
    pkt[idx++] = 0x00;
    pkt[idx++] = 0x04;
    pkt[idx++] = 'M';
    pkt[idx++] = 'Q';
    pkt[idx++] = 'T';
    pkt[idx++] = 'T';
    pkt[idx++] = 0x04;
    
    uint8_t flags = 0x02;
    if (strlen(client->connect_info.username) > 0) flags |= 0x80;
    if (strlen(client->connect_info.password) > 0) flags |= 0x40;
    pkt[idx++] = flags;
    
    uint16_t keepalive = client->connect_info.keepalive;
    pkt[idx++] = (keepalive >> 8) & 0xFF;
    pkt[idx++] = keepalive & 0xFF;
    
    idx += mqtt_encode_string(pkt + idx, client->connect_info.client_id);
    
    if (strlen(client->connect_info.username) > 0) {
        idx += mqtt_encode_string(pkt + idx, client->connect_info.username);
    }
    if (strlen(client->connect_info.password) > 0) {
        idx += mqtt_encode_string(pkt + idx, client->connect_info.password);
    }
    
    remain_len = idx - (remain_idx + MQTT_MAX_REMAIN_LEN);
    uint32_t enc_len = mqtt_encode_remaining_length(pkt + remain_idx, remain_len);
    
    if (enc_len != MQTT_MAX_REMAIN_LEN) {
        uint32_t data_len = idx - (remain_idx + MQTT_MAX_REMAIN_LEN);
        memmove(pkt + remain_idx + enc_len, pkt + remain_idx + MQTT_MAX_REMAIN_LEN, data_len);
        idx -= (MQTT_MAX_REMAIN_LEN - enc_len);
    }
    
    pkt[0] = MQTT_PKT_CONNECT;
    *len = idx;
    
    MQTT_LOG("Built CONNECT packet, len=%d", idx);
    return 0;
}

// ===========================
// 构建 PUBLISH 包 (保持不变)
// ===========================
static int mqtt_build_publish(mqtt_client_t *client, uint8_t *buf, uint16_t *len,
                               const char *topic, const uint8_t *payload, uint16_t payload_len,
                               mqtt_qos_t qos, bool retained) {
    uint32_t idx = 0;
    uint8_t *pkt = buf;
    uint32_t remain_len = 0;
    uint32_t remain_idx;
    uint16_t topic_len = strlen(topic);
    uint16_t packet_id = 0;
    
    idx += 1;
    remain_idx = idx;
    idx += MQTT_MAX_REMAIN_LEN;
    
    pkt[idx++] = (topic_len >> 8) & 0xFF;
    pkt[idx++] = topic_len & 0xFF;
    memcpy(pkt + idx, topic, topic_len);
    idx += topic_len;
    
    if (qos > MQTT_QOS_0) {
        packet_id = mqtt_calc_packet_id(client);
        pkt[idx++] = (packet_id >> 8) & 0xFF;
        pkt[idx++] = packet_id & 0xFF;
    }
    
    if (payload && payload_len > 0) {
        memcpy(pkt + idx, payload, payload_len);
        idx += payload_len;
    }
    
    remain_len = idx - (remain_idx + MQTT_MAX_REMAIN_LEN);
    uint32_t enc_len = mqtt_encode_remaining_length(pkt + remain_idx, remain_len);
    
    if (enc_len != MQTT_MAX_REMAIN_LEN) {
        uint32_t data_len = idx - (remain_idx + MQTT_MAX_REMAIN_LEN);
        memmove(pkt + remain_idx + enc_len, pkt + remain_idx + MQTT_MAX_REMAIN_LEN, data_len);
        idx -= (MQTT_MAX_REMAIN_LEN - enc_len);
    }
    
    uint8_t flags = 0;
    if (qos == MQTT_QOS_1) flags |= 0x02;
    else if (qos == MQTT_QOS_2) flags |= 0x04;
    if (retained) flags |= 0x01;
    pkt[0] = MQTT_PKT_PUBLISH | flags;
    
    *len = idx;
    MQTT_LOG("Built PUBLISH: topic=%s, len=%d, qos=%d", topic, idx, qos);
    return packet_id;
}

// ===========================
// 构建 SUBSCRIBE 包 (保持不变)
// ===========================
static int mqtt_build_subscribe(mqtt_client_t *client, uint8_t *buf, uint16_t *len,
                                 const char *topic, mqtt_qos_t qos) {
    uint32_t idx = 0;
    uint8_t *pkt = buf;
    uint32_t remain_len = 0;
    uint32_t remain_idx;
    uint16_t topic_len = strlen(topic);
    uint16_t packet_id;
    
    idx += 1;
    remain_idx = idx;
    idx += MQTT_MAX_REMAIN_LEN;
    
    packet_id = mqtt_calc_packet_id(client);
    pkt[idx++] = (packet_id >> 8) & 0xFF;
    pkt[idx++] = packet_id & 0xFF;
    
    pkt[idx++] = (topic_len >> 8) & 0xFF;
    pkt[idx++] = topic_len & 0xFF;
    memcpy(pkt + idx, topic, topic_len);
    idx += topic_len;
    pkt[idx++] = qos & 0xFF;
    
    remain_len = idx - (remain_idx + MQTT_MAX_REMAIN_LEN);
    uint32_t enc_len = mqtt_encode_remaining_length(pkt + remain_idx, remain_len);
    
    if (enc_len != MQTT_MAX_REMAIN_LEN) {
        uint32_t data_len = idx - (remain_idx + MQTT_MAX_REMAIN_LEN);
        memmove(pkt + remain_idx + enc_len, pkt + remain_idx + MQTT_MAX_REMAIN_LEN, data_len);
        idx -= (MQTT_MAX_REMAIN_LEN - enc_len);
    }
    
    pkt[0] = MQTT_PKT_SUBSCRIBE | 0x02;
    *len = idx;
    MQTT_LOG("Built SUBSCRIBE: topic=%s, len=%d", topic, idx);
    return packet_id;
}

// ===========================
// 构建 UNSUBSCRIBE 包 (保持不变)
// ===========================
static int mqtt_build_unsubscribe(mqtt_client_t *client, uint8_t *buf, uint16_t *len,
                                   const char *topic) {
    uint32_t idx = 0;
    uint8_t *pkt = buf;
    uint32_t remain_len = 0;
    uint32_t remain_idx;
    uint16_t topic_len = strlen(topic);
    uint16_t packet_id;
    
    idx += 1;
    remain_idx = idx;
    idx += MQTT_MAX_REMAIN_LEN;
    
    packet_id = mqtt_calc_packet_id(client);
    pkt[idx++] = (packet_id >> 8) & 0xFF;
    pkt[idx++] = packet_id & 0xFF;
    
    pkt[idx++] = (topic_len >> 8) & 0xFF;
    pkt[idx++] = topic_len & 0xFF;
    memcpy(pkt + idx, topic, topic_len);
    idx += topic_len;
    
    remain_len = idx - (remain_idx + MQTT_MAX_REMAIN_LEN);
    uint32_t enc_len = mqtt_encode_remaining_length(pkt + remain_idx, remain_len);
    
    if (enc_len != MQTT_MAX_REMAIN_LEN) {
        uint32_t data_len = idx - (remain_idx + MQTT_MAX_REMAIN_LEN);
        memmove(pkt + remain_idx + enc_len, pkt + remain_idx + MQTT_MAX_REMAIN_LEN, data_len);
        idx -= (MQTT_MAX_REMAIN_LEN - enc_len);
    }
    
    pkt[0] = MQTT_PKT_UNSUBSCRIBE | 0x02;
    *len = idx;
    MQTT_LOG("Built UNSUBSCRIBE: topic=%s, len=%d", topic, idx);
    return packet_id;
}

// ===========================
// 构建 PINGREQ 包
// ===========================
static int mqtt_build_pingreq(uint8_t *buf, uint16_t *len) {
    buf[0] = MQTT_PKT_PINGREQ;
    buf[1] = 0x00;
    *len = 2;
    MQTT_LOG("Built PINGREQ");
    return 0;
}

// ===========================
// 构建 DISCONNECT 包
// ===========================
static int mqtt_build_disconnect(uint8_t *buf, uint16_t *len) {
    buf[0] = MQTT_PKT_DISCONNECT;
    buf[1] = 0x00;
    *len = 2;
    MQTT_LOG("Built DISCONNECT");
    return 0;
}

// ===========================
// 解析 CONNACK
// ===========================
static int mqtt_parse_connack(const uint8_t *data, uint16_t len) {
    if (len < 4) return -1;
    if ((data[0] & 0xF0) != MQTT_PKT_CONNACK) return -1;
    
    uint8_t result = data[3];
    
    if (result == 0) {
        MQTT_LOG("CONNACK: Success");
        return 0;
    } else {
        const char *err[] = {"", "Unacceptable protocol version", "Identifier rejected", 
                             "Server unavailable", "Bad username/password", "Not authorized"};
        const char *msg = (result >= 1 && result <= 5) ? err[result] : "Unknown error";
        MQTT_LOG("CONNACK: Failed (code=%d): %s", result, msg);
        return -result;
    }
}

// ===========================
// 解析 PUBLISH (接收)
// ===========================
static int mqtt_parse_publish(mqtt_client_t *client, const uint8_t *data, uint16_t len) {
    if (len < 2) return -1;
    if ((data[0] & 0xF0) != MQTT_PKT_PUBLISH) return -1;
    
    uint32_t idx = 2;
    uint32_t remain_len = 0;
    uint32_t decoded = mqtt_decode_remaining_length(data + 1, &remain_len);
    if (decoded == 0) return -1;
    idx += decoded - 1;
    
    if (idx + 2 > len) return -1;
    uint16_t topic_len = (data[idx] << 8) | data[idx + 1];
    idx += 2;
    if (idx + topic_len > len) return -1;
    
    char topic[MQTT_MAX_TOPIC_LEN];
    if (topic_len >= MQTT_MAX_TOPIC_LEN) topic_len = MQTT_MAX_TOPIC_LEN - 1;
    memcpy(topic, data + idx, topic_len);
    topic[topic_len] = '\0';
    idx += topic_len;
    
    uint8_t qos = (data[0] >> 1) & 0x03;
    uint16_t packet_id = 0;
    if (qos > MQTT_QOS_0) {
        if (idx + 2 > len) return -1;
        packet_id = (data[idx] << 8) | data[idx + 1];
        idx += 2;
        (void)packet_id;
    }
    
    uint16_t payload_len = remain_len - (idx - 2 - (decoded - 1));
    if (idx + payload_len > len) return -1;
    
    MQTT_LOG("Received PUBLISH: topic=%s, qos=%d, len=%d", topic, qos, payload_len);
    
    if (client->callbacks.on_message) {
        client->callbacks.on_message(topic, data + idx, payload_len);
    }
    
    return 0;
}

// ===========================
// 解析 SUBACK
// ===========================
static int mqtt_parse_suback(mqtt_client_t *client, const uint8_t *data, uint16_t len) {
    if (len < 5) return -1;
    if ((data[0] & 0xF0) != MQTT_PKT_SUBACK) return -1;
    
    uint32_t idx = 2;
    uint32_t remain_len = 0;
    uint32_t decoded = mqtt_decode_remaining_length(data + 1, &remain_len);
    if (decoded == 0) return -1;
    idx += decoded - 1;
    
    if (idx + 2 > len) return -1;
    uint16_t packet_id = (data[idx] << 8) | data[idx + 1];
    idx += 2;
    (void)packet_id;
    
    if (idx >= len) return -1;
    uint8_t result = data[idx];
    
    MQTT_LOG("SUBACK: packet_id=%d, result=%d", packet_id, result);
    
    if (client->callbacks.on_subscribe) {
        client->callbacks.on_subscribe(NULL, result);
    }
    
    return (result <= 0x80) ? 0 : -1;
}

// ===========================
// 解析 PINGRESP
// ===========================
static int mqtt_parse_pingresp(const uint8_t *data, uint16_t len) {
    if (len < 2) return -1;
    if ((data[0] & 0xF0) != MQTT_PKT_PINGRESP) return -1;
    MQTT_LOG("Received PINGRESP");
    return 0;
}

// ===========================
// 公共 API (添加日志)
// ===========================
int mqtt_client_connect(mqtt_client_t *client, uint16_t keepalive, bool clean_session) {
    if (!client) {
        MQTT_LOG("Connect failed: client NULL");
        return -1;
    }
    
    MQTT_LOG("Connecting... keepalive=%d, clean_session=%d", keepalive, clean_session);
    
    client->connect_info.keepalive = keepalive;
    client->connect_info.clean_session = clean_session;
    client->state = MQTT_STATE_CONNECTING;
    client->last_ping_tick = client->transport.get_tick();
    client->last_rx_tick = client->last_ping_tick;
    
    uint16_t len;
    mqtt_build_connect(client, client->tx_buffer, &len);
    
    if (client->transport.send) {
        int ret = client->transport.send(client->tx_buffer, len);
        if (ret != 0) {
            MQTT_LOG("Connect send failed, ret=%d", ret);
            client->state = MQTT_STATE_ERROR;
            return -2;
        }
    }
    
    MQTT_LOG("Connect packet sent, waiting for CONNACK...");
    return 0;
}

int mqtt_client_disconnect(mqtt_client_t *client) {
    if (!client) return -1;
    MQTT_LOG("Disconnecting...");
    
    uint16_t len;
    mqtt_build_disconnect(client->tx_buffer, &len);
    
    if (client->transport.send) {
        client->transport.send(client->tx_buffer, len);
    }
    
    client->is_connected = false;
    client->state = MQTT_STATE_DISCONNECTED;
    
    if (client->callbacks.on_disconnect) {
        client->callbacks.on_disconnect();
    }
    
    return 0;
}

int mqtt_client_publish(mqtt_client_t *client, const char *topic,
                         const uint8_t *payload, uint16_t len,
                         mqtt_qos_t qos, bool retained) {
    if (!client || !topic) {
        MQTT_LOG("Publish failed: invalid params");
        return -1;
    }
    if (!client->is_connected) {
        MQTT_LOG("Publish failed: not connected");
        return -2;
    }
    
    MQTT_LOG("Publishing: topic=%s, len=%d, qos=%d", topic, len, qos);
    
    uint16_t pkt_len;
    int packet_id = mqtt_build_publish(client, client->tx_buffer, &pkt_len,
                                        topic, payload, len, qos, retained);
    
    if (client->transport.send) {
        int ret = client->transport.send(client->tx_buffer, pkt_len);
        if (ret != 0) {
            MQTT_LOG("Publish send failed, ret=%d", ret);
            return -3;
        }
    }
    
    if (client->callbacks.on_publish) {
        client->callbacks.on_publish(packet_id);
    }
    
    return packet_id;
}

int mqtt_client_subscribe(mqtt_client_t *client, const char *topic, mqtt_qos_t qos) {
    if (!client || !topic) {
        MQTT_LOG("Subscribe failed: invalid params");
        return -1;
    }
    if (!client->is_connected) {
        MQTT_LOG("Subscribe failed: not connected");
        return -2;
    }
    
    MQTT_LOG("Subscribing: topic=%s, qos=%d", topic, qos);
    
    client->state = MQTT_STATE_SUBSCRIBING;
    
    uint16_t pkt_len;
    int packet_id = mqtt_build_subscribe(client, client->tx_buffer, &pkt_len, topic, qos);
    
    if (client->transport.send) {
        int ret = client->transport.send(client->tx_buffer, pkt_len);
        if (ret != 0) {
            MQTT_LOG("Subscribe send failed, ret=%d", ret);
            return -3;
        }
    }
    
    return packet_id;
}

int mqtt_client_unsubscribe(mqtt_client_t *client, const char *topic) {
    if (!client || !topic) return -1;
    if (!client->is_connected) return -2;
    
    MQTT_LOG("Unsubscribing: topic=%s", topic);
    
    uint16_t pkt_len;
    int packet_id = mqtt_build_unsubscribe(client, client->tx_buffer, &pkt_len, topic);
    
    if (client->transport.send) {
        int ret = client->transport.send(client->tx_buffer, pkt_len);
        if (ret != 0) return -3;
    }
    
    if (client->callbacks.on_unsubscribe) {
        client->callbacks.on_unsubscribe(topic);
    }
    
    return packet_id;
}

int mqtt_client_ping(mqtt_client_t *client) {
    if (!client) return -1;
    if (!client->is_connected) return -2;
    
    uint16_t len;
    mqtt_build_pingreq(client->tx_buffer, &len);
    
    if (client->transport.send) {
        int ret = client->transport.send(client->tx_buffer, len);
        if (ret != 0) return -3;
    }
    
    client->last_ping_tick = client->transport.get_tick();
    MQTT_LOG("Ping sent");
    return 0;
}

int mqtt_client_process_rx(mqtt_client_t *client, const uint8_t *data, uint16_t len) {
    if (!client || !data || len == 0) return -1;
    
    client->last_rx_tick = client->transport.get_tick();
    
    if (client->rx_len + len > MQTT_MAX_PACKET_SIZE) {
        MQTT_LOG("RX buffer overflow");
        return -2;
    }
    
    memcpy(client->rx_buffer + client->rx_len, data, len);
    client->rx_len += len;
    
    MQTT_LOG("RX: %d bytes (total=%d)", len, client->rx_len);
    
    uint32_t idx = 0;
    while (idx < client->rx_len) {
        uint8_t packet_type = client->rx_buffer[idx] & 0xF0;
        uint32_t remain_len = 0;
        uint32_t decoded = mqtt_decode_remaining_length(client->rx_buffer + idx + 1, &remain_len);
        if (decoded == 0) break;
        
        uint32_t packet_len = 1 + decoded + remain_len;
        if (idx + packet_len > client->rx_len) break;
        
        switch (packet_type) {
            case MQTT_PKT_CONNACK: {
                int result = mqtt_parse_connack(client->rx_buffer + idx, packet_len);
                if (result == 0) {
                    client->is_connected = true;
                    client->state = MQTT_STATE_CONNECTED;
                    MQTT_LOG("MQTT Connected!");
                    if (client->callbacks.on_connect) {
                        client->callbacks.on_connect();
                    }
                } else {
                    client->state = MQTT_STATE_ERROR;
                    MQTT_LOG("MQTT Connection failed");
                }
                break;
            }
            case MQTT_PKT_PUBLISH: {
                mqtt_parse_publish(client, client->rx_buffer + idx, packet_len);
                break;
            }
            case MQTT_PKT_SUBACK: {
                int result = mqtt_parse_suback(client, client->rx_buffer + idx, packet_len);
                if (result == 0) {
                    client->state = MQTT_STATE_SUBSCRIBED;
                    MQTT_LOG("Subscribe confirmed");
                }
                break;
            }
            case MQTT_PKT_PINGRESP: {
                mqtt_parse_pingresp(client->rx_buffer + idx, packet_len);
                break;
            }
            case MQTT_PKT_PUBACK:
            case MQTT_PKT_PUBREC:
            case MQTT_PKT_PUBREL:
            case MQTT_PKT_PUBCOMP:
            case MQTT_PKT_UNSUBACK:
                MQTT_LOG("Received packet type: 0x%02X", packet_type);
                break;
            default:
                MQTT_LOG("Unknown packet type: 0x%02X", packet_type);
                break;
        }
        
        idx += packet_len;
    }
    
    if (idx > 0 && idx < client->rx_len) {
        memmove(client->rx_buffer, client->rx_buffer + idx, client->rx_len - idx);
        client->rx_len -= idx;
    } else if (idx >= client->rx_len) {
        client->rx_len = 0;
    }
    
    return 0;
}

void mqtt_client_loop(mqtt_client_t *client) {
    if (!client) return;
    if (!client->is_connected) return;
    
    uint32_t now = client->transport.get_tick();
    
    if (now - client->last_ping_tick > (client->connect_info.keepalive * 1000 / 2)) {
        MQTT_LOG("Keepalive: sending ping");
        mqtt_client_ping(client);
    }
    
    if (now - client->last_rx_tick > (client->connect_info.keepalive * 1000 * 2)) {
        MQTT_LOG("Keepalive timeout, disconnecting");
        client->is_connected = false;
        client->state = MQTT_STATE_DISCONNECTED;
        if (client->callbacks.on_disconnect) {
            client->callbacks.on_disconnect();
        }
    }
}

mqtt_state_t mqtt_client_get_state(mqtt_client_t *client) {
    return client ? client->state : MQTT_STATE_ERROR;
}

bool mqtt_client_is_connected(mqtt_client_t *client) {
    return client ? client->is_connected : false;
}