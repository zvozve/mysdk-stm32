# protocols.mqtt — MQTT 3.1.1 客户端

## 功能说明

纯 MQTT 3.1.1 客户端（仅 TCP 1883，不含 TLS）。**线缆层编解码由 vendored 的 Eclipse Paho `MQTTPacket` 实现**（目录内 `MQTTPacket.*` / `MQTTConnect*` / `MQTTPublish.*` / `MQTTSubscribe*` / `MQTTUnsubscribe*` / `MQTTFormat.*` / `StackTrace.*`，协议层来自 Eclipse Paho，EPL/EDL 双许可，零 HAL / 零 OS 依赖），SDK 自有的 `mqtt_client.*` 只负责：客户端状态机、传输注入、rx 包重组、keepalive、回调分发。

- **传输由用户注入**（`mqtt_transport_t{send,get_tick,delay}`），可接 LwIP socket / 串口 / TLS 任意后端，**不绑定 LwIP**。
- **零 RTOS 依赖**，裸机 F103（Cortex-M3）可直接用；只要 `get_tick` 基于已配置好的 `SystemCoreClock` 即可。
- 协议版本固定 **MQTT 3.1.1**（`MQTTVersion = 4`）。

## 目录

| 文件 | 来源 / 职责 |
|---|---|
| `mqtt_client.h/.c` | 本 SDK：客户端 API + 状态机 + rx 重组 + loop |
| `MQTTPacket.*` 等 7 对 | Eclipse Paho（vendored）：协议编解码，勿改 |
| `mqtt_json_payload.h/.c` | **demo 业务负载示例**（device_status_t→JSON），非协议层，详见下 |

## 用法

```c
mqtt_transport_t tp = {
    .send      = my_net_send,     // 用户实现：把 buf/len 发到 broker
    .get_tick  = oop_GetTickMS,   // 用户实现：ms 时间戳（chip 层唯一时基出口，勿直调 HAL_GetTick）
    .delay     = my_delay_ms,
};
mqtt_client_t cli;
mqtt_client_init(&cli, &tp);
mqtt_client_set_connect_info(&cli, "dev001", NULL, NULL);
mqtt_client_connect(&cli, 60, true);

// 收到网络数据后交给协议层重组/分发
mqtt_client_process_rx(&cli, net_buf, net_len);

// 任务循环里调用，负责 keepalive 与超时断线
mqtt_client_loop(&cli);
```

订阅/发布/取消订阅通过 `mqtt_client_subscribe / publish / unsubscribe`；`on_message` 回调里拿到的是指向内部 `rx_buffer` 的指针，**回调内应自行拷贝**，不要长期持有。

## 坑位

1. **transport 三件套必须齐**：`send/get_tick/delay` 任一为 NULL 都会行为异常；`get_tick` 的时基必须是已初始化的 `SystemCoreClock`（裸机要先跑 `SystemClock_Config`）。
2. **入站 PUBLISH 自动回 PUBACK**：QoS1/2 的入站消息由驱动内部自动回 PUBACK（Paho 不替客户端发），应用无需处理；QoS0 不回。
3. **`mqtt_json_payload.*` 是 demo，不是协议层**：它只是把 `device_status_t` 序列化成 JSON 的示例，依赖 `middleware.cJSON`，属于应用/任务层职责。接入工程应在自己的 app/task 里自带同类负载序列化，可删除本文件或用自身实现覆盖。
4. **与 LwIP 自带 mqtt 冲突**：若工程启用了 CubeMX 的 `LwIP/apps/mqtt/mqtt.c`，须 EXCLUDE 它，避免与 `protocols.mqtt` 同名符号冲突（见根 `readme.md` LwIP 避坑）。
5. **Paho 源勿改**：`MQTT*`/`StackTrace*` 来自上游，改动请保留许可头并尽量回馈上游；协议行为问题优先在 `mqtt_client.c` 侧处理。

## 驱动版本

V1.0（线缆编解码切换为 Paho MQTTPacket）
