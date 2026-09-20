#ifndef __SEGGER_RTT_LOG_H
#define __SEGGER_RTT_LOG_H

#include <stdint.h>

/* ============================================================
 * 时间戳来源（middleware 层不碰 HAL / RTOS）
 * ------------------------------------------------------------
 * 本头不再 include hal_platform.h，也不再 include FreeRTOS.h：
 * 时基一律取 chip 层的唯一出口 oop_GetTickMS()（内部自行区分 RTOS / 裸机）。
 * 这里只做「外部符号声明」，不是 include —— 于是
 *   - include 顺序无关：不管谁先 include 本头都不会有 implicit declaration；
 *   - 未拉取 chip.oop_dwt 时在**链接期**报 undefined reference（fail-fast），
 *     而不是像旧写法那样在 tick 停摆时静默打成 00:00:00。
 * 需要换时基（如 PC 单测传假 tick）：-DRTT_GET_TICK=你的可调用表达式。
 * ============================================================ */
#ifndef RTT_GET_TICK
    extern uint32_t oop_GetTickMS(void);
    #define RTT_GET_TICK()      oop_GetTickMS()
#endif

#ifndef RTT_LOG_ENABLE
    #define RTT_LOG_ENABLE     1
#endif

#ifndef RTT_LOG_MAX_HOURS
    #define RTT_LOG_MAX_HOURS  24
#endif

#if RTT_LOG_ENABLE
    #include "SEGGER_RTT.h"

    // ✅ 手动拼接时间戳，避免 snprintf
    static inline void RTT_log_format_time(uint32_t tick, char *buf, size_t buf_size) {
        (void)buf_size;
        uint32_t ms = tick;
        uint32_t hours = ms / 3600000;
        ms -= hours * 3600000;
        if (hours >= RTT_LOG_MAX_HOURS) {
            hours = hours % RTT_LOG_MAX_HOURS;
        }
        uint32_t minutes = ms / 60000;
        ms -= minutes * 60000;
        uint32_t seconds = ms / 1000;
        ms -= seconds * 1000;

        char *p = buf;
        *p++ = '0' + (hours / 10);
        *p++ = '0' + (hours % 10);
        *p++ = ':';
        *p++ = '0' + (minutes / 10);
        *p++ = '0' + (minutes % 10);
        *p++ = ':';
        *p++ = '0' + (seconds / 10);
        *p++ = '0' + (seconds % 10);
        *p++ = '.';
        *p++ = '0' + (ms / 100);
        *p++ = '0' + ((ms / 10) % 10);
        *p++ = '0' + (ms % 10);
        *p = '\0';
    }

    // ✅ 带缓冲区检查的日志输出
    #define RTT_LOG_EMIT(tag, fmt, ...)                                          \
        do {                                                                     \
            char _time_buf[16];                                                  \
            RTT_log_format_time(RTT_GET_TICK(), _time_buf, sizeof(_time_buf));   \
            SEGGER_RTT_printf(0, "[%s][%s] " fmt "\r\n", _time_buf, tag, ##__VA_ARGS__); \
        } while (0)

    #define RTT_LOG_TAG(enable, tag, fmt, ...)                     \
        do {                                                       \
            if (enable) { RTT_LOG_EMIT(tag, fmt, ##__VA_ARGS__); } \
        } while (0)

    #define HEX_PRINT(enable, prefix, data, len)                                   \
        do {                                                                       \
            if (enable) {                                                          \
                char _time_buf[16];                                                \
                RTT_log_format_time(RTT_GET_TICK(), _time_buf, sizeof(_time_buf)); \
                SEGGER_RTT_printf(0, "[%s][HEX] %s", _time_buf, prefix);           \
                for (size_t _i = 0; _i < (len); _i++) {                            \
                    SEGGER_RTT_printf(0, "%02X ", ((uint8_t*)(data))[_i]);         \
                }                                                                  \
                SEGGER_RTT_printf(0, "\r\n");                                      \
            }                                                                      \
        } while (0)

#else
    #define RTT_LOG_TAG(enable, tag, fmt, ...)   ((void)0)
    #define HEX_PRINT(enable, prefix, data, len) ((void)0)
#endif

// ===========================
// 标签日志定义
// ===========================
#define RTT_LOG(fmt, ...)     RTT_LOG_TAG(1, "RTT", fmt, ##__VA_ARGS__)

#ifndef SYS_LOG_ENABLE
    #define SYS_LOG_ENABLE     1
#endif
#define SYS_LOG(fmt, ...)     RTT_LOG_TAG(SYS_LOG_ENABLE,    "SYS",    fmt, ##__VA_ARGS__)

#ifndef ERR_LOG_ENABLE
    #define ERR_LOG_ENABLE     1
#endif
#define ERR_LOG(fmt, ...)     RTT_LOG_TAG(ERR_LOG_ENABLE,    "ERR",    fmt, ##__VA_ARGS__)

#ifndef WARN_LOG_ENABLE
    #define WARN_LOG_ENABLE    1
#endif
#define WARN_LOG(fmt, ...)    RTT_LOG_TAG(WARN_LOG_ENABLE,   "WARN",   fmt, ##__VA_ARGS__)

#ifndef INFO_LOG_ENABLE
    #define INFO_LOG_ENABLE    1
#endif
#define INFO_LOG(fmt, ...)    RTT_LOG_TAG(INFO_LOG_ENABLE,   "INFO",   fmt, ##__VA_ARGS__)

#ifndef DBG_LOG_ENABLE
    #define DBG_LOG_ENABLE     0
#endif
#define DBG_LOG(fmt, ...)     RTT_LOG_TAG(DBG_LOG_ENABLE,    "DBG",    fmt, ##__VA_ARGS__)

#ifndef HEX_LOG_ENABLE
    #define HEX_LOG_ENABLE     0
#endif
#define HEX_LOG(prefix, data, len) HEX_PRINT(HEX_LOG_ENABLE, prefix, data, len)

// ============================================================
// 本文件只保留「通用」标签（RTT / SYS / ERR / WARN / INFO / DBG / HEX），
// 这些是跨模块通用的诊断标签，无法归属到具体设备/协议。
//
// ⚠️ 模块专属标签（APP / UART / MODBUS / HMI / ...）不要在此集中定义，
//    否则本文件会随项目增多无限膨胀。请在各模块「最底层」头文件内自行定义，
//    复用本文件提供的 RTT_LOG_TAG 引擎即可，例如：
//   - MODBUS_LOG -> library/protocols/Modbus/Inc/modbus_core.h
//   - UART_LOG    -> library/chip/oop_uart/Inc/oop_uart_drv.h
//   - HMI_LOG     -> library/devices/delta_dop_107/Inc/delta_dop_107.h
//   - APP_LOG     -> 工程 User/Inc/app_main.h（应用层）
//    模块头只需 #include "SEGGER_RTT_Log.h" 即可拿到 RTT_LOG_TAG。
// ============================================================

#endif // __SEGGER_RTT_LOG_H