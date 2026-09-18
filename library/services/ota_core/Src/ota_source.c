/**
 * @file    ota_source.c
 * @brief   取数后端接口的通用实现
 * @version V1.0
 * @date    2026-09-18
 *
 * ota_source.h 是纯接口头（各通道各写一个实现），这里只放与具体通道无关的
 * 通用函数。此前 ota_source_check() 只有声明没有实现 —— 谁第一个真的调用
 * ota_flow_start() 谁才会链接失败，这种「声明了但没有实体」的函数最该在
 * 建模块当天就补上。
 */

#include <stddef.h>
#include "ota_source.h"

int ota_source_check(const ota_source_t *s)
{
    if (s == NULL || s->open == NULL || s->read == NULL) {
        return OTA_ERR_PARAM;
    }
    return OTA_OK;
}
