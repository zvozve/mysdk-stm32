#ifndef __HLK_RM58S_H
#define __HLK_RM58S_H

#include <stdint.h>
#include <stdbool.h>
#include "oop_dwt.h"
#include "oop_gpio_drv.h"
#include "oop_uart_drv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================
 * ★ 时间宏：使用 DWT，裸机/RTOS 通用 ★
 * =========================== */
// #define HLK_GET_TICK_MS()   (oop_GetCycleCount() / (SystemCoreClock / 1000))
#define HLK_GET_TICK_MS()   HAL_GetTick()
typedef enum {
    HLK_STATE_IDLE = 0,
    HLK_STATE_CONFIGURING,
    HLK_STATE_WAITING_LINK,
    HLK_STATE_LINKED,
    HLK_STATE_ERROR
} hlk_state_t;

typedef struct {
    const char *wifi_ssid;
    const char *wifi_password;
    const char *wifi_security;
    const char *remote_ip;
    const char *remote_port;
    const char *remote_protocol;
    uint32_t    uart_baudrate;
    uint8_t     uart_databits;
    char        uart_parity;
    uint8_t     uart_stopbits;
    uint16_t    uart_pack_len;
    uint16_t    uart_pack_timeout_ms;
    bool        dhcp_enable;
    bool        auto_reconnect;
    uint32_t    response_timeout_ms;
    uint8_t     max_retry;
} hlk_params_t;

typedef void (*hlk_rx_callback_t)(uint8_t *data, uint16_t len, void *user_ctx);
typedef void (*hlk_state_callback_t)(hlk_state_t state, void *user_ctx);

typedef struct hlk_drv {
    gpio_dev_t *gpio_es;
    gpio_dev_t *gpio_rst;
    gpio_dev_t *gpio_sts;
    gpio_dev_t *gpio_sot;
    uart_drv_t *uart;

    hlk_params_t params;

    volatile hlk_state_t state;
    uint32_t state_change_tick;

    struct {
        uint8_t  cmd_index;
        uint8_t  retry_count;
        uint32_t start_tick;
        uint32_t last_rx_tick;
        bool     waiting_response;
        char     response[384];
        uint16_t response_len;
        uint8_t  step;
        uint32_t step_start_tick;
        uint32_t step3_start_tick;
    } cfg;

    struct {
        uint32_t check_tick;
        uint8_t  retry_count;
    } link;

    struct {
        uint8_t fail_count;
    } reconnect;

    hlk_rx_callback_t   rx_cb;
    void               *rx_ctx;
    hlk_state_callback_t state_cb;
    void               *state_ctx;

    bool initialized;
} hlk_drv_t;

hlk_drv_t* hlk_create(void *gpio_es, void *gpio_rst, void *gpio_sts, void *gpio_sot,
                      void *uart_drv, const hlk_params_t *params);
void hlk_destroy(hlk_drv_t *drv);
void hlk_register_callbacks(hlk_drv_t *drv,
                            hlk_rx_callback_t rx_cb, void *rx_ctx,
                            hlk_state_callback_t state_cb, void *state_ctx);
void hlk_config(hlk_drv_t *drv);
void hlk_reconfig(hlk_drv_t *drv, const hlk_params_t *params);
int hlk_send(hlk_drv_t *drv, const uint8_t *data, uint16_t len);
void hlk_process(hlk_drv_t *drv);
hlk_state_t hlk_get_state(const hlk_drv_t *drv);
const char* hlk_state_string(hlk_state_t state);

#ifdef __cplusplus
}
#endif

#endif