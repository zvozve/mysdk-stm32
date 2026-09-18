/**
 * @file    ws1850s.h
 * @brief   WS1850S RFID 读卡器驱动（异步 / 非阻塞，UART 帧协议）
 * @version V1.0
 * @date    2026-09-18
 *
 * @note    板无关：UART 传输走 chip.oop_uart（uart_drv_t* 注入）、计时走 chip.oop_dwt、
 *          日志走 middleware.SEGGER_RTT；不引用任何 CubeMX 符号、不直调 HAL_*。
 *
 *          设计为「异步 / 拉取模型」，对应原工程的「异步版」（mcu-dev）：
 *          应用周期性调用 ws1850s_process()，驱动内部状态机收包、解析、触发回调，全程不阻塞。
 *
 *          使用流程：
 *            1) 用 oop_uart 准备好 uart_drv_t（含 RS485 DE 如有），传入 ws1850s_create()；
 *            2) ws1850s_register_callbacks() 注册 结果 / 卡片 / 状态 回调；
 *            3) ws1850s_start() 启动初始化状态机（查询地址 → 写地址 → 写工作模式）；
 *            4) 周期调用 ws1850s_process()（任务 / 定时器 tick）；
 *            5) 卡片靠近时（工作模式设为主动读）自动触发 card_cb；
 *               需要主动读取某块数据时调用 ws1850s_read_m1_block()，结果经 result_cb 返回。
 */

#ifndef __WS1850S_H__
#define __WS1850S_H__

#include <stdint.h>
#include <stdbool.h>
#include "oop_dwt.h"
#include "oop_gpio_drv.h"
#include "oop_uart_drv.h"
#include "SEGGER_RTT_Log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================== 日志标签 =========================== */
#ifndef WS1850S_LOG_ENABLE
#define WS1850S_LOG_ENABLE  1
#endif
#define WS1850S_LOG(fmt, ...)  RTT_LOG_TAG(WS1850S_LOG_ENABLE, "WS1850S", fmt, ##__VA_ARGS__)

/* =========================== 帧格式 =========================== */
#define WS1850S_FRAME_START       0x60
#define WS1850S_FRAME_START_ERR   0xE0

/* =========================== 命令码 =========================== */
#define WS1850S_CMD_QUERY_ADDR    0x02
#define WS1850S_CMD_WRITE_ADDR    0x03
#define WS1850S_CMD_WRITE_MODE    0x04
#define WS1850S_CMD_GET_CARD_UID  0x11
#define WS1850S_CMD_READ_M1_DATA  0x12

/* =========================== 工作模式 =========================== */
#define WS1850S_MODE_CMD                 0x01
#define WS1850S_MODE_READ_ISO14443A_UID  0x02
#define WS1850S_MODE_READ_M1_DATA        0x03
#define WS1850S_MODE_READ_M1_UID_DATA    0x04
#define WS1850S_MODE_READ_15693_UID       0x05
#define WS1850S_MODE_READ_15693_DATA     0x06
#define WS1850S_MODE_READ_15693_UID_DATA 0x07
#define WS1850S_MODE_READ_G2_UID1        0x08
#define WS1850S_MODE_READ_G2_UID2        0x09
#define WS1850S_MODE_READ_SRI_UID        0x0A
#define WS1850S_MODE_READ_FELICA_UID     0x0B
#define WS1850S_MODE_READ_ALL            0x0F

/* 上传模式 */
#define WS1850S_UP_MODE_AUTO    0x00
#define WS1850S_UP_MODE_MANUAL  0x01
#define WS1850S_UP_ONCE         0x00
#define WS1850S_UP_CONTINUE     0x01
#define WS1850S_RING_ON         0x01
#define WS1850S_RING_OFF        0x00

/* =========================== 状态码 =========================== */
#define WS1850S_STATUS_SUCCESS      0x00
#define WS1850S_STATUS_NO_CARD      0xA2
#define WS1850S_STATUS_AUTH_FAILED  0xA3
#define WS1850S_STATUS_TIMEOUT      0xFE
#define WS1850S_STATUS_INVALID      0xFF

/* =========================== 尺寸 =========================== */
#define WS1850S_MAX_UID_LEN    10
#define WS1850S_MAX_DATA_LEN   16
#define WS1850S_UART_BUF_LEN   64
#define WS1850S_DEFAULT_ID     0x00
#define WS1850S_WRITE_ID       0x21

