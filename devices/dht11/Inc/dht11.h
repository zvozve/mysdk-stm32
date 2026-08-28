/**
 * @file    dht11.h
 * @brief   DHT11 温湿度传感器驱动
 * @version V3.1
 * @date    2026-08-25
 */

#ifndef __DHT11_H
#define __DHT11_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 数据结构 ========== */

typedef struct {
    uint8_t hum_int;      /* 湿度整数部分 */
    uint8_t hum_dec;      /* 湿度小数部分 */
    uint8_t temp_int;     /* 温度整数部分 */
    uint8_t temp_dec;     /* 温度小数部分 */
    uint8_t checksum;     /* 校验和 */
    bool    is_valid;     /* 数据是否有效 */
} DHT11_Data_t;

/* ========== API ========== */

void DHT11_Init(void);
bool DHT11_Read(DHT11_Data_t *pData);

#ifdef __cplusplus
}
#endif

#endif /* __DHT11_H */