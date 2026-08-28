/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : lan8720a_reset.c
  * @brief          : Ethernet PHY hardware reset routine
  ******************************************************************************
  */
/* USER CODE END Header */
#include "lan8720a_reset.h"
#include "main.h"
#include "bsp_dwt.h"
#include "bsp_gpio_drv.h"
#include "SEGGER_RTT_Log.h"

static gpio_dev_t eth_rst_io = {0};   /* [0]=SEL1(A) [1]=SEL2(B) */

static void lan_delay_ms(uint32_t ms)
{
  HAL_Delay(ms);
}

void ETH_RST_Init(void)
{
  bsp_gpio_init_output(&eth_rst_io,    ETH_RST_GPIO_Port, ETH_RST_Pin, true);
  ETH_RST_Execute();
}

void ETH_RST_Execute(void)
{
  if (!bsp_gpio_is_initialized(&eth_rst_io)) return;

  bsp_gpio_write(&eth_rst_io, false);
  lan_delay_ms(50);
  bsp_gpio_write(&eth_rst_io, true);
  lan_delay_ms(50);

  SYS_LOG("LAN8720A Reset Done!");
}
