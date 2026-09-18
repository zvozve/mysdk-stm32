/**
 * @file    ws1850s.c
 * @brief   WS1850S RFID 读卡器驱动实现（异步 / 非阻塞）
 * @version V1.0
 * @date    2026-09-18
 *
 * @note    移植自原工程「异步版」（mcu-dev）的 UART 状态机思路，重写为 SDK 风格：
 *          - 去除所有 CubeMX 头 / 全局句柄 / HAL_* 直调；
 *          - UART 传输改走 chip.oop_uart（uart_drv_t* 注入，拉取模型）；
 *          - 计时走 chip.oop_dwt（oop_GetTickMS）；
 *          - 不阻塞：ws1850s_process() 每轮主动取走已就绪的串口帧并解析。
 */

#include "ws1850s.h"
#include "oop_uart_drv.h"
#include "oop_gpio_drv.h"
#include "oop_dwt.h"
#include "SEGGER_RTT_Log.h"
#include <stdlib.h>
#include <string.h>

/* =========================== 内部工具 =========================== */
static uint8_t ws1850s_checksum(const uint8_t *data, uint16_t len) {
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++) sum ^= data[i];
    return sum;
}

static void ws1850s_set_state(ws1850s_drv_t *d, ws1850s_state_t s) {
    if (d->state != s) {
        d->state = s;
        d->state_change_tick = oop_GetTickMS();
        if (d->state_cb) d->state_cb(s, d->state_ctx);
    }
}

/* 组帧并发送（非阻塞）。帧: [0x60][dev][len_h][len_l][cmd][params...][chk]
   len 字段 = params 字节数（不含 cmd），与原始工程一致。 */
static void ws1850s_send_command(ws1850s_drv_t *d, uint8_t cmd,
                                const uint8_t *params, uint16_t param_len) {
    uint8_t buf[WS1850S_UART_BUF_LEN];
    uint16_t pos = 0;
    memset(buf, 0, sizeof(buf));

    buf[pos++] = WS1850S_FRAME_START;
    buf[pos++] = d->device_id;
    buf[pos++] = (uint8_t)((param_len) >> 8);   /* 数据长度高字节 */
    buf[pos++] = (uint8_t)(param_len);          /* 数据长度低字节（=params 长度） */
    buf[pos++] = cmd;
    if (params != NULL && param_len > 0) {
        memcpy(&buf[pos], params, param_len);
        pos += param_len;
    }
    buf[pos++] = ws1850s_checksum(buf, pos);     /* 校验和覆盖 [0,pos-1] */

    uart_drv_send(d->uart, buf, pos);
}

/* 解析一帧。返回 0=正常帧, 1=错误帧(已解析), <0=畸形。
   帧结构: idx0=start, idx1=dev, idx2/3=len(大端), idx4=cmd, idx5..=data[len], last=chk */
static int ws1850s_parse_frame(const uint8_t *buf, uint16_t len,
                               uint8_t *out_cmd, const uint8_t **out_data,
                               uint16_t *out_data_len, uint8_t *out_status) {
    if (len < 6) return -1;
    uint8_t start = buf[0];
    if (start != WS1850S_FRAME_START && start != WS1850S_FRAME_START_ERR) return -2;

    uint16_t declared = ((uint16_t)buf[2] << 8) | buf[3];
    uint16_t frame_len = 6 + declared;
    if (len < frame_len) return -3;                       /* 帧未收全（拉取模型下一般不发生） */

    uint8_t chk = ws1850s_checksum(buf, frame_len - 1);
    if (chk != buf[frame_len - 1]) return -4;             /* 校验失败 */

    *out_cmd = buf[4];
    *out_data = &buf[5];
    *out_data_len = declared;

    if (start == WS1850S_FRAME_START_ERR) {
        *out_status = buf[4];                            /* 错误帧：状态在 idx4 */
        return 1;
    }
    *out_status = WS1850S_STATUS_SUCCESS;
    return 0;
}

