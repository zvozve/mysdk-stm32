/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : lan8720a_reset.c
  * @brief          : Ethernet PHY hardware reset routine
  ******************************************************************************
  */
/* USER CODE END Header */
#include "lan8720a_reset.h"
#include "hal_platform.h"   /* GPIO_TypeDef / HAL 类型（不依赖工程 main.h） */
#include "oop_dwt.h"
#include "oop_gpio_drv.h"
#include "SEGGER_RTT_Log.h"

static gpio_dev_t eth_rst_io = {0};   /* PHY 复位引脚，端口/引脚由 ETH_RST_Init 注入 */

static void lan_delay_ms(uint32_t ms)
{
  oop_DelayMS(ms);
}

void ETH_RST_Init(GPIO_TypeDef *port, uint16_t pin)
{
  oop_gpio_init_output(&eth_rst_io, port, pin, true);
  ETH_RST_Execute();
}

void ETH_RST_Execute(void)
{
  if (!oop_gpio_is_initialized(&eth_rst_io)) return;

  oop_gpio_write(&eth_rst_io, false);
  lan_delay_ms(50);
  oop_gpio_write(&eth_rst_io, true);
  lan_delay_ms(50);

  SYS_LOG("LAN8720A Reset Done!");
}
