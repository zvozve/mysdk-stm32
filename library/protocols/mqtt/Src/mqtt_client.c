#include "mqtt_client.h"
#include "MQTTPacket.h"   /* Paho MQTTPacket: pulls all codec headers + low-level helpers */
#include <string.h>
#include <stdlib.h>

#include "SEGGER_RTT_Log.h"

// ===========================
// 内部辅助
// ===========================
// 剩余长度解码（仅用于 process_rx 的包重组；线缆编码已由 Paho 完成）
static uint32_t mqtt_decode_remaining_length(const uint8_t *buf, uint32_t *len) {
    uint32_t multiplier = 1;
    uint32_t value = 0;
    uint32_t idx = 0;
    uint8_t byte;

    do {
        if (idx >= 4) return 0;
        byte = buf[idx++];
        value += (byte & 0x7F) * multiplier;
        multiplier *= 128;
    } while (byte & 0x80);

    *len = value;
    return idx;
}

static uint16_t mqtt_calc_packet_id(mqtt_client_t *client) {
    uint16_t id = client->next_packet_id++;
    if (client->next_packet_id == 0) client->next_packet_id = 1;
    return id;
}

// ===========================
// 初始化 / 配置（保持不变）
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
// 构建 CONNECT（Paho MQTTPacket）
// ===========================
static int mqtt_build_connect(mqtt_client_t *client, uint8_t *buf, uint16_t *len) {
    MQTTPacket_connectData opt = MQTTPacket_connectData_initializer;

    opt.clientID.cstring = client->connect_info.client_id;
    opt.keepAliveInterval = client->connect_info.keepalive;
    opt.cleansession = client->connect_info.clean_session ? 1 : 0;
    opt.MQTTVersion = 4; /* MQTT 3.1.1 */

    if (strlen(client->connect_info.username) > 0)
        opt.username.cstring = client->connect_info.username;
    if (strlen(client->connect_info.password) > 0)
        opt.password.cstring = client->connect_info.password;

    int rc = MQTTSerialize_connect(buf, MQTT_MAX_PACKET_SIZE, &opt);
    if (rc <= 0) {
        MQTT_LOG("CONNECT serialize failed: %d", rc);
        return -1;
    }
    *len = (uint16_t)rc;
    MQTT_LOG("Built CONNECT packet, len=%d", rc);
    return 0;
}

// ===========================
// 构建 PUBLISH（Paho MQTTPacket）
// ===========================
static int mqtt_build_publish(mqtt_client_t *client, uint8_t *buf, uint16_t *len,
                               const char *topic, const uint8_t *payload, uint16_t payload_len,
                               mqtt_qos_t qos, bool retained) {
    MQTTString topicStr = MQTTString_initializer;
    topicStr.cstring = (char *)(uintptr_t)topic;

    uint16_t packet_id = 0;
    if (qos > MQTT_QOS_0) packet_id = mqtt_calc_packet_id(client);

    int rc = MQTTSerialize_publish(buf, MQTT_MAX_PACKET_SIZE, 0, qos, retained ? 1 : 0,
                                   packet_id, topicStr, (unsigned char *)payload, payload_len);
    if (rc <= 0) {
        MQTT_LOG("PUBLISH serialize failed: %d", rc);
        return -1;
    }
    *len = (uint16_t)rc;
    MQTT_LOG("Built PUBLISH: topic=%s, len=%d, qos=%d", topic, rc, qos);
    return (int)packet_id;
}

// ===========================
// 构建 SUBSCRIBE（Paho MQTTPacket）
// ===========================
static int mqtt_build_subscribe(mqtt_client_t *client, uint8_t *buf, uint16_t *len,
                                 const char *topic, mqtt_qos_t qos) {
    uint16_t packet_id = mqtt_calc_packet_id(client);
    MQTTString topicFilters[1];
    int reqQoS[1];

    topicFilters[0].cstring = (char *)(uintptr_t)topic;
    reqQoS[0] = qos;

    int rc = MQTTSerialize_subscribe(buf, MQTT_MAX_PACKET_SIZE, 0, packet_id, 1, topicFilters, reqQoS);
    if (rc <= 0) {
        MQTT_LOG("SUBSCRIBE serialize failed: %d", rc);
        return -1;
    }
    *len = (uint16_t)rc;
    MQTT_LOG("Built SUBSCRIBE: topic=%s, len=%d", topic, rc);
    return (int)packet_id;
}

