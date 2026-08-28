#ifndef __WOL_H
#define __WOL_H

#include "config_network.h"
#include "lwip/err.h"
#include <stdbool.h>

#ifndef WOL_LOG_ENABLE
    #define WOL_LOG_ENABLE     1
#endif
#include "SEGGER_RTT_Log.h"
#define WOL_LOG(fmt, ...)     RTT_LOG_TAG(WOL_LOG_ENABLE,    "WOL",    fmt, ##__VA_ARGS__)

/**
 * @brief Send WOL magic packet via LAN Ethernet frame
 * @retval ERR_OK on success, others on failure
 */
err_t send_wol(void);

/**
 * @brief Print target MAC address for debugging
 */
void wol_print_mac(void);

/**
 * @brief Check if LAN network interface is ready
 * @retval true if ready, false otherwise
 */
bool wol_check_network_ready(void);

#endif