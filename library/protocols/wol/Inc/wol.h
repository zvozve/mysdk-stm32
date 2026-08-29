#ifndef __WOL_H
#define __WOL_H

#include "lwip/err.h"
#include <stdint.h>
#include <stdbool.h>

/* LwIP 网口结构：接口仅用指针传递，前向声明即可（头文件自包含，
 * 不依赖包含方是否已引入 lwip/netif.h） */
struct netif;

#ifndef WOL_LOG_ENABLE
    #define WOL_LOG_ENABLE     1
#endif
#include "SEGGER_RTT_Log.h"
#define WOL_LOG(fmt, ...)     RTT_LOG_TAG(WOL_LOG_ENABLE,    "WOL",    fmt, ##__VA_ARGS__)

/**
 * @brief Send WOL magic packet via LAN Ethernet frame
 * @param target_mac  目标 MAC（由工程 board_cfg 注入，不依赖 config_network.h）
 * @param netif       局域网接口（由工程 LwIP 初始化后注入，SDK 不引用全局 gnetif）
 * @retval ERR_OK on success, others on failure
 */
err_t send_wol(const uint8_t target_mac[6], struct netif *netif);

/**
 * @brief Print target MAC address for debugging
 */
void wol_print_mac(const uint8_t mac[6]);

/**
 * @brief Check if LAN network interface is ready
 * @param netif  局域网接口（由工程注入）
 * @retval true if ready, false otherwise
 */
bool wol_check_network_ready(struct netif *netif);

#endif