// ===========================
// 构建 UNSUBSCRIBE（Paho MQTTPacket）
// ===========================
static int mqtt_build_unsubscribe(mqtt_client_t *client, uint8_t *buf, uint16_t *len,
                                   const char *topic) {
    uint16_t packet_id = mqtt_calc_packet_id(client);
    MQTTString topicFilters[1];

    topicFilters[0].cstring = (char *)(uintptr_t)topic;

    int rc = MQTTSerialize_unsubscribe(buf, MQTT_MAX_PACKET_SIZE, 0, packet_id, 1, topicFilters);
    if (rc <= 0) {
        MQTT_LOG("UNSUBSCRIBE serialize failed: %d", rc);
        return -1;
    }
    *len = (uint16_t)rc;
    MQTT_LOG("Built UNSUBSCRIBE: topic=%s, len=%d", topic, rc);
    return (int)packet_id;
}

// ===========================
// 构建 PINGREQ / DISCONNECT / PUBACK（Paho MQTTPacket）
// ===========================
static int mqtt_build_pingreq(uint8_t *buf, uint16_t *len) {
    int rc = MQTTSerialize_pingreq(buf, MQTT_MAX_PACKET_SIZE);
    if (rc <= 0) return -1;
    *len = (uint16_t)rc;
    MQTT_LOG("Built PINGREQ");
    return 0;
}

static int mqtt_build_disconnect(uint8_t *buf, uint16_t *len) {
    int rc = MQTTSerialize_disconnect(buf, MQTT_MAX_PACKET_SIZE);
    if (rc <= 0) return -1;
    *len = (uint16_t)rc;
    MQTT_LOG("Built DISCONNECT");
    return 0;
}

static int mqtt_build_puback(uint8_t *buf, uint16_t *len, uint16_t packet_id) {
    int rc = MQTTSerialize_puback(buf, MQTT_MAX_PACKET_SIZE, packet_id);
    if (rc <= 0) return -1;
    *len = (uint16_t)rc;
    return 0;
}