/* 命令应答中提取卡片信息（忠实移植原工程解析） */
static void ws1850s_update_card_from_response(ws1850s_drv_t *d, uint8_t cmd,
                                             const uint8_t *data, uint16_t len) {
    if (cmd == WS1850S_CMD_GET_CARD_UID && len >= 2) {
        /* 原工程解析：data[0]=uid 长度(type 镜像), data[1]=type, data[2..]=uid */
        d->card.uid_len = data[0];
        d->card.type[1] = data[0];
        d->card.type[0] = data[1];
        uint16_t n = (data[0] < WS1850S_MAX_UID_LEN) ? data[0] : WS1850S_MAX_UID_LEN;
        if (n > (uint16_t)(len - 2)) n = (uint16_t)(len - 2); /* 不越过实际帧长 */
        memcpy(d->card.uid, &data[2], n);
    } else if (cmd == WS1850S_CMD_READ_M1_DATA) {
        uint16_t n = (len < WS1850S_MAX_DATA_LEN) ? len : WS1850S_MAX_DATA_LEN;
        memcpy(d->card.block_data, data, n);
    }
}

/* 主动读卡片帧（MODE_READ_M1_UID_DATA）: data[0]=0x04, data[1..4]=uid(4), data[5..]=block(16) */
static void ws1850s_parse_active_card(const uint8_t *data, uint16_t len, ws1850s_card_t *card) {
    if (len < 5) return;
    card->uid_len = 4;
    card->type[0] = 0x00;
    card->type[1] = data[0];                  /* 主动读模式下 data[0] 即模式/类型字节 */
    memcpy(card->uid, &data[1], 4);
    uint16_t n = (len >= 21) ? 16 : (len > 5 ? (len - 5) : 0);
    if (n > WS1850S_MAX_DATA_LEN) n = WS1850S_MAX_DATA_LEN;
    if (n > 0) memcpy(card->block_data, &data[5], n);
}

/* =========================== 初始化状态机 =========================== */
static void ws1850s_init_send_current(ws1850s_drv_t *d) {
    uint8_t p[9];
    uint16_t plen;
    switch (d->init_step) {
        case 0:
            p[0] = WS1850S_RING_ON; plen = 1;
            d->pending_cmd = WS1850S_CMD_QUERY_ADDR;
            break;
        case 1:
            p[0] = WS1850S_RING_ON; p[1] = d->write_device_id; plen = 2;
            d->pending_cmd = WS1850S_CMD_WRITE_ADDR;
            break;
        case 2:
            p[0] = WS1850S_RING_ON; p[1] = d->work_mode; p[2] = d->block_addr;
            p[3] = WS1850S_UP_MODE_AUTO; p[4] = WS1850S_RING_ON;
            p[5] = 0x00; p[6] = 0x00; p[7] = WS1850S_UP_ONCE; p[8] = 0x01;
            plen = 9;
            d->pending_cmd = WS1850S_CMD_WRITE_MODE;
            break;
        default:
            return;
    }
    d->waiting_response = true;
    d->resp_start_tick = oop_GetTickMS();
    ws1850s_send_command(d, d->pending_cmd, p, plen);
    WS1850S_LOG("init step %d send cmd=0x%02X", d->init_step, d->pending_cmd);
}

static void ws1850s_init_advance(ws1850s_drv_t *d, uint8_t status) {
    if (d->state != WS1850S_STATE_INITIALIZING) return;

    if (status != WS1850S_STATUS_SUCCESS) {
        d->init_retry++;
        if (d->init_retry >= d->max_retry) {
            WS1850S_LOG("init failed at step %d (status=0x%02X)", d->init_step, status);
            ws1850s_set_state(d, WS1850S_STATE_ERROR);
            return;
        }
        WS1850S_LOG("init step %d retry %d", d->init_step, d->init_retry);
        ws1850s_init_send_current(d);
        return;
    }

    d->init_retry = 0;
    d->init_step++;
    if (d->init_step > 2) {
        WS1850S_LOG("init done, READY (mode=0x%02X block=0x%02X)",
                    d->work_mode, d->block_addr);
        ws1850s_set_state(d, WS1850S_STATE_READY);
        return;
    }
    ws1850s_init_send_current(d);
}

