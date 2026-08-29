#ifndef __MODBUS_CORE_H__

#define __MODBUS_CORE_H__



#include <stdint.h>

#include <stdbool.h>
#include "SEGGER_RTT_Log.h"   /* 复用 RTT_LOG_TAG 日志引擎；本模块专属标签在此定义，不进 SDK 公共头 */



#ifdef __cplusplus

extern "C" {

#endif



// ===========================
// 模块专属日志标签（仅本模块 .c 可见，避免污染 SEGGER_RTT_Log.h）
// ===========================
#ifndef MODBUS_LOG_ENABLE
    #define MODBUS_LOG_ENABLE  1
#endif
#define MODBUS_LOG(fmt, ...)  RTT_LOG_TAG(MODBUS_LOG_ENABLE, "MODBUS", fmt, ##__VA_ARGS__)

#define MB_RTU_DRVIER_VERSION "4.0.1"

#define MB_RTU_DRVIER_DATE "2026-08-14"



// ===========================

// 系统配置

// ===========================

#define MB_USE_RTOS       1    // 0: 裸机, 1: RTOS



// ===========================

// 时间相关宏

// ===========================

#if MB_USE_RTOS

    #include "FreeRTOS.h"

    #include "task.h"

    #define MB_GET_TICK()      xTaskGetTickCount()

    #define MB_Delay_ms(ms)    vTaskDelay(pdMS_TO_TICKS(ms))

#else

    #include "oop_dwt.h"   /* chip 层时序封装：oop_GetTickMS / oop_DelayMS（不直调 HAL） */

    #define MB_GET_TICK()      oop_GetTickMS()

    #define MB_Delay_ms(ms)    oop_DelayMS(ms)

#endif



// ===========================

// 传输开关（文件内宏门控，配合 CMake 统一引入所有源文件）
// 优先级：CMake -D 显式覆盖  >  board_cfg.h(BOARD_MODBUS_*_ENABLE)  >  SDK 安全默认
//   - 工程在 CMake 定义 MB_BOARD_CFG 并把 board_cfg.h 目录加入包含路径后，
//     本头读取 BOARD_MODBUS_RTU_ENABLE / BOARD_MODBUS_TCP_ENABLE。
//   - 未启用 MB_BOARD_CFG（如 SDK 独立编译 / 单测）时走下方安全默认：
//     RTU 默认开（仅依赖 UART，无外部栈），TCP 默认关（依赖 LwIP，无网口板不应编入）。

// ===========================

#ifdef MB_BOARD_CFG
#include "board_cfg.h"   // 工程板级绑定：仅读取功能开关宏，不引用任何具体引脚/句柄
#endif

#ifndef MODBUS_ENABLE_RTU
    #ifdef BOARD_MODBUS_RTU_ENABLE
        #define MODBUS_ENABLE_RTU    BOARD_MODBUS_RTU_ENABLE
    #else
        #define MODBUS_ENABLE_RTU    1
    #endif
#endif

#ifndef MODBUS_ENABLE_TCP
    #ifdef BOARD_MODBUS_TCP_ENABLE
        #define MODBUS_ENABLE_TCP    BOARD_MODBUS_TCP_ENABLE
    #else
        #define MODBUS_ENABLE_TCP    0
    #endif
#endif



// ===========================

// 配置

// ===========================

#define MODBUS_MAX_INSTANCES    4

#define MODBUS_BUF_SIZE         260   // RTU: addr+PDU(253)+CRC(2)=256; TCP: MBAP(7)+PDU(253)=260

#define MODBUS_BROADCAST_ADDR   0



// ===========================

// 功能码

// ===========================

typedef enum {

    MODBUS_FC_READ_COILS           = 0x01,

    MODBUS_FC_READ_DISCRETE_INPUTS = 0x02,

    MODBUS_FC_READ_HOLDING_REGS    = 0x03,

    MODBUS_FC_READ_INPUT_REGS      = 0x04,

    MODBUS_FC_WRITE_SINGLE_COIL    = 0x05,

    MODBUS_FC_WRITE_SINGLE_REG     = 0x06,

    MODBUS_FC_WRITE_MULTIPLE_COILS = 0x0F,

    MODBUS_FC_WRITE_MULTIPLE_REGS  = 0x10,

    MODBUS_FC_MASK_WRITE_REG       = 0x16,

    MODBUS_FC_READ_WRITE_MULT_REGS = 0x17,

} modbus_func_code_t;



// ===========================

// 异常码

// ===========================

typedef enum {

    MODBUS_EXCEPTION_ILLEGAL_FUNCTION   = 0x01,

    MODBUS_EXCEPTION_ILLEGAL_DATA_ADDR  = 0x02,

    MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE = 0x03,

    MODBUS_EXCEPTION_SLAVE_DEVICE_FAIL  = 0x04,

    MODBUS_EXCEPTION_ACKNOWLEDGE        = 0x05,

    MODBUS_EXCEPTION_SLAVE_DEVICE_BUSY  = 0x06,

    MODBUS_EXCEPTION_MEMORY_PARITY_ERR  = 0x08,

    MODBUS_EXCEPTION_GATEWAY_PATH_ERR   = 0x0A,

    MODBUS_EXCEPTION_GATEWAY_TARGET_ERR = 0x0B,

} modbus_exception_t;



// ===========================

// 模式

// ===========================

typedef enum {

    MODBUS_MODE_RTU,

    MODBUS_MODE_ASCII,

    MODBUS_MODE_TCP,

} modbus_mode_t;



// ===========================

// 角色

// ===========================

typedef enum {

    MODBUS_ROLE_MASTER,

    MODBUS_ROLE_SLAVE,

} modbus_role_t;



// ===========================

// 断线状态

// ===========================

typedef enum {

    MODBUS_LINE_OK = 0,

    MODBUS_LINE_TIMEOUT,

    MODBUS_LINE_DISCONNECTED,

} modbus_line_state_t;



// ===========================

// 实例状态

// ===========================

typedef enum {

    MODBUS_STATE_IDLE,

    MODBUS_STATE_SENDING,

    MODBUS_STATE_WAITING_RESPONSE,

    MODBUS_STATE_RECEIVING,

    MODBUS_STATE_PROCESSING,

    MODBUS_STATE_ERROR,

} modbus_state_t;



// ===========================

// 传输接口

// ===========================

typedef struct {

    int (*send)(void *ctx, const uint8_t *data, uint16_t len);

    uint16_t (*peek)(void *ctx);

    uint16_t (*recv)(void *ctx, uint8_t *buf, uint16_t len);

    uint32_t (*get_tick)(void);

    void (*delay)(uint32_t ms);

    /* 帧封装交给端口层：core 只处理纯 PDU（RTU:加/验CRC；TCP:加/剥MBAP）。

     * 未注册(NULL)时 core 直接收发 PDU（兼容裸传输）。 */

    uint16_t (*frame_tx)(void *ctx, const uint8_t *pdu, uint16_t pdu_len,

                         uint8_t *out, uint16_t out_cap);

    int      (*frame_rx)(void *ctx, uint8_t *raw, uint16_t *raw_len);

    void     (*port_poll)(void *ctx);   /* TCP: accept/connect/重连; RTU: 可 NULL */

    void *ctx;

} modbus_transport_t;



// ===========================

// Modbus实例（核心）

// ===========================

typedef struct modbus_instance {

    // 基本属性

    uint8_t slave_addr;

    modbus_role_t role;

    modbus_mode_t mode;

    modbus_state_t state;

    

    // 传输接口

    modbus_transport_t transport;

    

    // 帧缓冲区

    uint8_t tx_buf[MODBUS_BUF_SIZE];

    uint8_t rx_buf[MODBUS_BUF_SIZE];

    uint8_t tx_frame[MODBUS_BUF_SIZE];   /* frame_tx 输出（含 RTU CRC / TCP MBAP） */

    uint16_t tx_len;

    uint16_t rx_len;

    

    // 超时管理

    uint32_t send_tick;

    uint32_t response_timeout;

    uint32_t last_activity_tick;

    

    // 断线检测

    modbus_line_state_t line_state;

    uint8_t timeout_count;

    uint8_t max_timeout_count;

    uint32_t slave_timeout_ms;

    uint32_t reconnect_interval;

    uint32_t reconnect_tick;

    

    // 断线回调

    void (*on_line_break)(struct modbus_instance *ctx);

    void (*on_line_recover)(struct modbus_instance *ctx);

    

    // 从机数据映射

    struct {

        uint8_t *coils;

        uint16_t coils_size;

        uint8_t *discrete_inputs;

        uint16_t discrete_size;

        uint16_t *holding_regs;

        uint16_t holding_size;

        uint16_t *input_regs;

        uint16_t input_size;

    } data_map;

    

    // 主机事务

    struct {

        uint8_t *req_data;

        uint16_t req_len;

        uint8_t *resp_data;

        uint16_t *resp_len;

        uint8_t func_code;

        bool pending;

        bool completed;

        int result;

        void *job;              /* 当前事务对应的作业引用（仲裁器设置，应答钩子使用） */

    } transaction;

    

    // 主机轮询

    void (*poll_callback)(struct modbus_instance *ctx);

    uint32_t poll_interval;

    uint32_t last_poll_tick;



    // 主机应答分发钩子（modbus_master 仲裁器挂载）

    void (*on_master_response)(struct modbus_instance *ctx);

    void *master_priv;          /* 仲裁器实例（master 层私有数据） */

    

    // 主机缓存（检测变化用）

    uint16_t *last_regs;

    uint8_t *last_coils;

    uint16_t last_regs_count;

    uint16_t last_coils_count;

    bool reg_baseline_done;

    bool coil_baseline_done;

    uint16_t last_regs_start_addr;

    uint16_t last_coils_start_addr;



    // 主机变化回调

    void (*on_master_reg_change)(uint16_t addr, uint16_t old_val, uint16_t new_val);

    void (*on_master_coil_change)(uint16_t addr, bool old_val, bool new_val);

    

    // 实例链

    struct modbus_instance *next;

} modbus_t;



// ===========================

// 核心API

// ===========================

void modbus_init(modbus_t *ctx);

void modbus_set_transport(modbus_t *ctx, const modbus_transport_t *transport);

void modbus_set_role(modbus_t *ctx, modbus_role_t role);

void modbus_set_slave_addr(modbus_t *ctx, uint8_t addr);

void modbus_set_timeouts(modbus_t *ctx, uint32_t response_timeout_ms, 

                         uint32_t slave_timeout_ms, uint8_t max_retries);

void modbus_set_reconnect_interval(modbus_t *ctx, uint32_t interval_ms);

void modbus_set_line_callbacks(modbus_t *ctx, 

    void (*on_break)(modbus_t *), void (*on_recover)(modbus_t *));



void modbus_process(modbus_t *ctx);

void modbus_process_all(void);



modbus_state_t modbus_get_state(modbus_t *ctx);

modbus_line_state_t modbus_get_line_state(modbus_t *ctx);

uint32_t modbus_get_last_activity(modbus_t *ctx);



void modbus_register_instance(modbus_t *ctx);

void modbus_unregister_instance(modbus_t *ctx);



#ifdef __cplusplus

}

#endif



#endif // __MODBUS_CORE_H__