/**
 * @file    hlk_rm58s_cmd.h
 * @brief   HLK-RM58S AT指令宏模板（内部使用）
 * @note    只定义字符串拼接模板，不包含具体参数值
 */

#ifndef __HLK_RM58S_CMD_H
#define __HLK_RM58S_CMD_H

/* ===========================
 * 基本指令
 * =========================== */
#define HLK_CMD_NETMODE(mode)           "at+netmode=" mode
#define HLK_CMD_WIFI_CONF(ssid, sec, pwd)  "at+wifi_conf=" ssid "," sec "," pwd
#define HLK_CMD_DHCPC(enable)           "at+dhcpc=" enable
#define HLK_CMD_REMOTEIP(ip)            "at+remoteip=" ip
#define HLK_CMD_REMOTEPORT(port)        "at+remoteport=" port
#define HLK_CMD_REMOTEPRO(proto)        "at+remotepro=" proto
#define HLK_CMD_MODE(mode)              "at+mode=" mode
#define HLK_CMD_UART(baud, bits, parity, stop) \
    "at+uart=" baud "," bits "," parity "," stop
#define HLK_CMD_UARTPACKLEN(len)        "at+uartpacklen=" len
#define HLK_CMD_UARTPACKTIMEOUT(ms)     "at+uartpacktimeout=" ms
#define HLK_CMD_NET_COMMIT(commit)      "at+net_commit=" commit
#define HLK_CMD_RECONN(enable)          "at+reconn=" enable
#define HLK_CMD_DEFAULT                 "at+default=1"
#define HLK_CMD_REBOOT                  "at+reboot=1"
#define HLK_CMD_QUERY_MAC               "at+Get_MAC=?"

/* ===========================
 * 响应期望关键字
 * =========================== */
#define HLK_EXPECT_OK           "ok"
#define HLK_EXPECT_GET_MAC      "Get_MAC"
#define HLK_EXPECT_ERROR        "error"
#define HLK_EXPECT_READY        "ready"

#endif /* __HLK_RM58S_CMD_H */