/* =========================== 帧处理 =========================== */
static void ws1850s_handle_packet(ws1850s_drv_t *d, const uint8_t *pkt, uint16_t len) {
    uint8_t cmd = 0, status = 0;
    const uint8_t *data = NULL;
    uint16_t data_len = 0;

    int pr = ws1850s_parse_frame(pkt, len, &cmd, &data, &data_len, &status);
    if (pr < 0) {
        WS1850S_LOG("bad frame (len=%d)", len);
        return;
    }
    if (pr == 1) { /* 错误帧 */
        WS1850S_LOG("error frame cmd=0x%02X status=0x%02X", cmd, status);
    }

    if (d->waiting_response && cmd == d->pending_cmd) {
        d->waiting_response = false;
        d->result.cmd = cmd;
        d->result.status = (pr == 1) ? status : WS1850S_STATUS_SUCCESS;
        d->result.data_len = data_len;
        if (data_len > 0 && data != NULL) {
            uint16_t n = (data_len < WS1850S_UART_BUF_LEN) ? data_len : WS1850S_UART_BUF_LEN;
            memcpy(d->result.data, data, n);
        }
        ws1850s_update_card_from_response(d, cmd, data, data_len);
        ws1850s_init_advance(d, d->result.status);

        /* 无论成功/错误/超时，统一经 result_cb 返回 */
        if (d->result_cb) d->result_cb(&d->result, d->result_ctx);
    } else if (data_len > 0 && data[0] == WS1850S_MODE_READ_M1_UID_DATA) {
        /* 主动读卡片帧（未 solicited） */
        ws1850s_parse_active_card(data, data_len, &d->card);
        if (d->card_cb) d->card_cb(&d->card, d->card_ctx);
    } else {
        WS1850S_LOG("unsolicited frame cmd=0x%02X (ignored)", cmd);
    }
}

/* =========================== 公共 API =========================== */
ws1850s_drv_t* ws1850s_create(uart_drv_t *uart, const ws1850s_params_t *params) {
    if (uart == NULL) return NULL;

    ws1850s_drv_t *d = (ws1850s_drv_t *)calloc(1, sizeof(ws1850s_drv_t));
    if (d == NULL) return NULL;

    d->uart = uart;

    if (params != NULL) {
        d->device_id       = params->device_id;
        d->write_device_id = params->write_device_id;
        d->work_mode       = params->work_mode;
        d->block_addr      = params->block_addr;
        d->resp_timeout_ms = params->resp_timeout_ms ? params->resp_timeout_ms : 1000;
        d->max_retry       = params->max_retry ? params->max_retry : 3;
        d->gpio_approach   = params->gpio_approach;
    } else {
        d->device_id       = WS1850S_DEFAULT_ID;
        d->write_device_id = WS1850S_WRITE_ID;
        d->work_mode       = WS1850S_MODE_READ_M1_UID_DATA;
        d->block_addr      = 0x02;
        d->resp_timeout_ms = 1000;
        d->max_retry       = 3;
        d->gpio_approach   = NULL;
    }

    /* 拉取模型：不注册 oop_uart 回调，由 ws1850s_process 主动取包 */
    uart_drv_reg_cb(d->uart, NULL, NULL, NULL);

    d->state = WS1850S_STATE_OFFLINE;
    d->initialized = true;
    WS1850S_LOG("created (mode=0x%02X block=0x%02X)", d->work_mode, d->block_addr);
    return d;
}

void ws1850s_destroy(ws1850s_drv_t *d) {
    if (d == NULL) return;
    free(d);
}

void ws1850s_register_callbacks(ws1850s_drv_t *d,
    ws1850s_result_cb_t result_cb, void *result_ctx,
    ws1850s_card_cb_t   card_cb,   void *card_ctx,
    ws1850s_state_cb_t  state_cb,  void *state_ctx) {
    if (d == NULL) return;
    d->result_cb = result_cb; d->result_ctx = result_ctx;
    d->card_cb   = card_cb;   d->card_ctx   = card_ctx;
    d->state_cb  = state_cb;  d->state_ctx  = state_ctx;
}

