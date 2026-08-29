/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : eth_phy.h
  * @brief          : Ethernet PHY hardware reset
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __ETH_PHY_H
#define __ETH_PHY_H

#include <stdint.h>
#include "hal_platform.h"   /* GPIO_TypeDef（头文件自包含，不依赖包含顺序） */

#ifdef __cplusplus
extern "C" {
#endif

void ETH_RST_Init(GPIO_TypeDef *port, uint16_t pin);
void ETH_RST_Execute(void);

#ifdef __cplusplus
}
#endif

#endif /* __ETH_PHY_H */
