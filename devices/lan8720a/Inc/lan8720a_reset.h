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

#ifdef __cplusplus
extern "C" {
#endif

void ETH_RST_Init(void);
void ETH_RST_Execute(void);

#ifdef __cplusplus
}
#endif

#endif /* __ETH_PHY_H */
