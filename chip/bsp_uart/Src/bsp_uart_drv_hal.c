// ==================== uart_drv_hal.c ====================
#include "bsp_uart_drv.h"

// 所有HAL回调统一桥接到驱动层
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    uart_drv_on_tx_done(huart);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    uart_drv_on_idle(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    uart_drv_on_error(huart);
}