void ws1850s_start(ws1850s_drv_t *d) {
    if (d == NULL || !d->initialized) return;
    d->init_step  = 0;
    d->init_retry = 0;
    ws1850s_set_state(d, WS1850S_STATE_INITIALIZING);
    ws1850s_init_send_current(d);
}

void ws1850s_stop(ws1850s_drv_t *d) {
    if (d == NULL) return;
    d->waiting_response = false;
    ws1850s_set_state(d, WS1850S_STATE_OFFLINE);
}

void ws1850s_process(ws1850s_drv_t *d) {
    if (d == NULL || !d->initialized) return;

    /* 1) 拉取已就绪的串口数据包，逐个解析 */
    uint8_t pkt[WS1850S_UART_BUF_LEN];
    while (uart_drv_available(d->uart) > 0) {
        uint16_t plen = 0;
        uint8_t *p = uart_drv_get_packet(d->uart, &plen);
        if (p == NULL || plen == 0) {
            uart_drv_release_packet(d->uart);
            break;
        }
        uint16_t copy = (plen < WS1850S_UART_BUF_LEN) ? plen : WS1850S_UART_BUF_LEN;
        memcpy(pkt, p, copy);
        uart_drv_release_packet(d->uart);   /* 取完即释放，接收持续进行 */
        ws1850s_handle_packet(d, pkt, copy);
    }

    /* 2) 待响应命令超时看门狗 */
    if (d->waiting_response) {
        uint32_t el = oop_GetTickMS() - d->resp_start_tick;
        if (el > d->resp_timeout_ms) {
            d->waiting_response = false;
            d->result.cmd = d->pending_cmd;
            d->result.status = WS1850S_STATUS_TIMEOUT;
            d->result.data_len = 0;
            if (d->state == WS1850S_STATE_INITIALIZING) {
                d->init_retry++;
                if (d->init_retry >= d->max_retry) {
                    ws1850s_set_state(d, WS1850S_STATE_ERROR);
                } else {
                    ws1850s_init_send_current(d); /* 重发当前步 */
                }
            } else if (d->result_cb) {
                d->result_cb(&d->result, d->result_ctx);
            }
        }
    }
}

void ws1850s_query_device_address(ws1850s_drv_t *d) {
    if (d == NULL || !d->initialized) return;
    uint8_t p[1] = { WS1850S_RING_ON };
    d->pending_cmd = WS1850S_CMD_QUERY_ADDR;
    d->waiting_response = true;
    d->resp_start_tick = oop_GetTickMS();
    ws1850s_send_command(d, WS1850S_CMD_QUERY_ADDR, p, 1);
}

void ws1850s_get_card_uid(ws1850s_drv_t *d) {
    if (d == NULL || !d->initialized) return;
    uint8_t p[3] = { WS1850S_RING_ON, 0x26, 0x0A }; /* 提示音开，IDLE 寻卡，天线复位 0ms */
    d->pending_cmd = WS1850S_CMD_GET_CARD_UID;
    d->waiting_response = true;
    d->resp_start_tick = oop_GetTickMS();
    ws1850s_send_command(d, WS1850S_CMD_GET_CARD_UID, p, 3);
}

void ws1850s_read_m1_block(ws1850s_drv_t *d, uint8_t block_addr) {
    if (d == NULL || !d->initialized) return;
    uint8_t p[4] = { WS1850S_RING_ON, 0x26, 0x60, block_addr }; /* IDLE，KeyA 认证 */
    d->pending_cmd = WS1850S_CMD_READ_M1_DATA;
    d->waiting_response = true;
    d->resp_start_tick = oop_GetTickMS();
    ws1850s_send_command(d, WS1850S_CMD_READ_M1_DATA, p, 4);
}

ws1850s_state_t ws1850s_get_state(const ws1850s_drv_t *d) {
    return (d != NULL) ? d->state : WS1850S_STATE_OFFLINE;
}

bool ws1850s_is_card_approaching(const ws1850s_drv_t *d) {
    if (d == NULL || d->gpio_approach == NULL) return false;
    return oop_gpio_read(d->gpio_approach);
}

const ws1850s_card_t* ws1850s_get_last_card(const ws1850s_drv_t *d) {
    return (d != NULL) ? &d->card : NULL;
}
