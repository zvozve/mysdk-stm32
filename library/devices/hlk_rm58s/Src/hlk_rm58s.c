#include "hlk_rm58s.h"
#include "oop_gpio_drv.h"
#include "oop_uart_drv.h"
#include "oop_dwt.h"
#include "SEGGER_RTT_Log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ===========================
 * AT指令表
 * =========================== */
typedef struct {
    const char *cmd_fmt;
    const char *expect;
    uint32_t timeout_ms;
    uint8_t max_retry;
} at_cmd_entry_t;

static const at_cmd_entry_t g_at_cmds[] = {
    {"at+netmode=2",                "ok",      3000, 3},
    {"at+wifi_conf=%s,%s,%s",       "ok",      5000, 3},
    {"at+dhcpc=%s",                 "ok",      3000, 3},
    {"at+remoteip=%s",              "ok",      3000, 3},
    {"at+remoteport=%s",            "ok",      3000, 3},
    {"at+Get_MAC=?",                "Get_MAC", 3000, 2},
    {"at+remotepro=%s",             "ok",      2000, 2},
    {"at+mode=client",              "ok",      2000, 2},
    {"at+uart=%lu,8,n,1",           "ok",      3000, 3},
    {"at+uartpacklen=%d",           "ok",      3000, 3},
    {"at+uartpacktimeout=%d",       "ok",      3000, 3},
    {"at+net_commit=1",             "ok",      5000, 5},
    {"at+reconn=%s",                "ok",      2000, 2},
};

#define AT_CMD_COUNT (sizeof(g_at_cmds) / sizeof(at_cmd_entry_t))

static const hlk_params_t g_default_params = {
    .wifi_ssid          = "",
    .wifi_password      = "",
    .wifi_security      = "open",
    .remote_ip          = "",
    .remote_port        = "",
    .remote_protocol    = "tcp",
    .uart_baudrate      = 115200,
    .uart_databits      = 8,
    .uart_parity        = 'n',
    .uart_stopbits      = 1,
    .uart_pack_len      = 512,
    .uart_pack_timeout_ms = 20,
    .dhcp_enable        = true,
    .auto_reconnect     = true,
    .response_timeout_ms = 3000,
    .max_retry          = 3,
};

/* 注：不再依赖全局实例，改由 hlk_process() 主动拉取接收（方案 B） */

/* ===========================
 * 函数声明
 * =========================== */
static void hlk_set_state(hlk_drv_t *d, hlk_state_t state);
static void hlk_send_next_cmd(hlk_drv_t *d);
static void hlk_handle_response(hlk_drv_t *d);
static bool hlk_parse_response(const char *resp, const char *expect);
static void hlk_config_step_process(hlk_drv_t *d);
static void hlk_start_config(hlk_drv_t *d);
static void hlk_do_gpio_reset(hlk_drv_t *d);
static void hlk_do_gpio_reset_high(hlk_drv_t *d);
static void hlk_do_gpio_enter_at_mode(hlk_drv_t *d);
static void hlk_do_gpio_exit_at_mode(hlk_drv_t *d);
static bool hlk_gpio_read_sta(hlk_drv_t *d);
static bool hlk_gpio_read_sot(hlk_drv_t *d);
static void hlk_uart_poll_rx(hlk_drv_t *d);
static void hlk_build_cmd(char *buf, uint16_t len, uint8_t idx, const hlk_params_t *p);

/* ===========================
 * 状态工具
 * =========================== */
const char* hlk_state_string(hlk_state_t state) {
    static const char *const names[] = {"IDLE", "CONFIGURING", "WAITING_LINK", "LINKED", "ERROR"};
    return (state < sizeof(names)/sizeof(names[0])) ? names[state] : "UNKNOWN";
}

static void hlk_set_state(hlk_drv_t *d, hlk_state_t state) {
    if (d->state != state) {
        DBG_LOG("HLK: %s -> %s", hlk_state_string(d->state), hlk_state_string(state));
        d->state = state;
        d->state_change_tick = HLK_GET_TICK_MS();
        if (d->state_cb) d->state_cb(state, d->state_ctx);
    }
}

/* ===========================
 * AT指令拼接
 * =========================== */