/* =========================== 状态 =========================== */
typedef enum {
    WS1850S_STATE_OFFLINE = 0,
    WS1850S_STATE_INITIALIZING,
    WS1850S_STATE_READY,
    WS1850S_STATE_ERROR
} ws1850s_state_t;

/* =========================== 数据结构 =========================== */
typedef struct {
    uint8_t  type[2];
    uint8_t  uid[WS1850S_MAX_UID_LEN];
    uint16_t uid_len;
    uint8_t  block_data[WS1850S_MAX_DATA_LEN];
} ws1850s_card_t;

typedef struct {
    uint8_t  cmd;                  /* 回声命令码 */
    uint8_t  status;               /* 0=成功，否则为状态/错误码 */
    uint8_t  data[WS1850S_UART_BUF_LEN];
    uint16_t data_len;
} ws1850s_result_t;

/* 回调类型 */
typedef void (*ws1850s_result_cb_t)(ws1850s_result_t *result, void *ctx);
typedef void (*ws1850s_card_cb_t)(const ws1850s_card_t *card, void *ctx);
typedef void (*ws1850s_state_cb_t)(ws1850s_state_t state, void *ctx);

typedef struct ws1850s_drv {
    uart_drv_t  *uart;
    gpio_dev_t  *gpio_approach;    /* 卡片靠近输入（可选，可 NULL） */

    uint8_t   device_id;
    uint8_t   write_device_id;
    uint8_t   work_mode;
    uint8_t   block_addr;
    uint16_t  resp_timeout_ms;
    uint8_t   max_retry;

    volatile ws1850s_state_t state;
    uint32_t  state_change_tick;

    /* 初始化状态机 */
    uint8_t   init_step;           /* 0=查地址 1=写地址 2=写模式 */
    uint8_t   init_retry;

    /* 待响应命令 */
    uint8_t   pending_cmd;
    bool      waiting_response;
    uint32_t  resp_start_tick;
    ws1850s_result_t result;

    ws1850s_card_t card;

    ws1850s_result_cb_t result_cb;  void *result_ctx;
    ws1850s_card_cb_t   card_cb;    void *card_ctx;
    ws1850s_state_cb_t  state_cb;   void *state_ctx;

    bool initialized;
} ws1850s_drv_t;

typedef struct {
    uint8_t   device_id;        /* 默认 0x00 */
    uint8_t   write_device_id;  /* 初始化写入的地址，默认 0x21 */
    uint8_t   work_mode;        /* 默认 WS1850S_MODE_READ_M1_UID_DATA */
    uint8_t   block_addr;       /* 主动读模式下的数据块地址 */
    uint16_t  resp_timeout_ms;  /* 单条命令应答超时 */
    uint8_t   max_retry;        /* 单条命令最大重试 */
    gpio_dev_t *gpio_approach;  /* 卡片靠近输入（可选） */
} ws1850s_params_t;

/* =========================== API =========================== */
ws1850s_drv_t* ws1850s_create(uart_drv_t *uart, const ws1850s_params_t *params);
void ws1850s_destroy(ws1850s_drv_t *d);

void ws1850s_register_callbacks(ws1850s_drv_t *d,
    ws1850s_result_cb_t result_cb, void *result_ctx,
    ws1850s_card_cb_t   card_cb,   void *card_ctx,
    ws1850s_state_cb_t  state_cb,  void *state_ctx);

/* 启动初始化状态机（查询地址 → 写地址 → 写工作模式），非阻塞 */
void ws1850s_start(ws1850s_drv_t *d);
void ws1850s_stop(ws1850s_drv_t *d);

/* 周期调用：收包 / 解析 / 状态机推进 / 超时，全程非阻塞 */
void ws1850s_process(ws1850s_drv_t *d);

/* 异步命令请求（非阻塞），结果经 result_cb 返回 */
void ws1850s_query_device_address(ws1850s_drv_t *d);
void ws1850s_get_card_uid(ws1850s_drv_t *d);
void ws1850s_read_m1_block(ws1850s_drv_t *d, uint8_t block_addr);

/* 只读访问 */
ws1850s_state_t ws1850s_get_state(const ws1850s_drv_t *d);
bool ws1850s_is_card_approaching(const ws1850s_drv_t *d);
const ws1850s_card_t* ws1850s_get_last_card(const ws1850s_drv_t *d);

#ifdef __cplusplus
}
#endif

#endif /* __WS1850S_H__ */