// ===========================
// 公共 API（签名保持不变）
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
    if (mqtt_build_connect(client, client->tx_buffer, &len) != 0) {
        client->state = MQTT_STATE_ERROR;
        return -2;
    }

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

    uint16_t len = 0;
    if (mqtt_build_disconnect(client->tx_buffer, &len) != 0) {
        MQTT_LOG("Disconnect build failed");
    } else if (client->transport.send) {
        int ret = client->transport.send(client->tx_buffer, len);
        if (ret != 0) MQTT_LOG("Disconnect send failed, ret=%d", ret);
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

    uint16_t pkt_len = 0;
    int packet_id = mqtt_build_publish(client, client->tx_buffer, &pkt_len,
                                        topic, payload, len, qos, retained);
    if (packet_id < 0) {
        MQTT_LOG("Publish build failed, rc=%d", packet_id);
        return -3;
    }

    if (client->transport.send) {
        int ret = client->transport.send(client->tx_buffer, pkt_len);
        if (ret != 0) {
            MQTT_LOG("Publish send failed, ret=%d", ret);
            return -3;
        }
    }

    if (client->callbacks.on_publish) {
        client->callbacks.on_publish((uint16_t)packet_id);
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

    uint16_t pkt_len = 0;
    int packet_id = mqtt_build_subscribe(client, client->tx_buffer, &pkt_len, topic, qos);
    if (packet_id < 0) {
        MQTT_LOG("Subscribe build failed, rc=%d", packet_id);
        return -3;
    }

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

    uint16_t pkt_len = 0;
    int packet_id = mqtt_build_unsubscribe(client, client->tx_buffer, &pkt_len, topic);
    if (packet_id < 0) {
        MQTT_LOG("Unsubscribe build failed, rc=%d", packet_id);
        return -3;
    }

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

    uint16_t len = 0;
    if (mqtt_build_pingreq(client->tx_buffer, &len) != 0) {
        MQTT_LOG("Ping build failed");
        return -3;
    }

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

        uint8_t *pkt = client->rx_buffer + idx;

        switch (packet_type) {
            case MQTT_PKT_CONNACK: {
                unsigned char sessionPresent = 0, connack_rc = 0;
                int rc = MQTTDeserialize_connack(&sessionPresent, &connack_rc, pkt, (int)packet_len);
                if (rc && connack_rc == 0) {
                    client->is_connected = true;
                    client->state = MQTT_STATE_CONNECTED;
                    MQTT_LOG("MQTT Connected!");
                    if (client->callbacks.on_connect) client->callbacks.on_connect();
                } else {
                    client->state = MQTT_STATE_ERROR;
                    MQTT_LOG("MQTT Connection failed (rc=%d)", connack_rc);
                }
                break;
            }
            case MQTT_PKT_PUBLISH: {
                unsigned char dup = 0;
                int qos = 0;
                unsigned char retained = 0;
                unsigned short pid = 0;
                MQTTString topicName;
                unsigned char *payload = NULL;
                int payloadlen = 0;

                int rc = MQTTDeserialize_publish(&dup, &qos, &retained, &pid,
                                                 &topicName, &payload, &payloadlen, pkt, (int)packet_len);
                if (rc) {
                    char topic[MQTT_MAX_TOPIC_LEN];
                    int tlen = topicName.cstring ? (int)strlen(topicName.cstring) : 0;
                    if (tlen >= MQTT_MAX_TOPIC_LEN) tlen = MQTT_MAX_TOPIC_LEN - 1;
                    if (tlen > 0) memcpy(topic, topicName.cstring, tlen);
                    topic[tlen] = '\0';

                    MQTT_LOG("RX PUBLISH: topic=%s, qos=%d, len=%d", topic, qos, payloadlen);

                    if (client->callbacks.on_message)
                        client->callbacks.on_message(topic, payload, (uint16_t)payloadlen);

                    /* QoS1/2 入站必须由客户端回 PUBACK（Paho 不自动发） */
                    if (qos == 1 || qos == 2) {
                        uint16_t alen;
                        if (mqtt_build_puback(client->tx_buffer, &alen, pid) == 0 &&
                            client->transport.send)
                            client->transport.send(client->tx_buffer, alen);
                    }
                }
                break;
            }
            case MQTT_PKT_SUBACK: {
                unsigned short pid = 0;
                int count = 0;
                int grantedQoSs[1];
                int rc = MQTTDeserialize_suback(&pid, 1, &count, grantedQoSs, pkt, (int)packet_len);
                if (rc) {
                    int result = (count > 0) ? grantedQoSs[0] : -1;
                    MQTT_LOG("SUBACK: packet_id=%d, grantedQoS=%d", pid, result);
                    if (client->callbacks.on_subscribe)
                        client->callbacks.on_subscribe(NULL, (uint8_t)result);
                    if (result <= 0x80) client->state = MQTT_STATE_SUBSCRIBED;
                }
                break;
            }
            case MQTT_PKT_PINGRESP: {
                MQTT_LOG("Received PINGRESP");
                break;
            }
            case MQTT_PKT_PUBACK:
            case MQTT_PKT_PUBREC:
            case MQTT_PKT_PUBREL:
            case MQTT_PKT_PUBCOMP:
            case MQTT_PKT_UNSUBACK: {
                unsigned char ptype = 0, dup = 0;
                unsigned short pid = 0;
                int rc = MQTTDeserialize_ack(&ptype, &dup, &pid, pkt, (int)packet_len);
                if (rc) {
                    if (ptype == MQTT_PKT_PUBACK && client->callbacks.on_publish)
                        client->callbacks.on_publish(pid);
                    else
                        MQTT_LOG("RX ack: type=0x%02X pid=%d", ptype, pid);
                }
                break;
            }
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