static void hlk_build_cmd(char *buf, uint16_t len, uint8_t idx, const hlk_params_t *p) {
    const at_cmd_entry_t *entry = &g_at_cmds[idx];
    switch (idx) {
        case 0:  snprintf(buf, len, "%s", entry->cmd_fmt); break;
        case 1:  snprintf(buf, len, entry->cmd_fmt,
                          p->wifi_ssid ? p->wifi_ssid : "",
                          p->wifi_security ? p->wifi_security : "open",
                          p->wifi_password ? p->wifi_password : ""); break;
        case 2:  snprintf(buf, len, entry->cmd_fmt, p->dhcp_enable ? "1" : "0"); break;
        case 3:  snprintf(buf, len, entry->cmd_fmt, p->remote_ip ? p->remote_ip : ""); break;
        case 4:  snprintf(buf, len, entry->cmd_fmt, p->remote_port ? p->remote_port : ""); break;
        case 5:  snprintf(buf, len, "%s", entry->cmd_fmt); break;
        case 6:  snprintf(buf, len, entry->cmd_fmt, p->remote_protocol ? p->remote_protocol : "tcp"); break;
        case 7:  snprintf(buf, len, "%s", entry->cmd_fmt); break;
        case 8:  snprintf(buf, len, entry->cmd_fmt, (unsigned long)p->uart_baudrate); break;
        case 9:  snprintf(buf, len, entry->cmd_fmt, p->uart_pack_len); break;
        case 10: snprintf(buf, len, entry->cmd_fmt, p->uart_pack_timeout_ms); break;
        case 11: snprintf(buf, len, "%s", entry->cmd_fmt); break;
        case 12: snprintf(buf, len, entry->cmd_fmt, p->auto_reconnect ? "1" : "0"); break;
        default: buf[0] = '\0'; break;
    }
}

/* ===========================
 * GPIO操作
 * =========================== */
static void hlk_do_gpio_reset(hlk_drv_t *d) {
    if (d->gpio_rst == NULL) return;
    oop_gpio_set_low(d->gpio_rst);
    DBG_LOG("HLK: RST LOW");
}

static void hlk_do_gpio_reset_high(hlk_drv_t *d) {
    if (d->gpio_rst == NULL) return;
    oop_gpio_set_high(d->gpio_rst);
    DBG_LOG("HLK: RST HIGH");
}

static void hlk_do_gpio_enter_at_mode(hlk_drv_t *d) {
    if (d->gpio_es == NULL) return;
    oop_gpio_set_low(d->gpio_es);
    DBG_LOG("HLK: ES LOW");
}

static void hlk_do_gpio_exit_at_mode(hlk_drv_t *d) {
    if (d->gpio_es == NULL) return;
    oop_gpio_set_high(d->gpio_es);
    DBG_LOG("HLK: ES HIGH");
}

static bool hlk_gpio_read_sta(hlk_drv_t *d) {
    return d->gpio_sts ? oop_gpio_read(d->gpio_sts) : false;
}

static bool hlk_gpio_read_sot(hlk_drv_t *d) {
    return d->gpio_sot ? oop_gpio_read(d->gpio_sot) : false;
}

/* ===========================
 * UART 接收（拉取模型 / 方案 B）
 * 由 hlk_process() 每轮主动调用：取走 oop_uart 已就绪的数据包，
 * 处理完后必须 release 以重启 DMA 接收 —— 这是与旧版
 * RxEventCallback 内 HLK_StartReceive() 等价的"连续接收"语义。
 * =========================== */
static void hlk_uart_poll_rx(hlk_drv_t *d) {
    uint16_t len = uart_drv_available(d->uart);
    if (len == 0) return;

    uint8_t *data = uart_drv_get_packet(d->uart, &len);
    if (data == NULL || len == 0) {
        uart_drv_release_packet(d->uart);
        return;
    }

    if (d->state == HLK_STATE_CONFIGURING) {
        if (d->cfg.waiting_response) {
            uint16_t remain = sizeof(d->cfg.response) - d->cfg.response_len - 1;
            uint16_t copy = len < remain ? len : remain;
            if (copy > 0) {
                memcpy(d->cfg.response + d->cfg.response_len, data, copy);
                d->cfg.response_len += copy;
                d->cfg.response[d->cfg.response_len] = '\0';
                d->cfg.last_rx_tick = HLK_GET_TICK_MS();
            }
        }
    } else if (d->state == HLK_STATE_LINKED && d->rx_cb) {
        d->rx_cb(data, len, d->rx_ctx);
    }
    /* 拉取模型关键：无论是否处理、处理何种状态，都必须释放包以重启接收 */
    uart_drv_release_packet(d->uart);
}

/* ===========================
 * 响应解析
 * =========================== */
static bool hlk_parse_response(const char *resp, const char *expect) {
    if (resp == NULL || *resp == '\0') return false;
    if (expect == NULL) return true;

    const char *p = resp;
    while (*p) {
        const char *p2 = p, *e2 = expect;
        while (*p2 && *e2 && tolower(*p2) == tolower(*e2)) { p2++; e2++; }
        if (*e2 == '\0') return true;
        p++;
    }
    return false;
}

/* ===========================
 * AT指令调度
 * =========================== */
static void hlk_send_next_cmd(hlk_drv_t *d) {
    if (d->cfg.cmd_index >= AT_CMD_COUNT) {
        DBG_LOG("HLK: All AT cmds done, hw reset to apply config");
        /* 对齐旧版 HLK_GPIO_ActiveReset：配置完成后硬件复位，
           使配置写入并退出 AT 模式，模块随后连接 WiFi */
        hlk_do_gpio_reset(d);                 /* RST LOW */
        d->cfg.step = 6;
        d->cfg.step_start_tick = HLK_GET_TICK_MS();
        return;
    }

    char cmd_buf[128];
    hlk_build_cmd(cmd_buf, sizeof(cmd_buf), d->cfg.cmd_index, &d->params);

    DBG_LOG("HLK CMD[%d/%d]: %s", d->cfg.cmd_index + 1, AT_CMD_COUNT, cmd_buf);

    d->cfg.waiting_response = true;
    d->cfg.start_tick = HLK_GET_TICK_MS();
    d->cfg.last_rx_tick = d->cfg.start_tick;
    d->cfg.response_len = 0;
    memset(d->cfg.response, 0, sizeof(d->cfg.response));

    char tx_buf[136];
    snprintf(tx_buf, sizeof(tx_buf), "%s", cmd_buf);
    uart_drv_send(d->uart, (uint8_t*)tx_buf, strlen(tx_buf));
}

static void hlk_handle_response(hlk_drv_t *d) {
    if (d->cfg.cmd_index >= AT_CMD_COUNT) return;

    const at_cmd_entry_t *entry = &g_at_cmds[d->cfg.cmd_index];
    uint32_t elapsed = HLK_GET_TICK_MS() - d->cfg.start_tick;
    uint32_t timeout = (d->params.response_timeout_ms > 0) ? d->params.response_timeout_ms : entry->timeout_ms;

    if (elapsed > timeout) {
        DBG_LOG("HLK: TIMEOUT [%d]", d->cfg.cmd_index);
        d->cfg.waiting_response = false;
        d->cfg.retry_count++;

        if (d->cfg.retry_count >= d->params.max_retry) {
            DBG_LOG("HLK: Max retry, restart config");
            d->cfg.step = 1;
            d->cfg.step_start_tick = HLK_GET_TICK_MS();
            hlk_do_gpio_reset(d);
        } else {
            hlk_send_next_cmd(d);
        }
        return;
    }

    if (d->cfg.waiting_response && d->cfg.response_len > 0) {
        bool matched = hlk_parse_response(d->cfg.response, entry->expect);
        bool has_newline = (d->cfg.response[d->cfg.response_len - 1] == '\n' ||
                           d->cfg.response[d->cfg.response_len - 1] == '\r');

        if (matched) {
            DBG_LOG("HLK: CMD OK [%d]", d->cfg.cmd_index);
            d->cfg.cmd_index++;
            d->cfg.retry_count = 0;
            d->cfg.waiting_response = false;
            hlk_send_next_cmd(d);
        } else if (has_newline) {
            DBG_LOG("HLK: Unexpected: %s", d->cfg.response);
            d->cfg.waiting_response = false;
            d->cfg.retry_count++;
            if (d->cfg.retry_count >= d->params.max_retry) {
                DBG_LOG("HLK: Max retry, restart config");
                d->cfg.step = 1;
                d->cfg.step_start_tick = HLK_GET_TICK_MS();
                hlk_do_gpio_reset(d);
            } else {
                hlk_send_next_cmd(d);
            }
        }
    }
}

/* ===========================
 * 轮询版配置状态机
 * =========================== */
static void hlk_config_step_process(hlk_drv_t *d) {
    uint32_t now = HLK_GET_TICK_MS();

    switch (d->cfg.step) {
        case 0: 
            break;
            
        case 1:
            hlk_do_gpio_reset(d);
            d->cfg.step = 2;
            d->cfg.step_start_tick = now;
            DBG_LOG("HLK: Step: RST LOW, wait 600ms");
            break;
            
        case 2:
            if (now - d->cfg.step_start_tick >= 600) {
                hlk_do_gpio_reset_high(d);
                d->cfg.step = 3;
                d->cfg.step3_start_tick = now;   /* ★ 用新变量 */
                DBG_LOG("HLK: Step: RST HIGH, wait 2000ms");
            }
            break;
            
        case 3:
            if (now - d->cfg.step3_start_tick >= 2000) {  /* ★ 用新变量 */
                d->cfg.step = 4;
                d->cfg.step_start_tick = now;
                DBG_LOG("HLK: Step: Enter AT mode");
                hlk_do_gpio_enter_at_mode(d);
            }
            break;
            
        case 4:
            if (now - d->cfg.step_start_tick >= 100) {
                hlk_do_gpio_exit_at_mode(d);
                d->cfg.step = 5;
                d->cfg.step_start_tick = now;
                DBG_LOG("HLK: Step: ES HIGH, wait 200ms");
            }
            break;
            
        case 5:
            if (now - d->cfg.step_start_tick >= 200) {
                d->cfg.step = 0;
                d->cfg.cmd_index = 0;
                d->cfg.retry_count = 0;
                d->cfg.waiting_response = false;
                d->cfg.response_len = 0;
                DBG_LOG("HLK: Step: Start sending AT commands");
                hlk_send_next_cmd(d);
            }
            break;

        case 6:
            /* 配置完成后硬件复位：RST 保持低 ≥600ms（对齐旧版 ActiveReset） */
            if (now - d->cfg.step_start_tick >= 600) {
                hlk_do_gpio_reset_high(d);    /* RST HIGH */
                d->cfg.step = 7;
                d->cfg.step_start_tick = now;
                DBG_LOG("HLK: Step: RST HIGH, wait 100ms");
            }
            break;

        case 7:
            if (now - d->cfg.step_start_tick >= 100) {
                d->cfg.step = 0;
                d->link.check_tick = HLK_GET_TICK_MS();
                d->link.retry_count = 0;
                DBG_LOG("HLK: Step: reset done, enter WAITING_LINK");
                hlk_set_state(d, HLK_STATE_WAITING_LINK);
            }
            break;

        default:
            d->cfg.step = 0;
            break;
    }
}

static void hlk_start_config(hlk_drv_t *d) {
    DBG_LOG("HLK: Start config");
    d->cfg.cmd_index = 0;
    d->cfg.retry_count = 0;
    d->cfg.waiting_response = false;
    d->cfg.response_len = 0;
    d->cfg.step = 1;
    d->cfg.step_start_tick = HLK_GET_TICK_MS();
    d->link.retry_count = 0;
    d->reconnect.fail_count++;

    hlk_set_state(d, HLK_STATE_CONFIGURING);
}

/* ===========================
 * 状态机处理
 * =========================== */
static void hlk_state_configuring(hlk_drv_t *d) {
    if (d->cfg.step != 0) {
        hlk_config_step_process(d);
        return;
    }
    hlk_handle_response(d);
}

static void hlk_state_waiting_link(hlk_drv_t *d) {
    uint32_t now = HLK_GET_TICK_MS();
    if (now - d->link.check_tick < 1000) return;
    d->link.check_tick = now;

    bool sta = hlk_gpio_read_sta(d);
    bool sot = hlk_gpio_read_sot(d);

    DBG_LOG("HLK: Check STA=%d SOT=%d retry=%d/%d", sta, sot, d->link.retry_count, 20);

    if (sta && sot) {
        DBG_LOG("HLK: Connected!");
        hlk_set_state(d, HLK_STATE_LINKED);
        d->reconnect.fail_count = 0;
        d->link.retry_count = 0;
        return;
    }

    d->link.retry_count++;
    if (d->link.retry_count >= 20) {
        DBG_LOG("HLK: Link timeout, reconfig");
        d->link.retry_count = 0;
        hlk_start_config(d);
    }
}

static void hlk_state_linked(hlk_drv_t *d) {
    uint32_t now = HLK_GET_TICK_MS();
    if (now - d->link.check_tick < 1000) return;
    d->link.check_tick = now;

    bool sta = hlk_gpio_read_sta(d);
    bool sot = hlk_gpio_read_sot(d);

    if (!sta || !sot) {
        DBG_LOG("HLK: Lost! STA=%d SOT=%d", sta, sot);
        hlk_start_config(d);
    }
}

/* ===========================
 * API实现
 * =========================== */
hlk_drv_t* hlk_create(void *gpio_es, void *gpio_rst, void *gpio_sts, void *gpio_sot,
                      void *uart_drv, const hlk_params_t *params) {
    if (uart_drv == NULL) return NULL;

    hlk_drv_t *d = (hlk_drv_t*)calloc(1, sizeof(hlk_drv_t));
    if (d == NULL) return NULL;

    d->gpio_es   = (gpio_dev_t*)gpio_es;
    d->gpio_rst  = (gpio_dev_t*)gpio_rst;
    d->gpio_sts  = (gpio_dev_t*)gpio_sts;
    d->gpio_sot  = (gpio_dev_t*)gpio_sot;
    d->uart      = (uart_drv_t*)uart_drv;

    if (params != NULL) d->params = *params;
    else d->params = g_default_params;

    if (d->gpio_es) oop_gpio_set_high(d->gpio_es);
    if (d->gpio_rst) oop_gpio_set_high(d->gpio_rst);

    /* 改用拉取模型：不注册接收回调，由 hlk_process() 主动 uart_drv_available/get/release */
    uart_drv_reg_cb(d->uart, NULL, NULL, NULL);

    d->state = HLK_STATE_IDLE;
    d->cfg.step = 0;
    d->initialized = true;

    DBG_LOG("HLK: Created");
    return d;
}

void hlk_destroy(hlk_drv_t *d) {
    if (d == NULL) return;
    free(d);
}

void hlk_register_callbacks(hlk_drv_t *d,
                            hlk_rx_callback_t rx_cb, void *rx_ctx,
                            hlk_state_callback_t state_cb, void *state_ctx) {
    if (d == NULL) return;
    d->rx_cb = rx_cb;
    d->rx_ctx = rx_ctx;
    d->state_cb = state_cb;
    d->state_ctx = state_ctx;
}

void hlk_config(hlk_drv_t *d) {
    if (d == NULL || !d->initialized) return;
    d->reconnect.fail_count = 0;
    hlk_start_config(d);
}

void hlk_reconfig(hlk_drv_t *d, const hlk_params_t *params) {
    if (d == NULL || params == NULL) return;
    d->params = *params;
    hlk_config(d);
}

int hlk_send(hlk_drv_t *d, const uint8_t *data, uint16_t len) {
    if (d == NULL || data == NULL || len == 0) return -1;
    if (!d->initialized) return -2;
    if (d->state != HLK_STATE_LINKED) return -3;
    return uart_drv_send(d->uart, data, len);
}

void hlk_process(hlk_drv_t *d) {
    if (d == NULL || !d->initialized) return;

    /* 拉取模型：每轮先取走已就绪的串口数据包（取完即释放，接收持续进行） */
    hlk_uart_poll_rx(d);

    switch (d->state) {
        case HLK_STATE_CONFIGURING:  hlk_state_configuring(d);   break;
        case HLK_STATE_WAITING_LINK: hlk_state_waiting_link(d);  break;
        case HLK_STATE_LINKED:       hlk_state_linked(d);        break;
        default: break;
    }
}

hlk_state_t hlk_get_state(const hlk_drv_t *d) {
    return d ? d->state : HLK_STATE_IDLE;
}