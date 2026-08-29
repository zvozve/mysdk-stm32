#include "oop_uart_drv.h"
// #include "stm32g4xx_hal_uart_ex.h"   /* HAL_UARTEx_GetRxEventType */
#include "SEGGER_RTT_Log.h"
#include <string.h>

// ===========================
// 实例管理
//   ★ SDK 不记录任何具体句柄/IO；huart 由上层（工程 board_cfg）注入 ★
// ===========================
#define UART_DRV_MAX_INSTANCES   8   /* UART1~UART8 全覆盖，仅容量，不绑定具体实例 */

static uart_drv_t *s_drv_table[UART_DRV_MAX_INSTANCES];
static int s_drv_count = 0;

static void uart_drv_register_instance(uart_drv_t *drv) {
    if (s_drv_count < UART_DRV_MAX_INSTANCES) {
        s_drv_table[s_drv_count++] = drv;
    }
}

static uart_drv_t* uart_drv_find(UART_HandleTypeDef *huart) {
    for (int i = 0; i < s_drv_count; i++) {
        if (s_drv_table[i] != NULL && s_drv_table[i]->huart == huart) {
            return s_drv_table[i];
        }
    }
    return NULL;
}

/* 调试用：按注册顺序返回实例标签（非硬件编号），不依赖任何具体句柄 */
const char* uart_drv_get_name(UART_HandleTypeDef *huart) {
    for (int i = 0; i < s_drv_count; i++) {
        if (s_drv_table[i] != NULL && s_drv_table[i]->huart == huart) {
            static char name[8];
            name[0] = 'U'; name[1] = 'A'; name[2] = 'R'; name[3] = 'T';
            name[4] = (char)('0' + (i + 1));   /* 实例序号，单数字足够 */
            name[5] = '\0';
            return name;
        }
    }
    return "UNKNOWN";
}

uint8_t uart_drv_get_index(UART_HandleTypeDef *huart) {
    for (int i = 0; i < s_drv_count; i++) {
        if (s_drv_table[i] != NULL && s_drv_table[i]->huart == huart) {
            return (uint8_t)(i + 1);
        }
    }
    return 0;
}

// ===========================
// RS485
// ===========================
static void rs485_tx_enable(uart_drv_t *drv) {
    if (drv->rs485 == NULL) return;
    HAL_GPIO_WritePin(drv->rs485->port, drv->rs485->pin,
                      drv->rs485->active_level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void rs485_rx_enable(uart_drv_t *drv) {
    if (drv->rs485 == NULL) return;
    // while (!__HAL_UART_GET_FLAG(drv->huart, UART_FLAG_TC)) {};
    HAL_GPIO_WritePin(drv->rs485->port, drv->rs485->pin,
                      drv->rs485->active_level ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

// ===========================
// 启动接收
// ===========================
static void start_rx(uart_drv_t *drv) {
    if (drv->state == UART_DRV_ERROR) return;
    
    // ★ 清除所有标志 ★
    HAL_UART_AbortReceive(drv->huart);
    __HAL_UART_CLEAR_FLAG(drv->huart, UART_FLAG_ORE | UART_FLAG_IDLE);
    
    drv->rx_len = 0;
    drv->state = UART_DRV_IDLE;
    
    if (drv->huart->hdmarx != NULL) {
        HAL_UARTEx_ReceiveToIdle_DMA(drv->huart, drv->rx_buf, UART_DRV_BUF_SIZE);
    } else {
        // HAL_UART_Receive_IT(drv->huart, drv->rx_buf, UART_DRV_BUF_SIZE);
    }    
}

// ===========================
// API实现
// ===========================

void uart_drv_init(uart_drv_t *drv, UART_HandleTypeDef *huart, uart_rs485_t *rs485) {
    memset(drv, 0, sizeof(uart_drv_t));
    drv->huart = huart;
    drv->rs485 = rs485;
    drv->state = UART_DRV_IDLE;
    drv->locked = 0;
    
    drv->cfg.baudrate = huart->Init.BaudRate;
    drv->cfg.word_length = huart->Init.WordLength;
    drv->cfg.stop_bits = huart->Init.StopBits;
    drv->cfg.parity = huart->Init.Parity;
    
    uart_drv_register_instance(drv);
    
    rs485_rx_enable(drv);
    start_rx(drv);
}

int uart_drv_reconfig(uart_drv_t *drv, const uart_drv_cfg_t *cfg) {
    if (drv == NULL || cfg == NULL) return -1;
    
    UART_HandleTypeDef *huart = drv->huart;
    
    HAL_UART_DMAStop(huart);
    HAL_UART_Abort(huart);
    HAL_UART_DeInit(huart);
    
    huart->Init.BaudRate = cfg->baudrate;
    huart->Init.WordLength = cfg->word_length;
    huart->Init.StopBits = cfg->stop_bits;
    huart->Init.Parity = cfg->parity;
    huart->Init.Mode = UART_MODE_TX_RX;
    huart->Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart->Init.OverSampling = UART_OVERSAMPLING_16;
    
    if (HAL_UART_Init(huart) != HAL_OK) {
        drv->state = UART_DRV_ERROR;
        if (drv->on_error) drv->on_error(drv);
        return -2;
    }
    
    drv->cfg = *cfg;
    drv->state = UART_DRV_IDLE;
    drv->locked = 0;
    drv->rx_len = 0;
    
    rs485_rx_enable(drv);
    start_rx(drv);
    return 0;
}

void uart_drv_reg_cb(uart_drv_t *drv,
                     void (*on_recv)(uart_drv_t *, uint8_t *, uint16_t),
                     void (*on_sent)(uart_drv_t *),
                     void (*on_error)(uart_drv_t *)) {
    if (on_recv)
        drv->on_recv = on_recv;
    if (on_sent)
        drv->on_sent = on_sent;
    if (on_error)
        drv->on_error = on_error;
}

int uart_drv_send(uart_drv_t *drv, const uint8_t *data, uint16_t len) {
    if (drv == NULL || data == NULL || len == 0) return -1;
    if (drv->state == UART_DRV_TX_BUSY) return -2;
    if (drv->state == UART_DRV_DATA_READY && drv->locked) return -3;
    
    HAL_UART_AbortReceive(drv->huart);
    
    memcpy(drv->tx_buf, data, len);
    
    rs485_tx_enable(drv);
    drv->state = UART_DRV_TX_BUSY;
    
    if (drv->huart->hdmatx != NULL) {
        HAL_UART_Transmit_DMA(drv->huart, drv->tx_buf, len);
    } else {
        HAL_UART_Transmit_IT(drv->huart, drv->tx_buf, len);
    }
    return 0;
}

// ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★
// ★ 核心修复：接收接口
// ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★

uint8_t* uart_drv_get_packet(uart_drv_t *drv, uint16_t *len) {
    if (drv == NULL || len == NULL) return NULL;
    
    // ★ 只有 DATA_READY 且未锁定才能获取 ★
    if (drv->state != UART_DRV_DATA_READY) return NULL;
    if (drv->locked) return NULL;
    
    drv->locked = 1;
    *len = drv->rx_len;
    return drv->rx_buf;
}

void uart_drv_release_packet(uart_drv_t *drv) {
    if (drv == NULL) return;
    
    // ★★★ 关键修复：无论 locked 如何，都清理状态 ★★★
    // 但需要防止在中断中误操作
    if (drv->state != UART_DRV_DATA_READY) {
        // 如果不是 DATA_READY，不需要释放
        return;
    }
    
    drv->locked = 0;
    drv->rx_len = 0;
    drv->state = UART_DRV_IDLE;
    
    // 重新启动接收
    start_rx(drv);
}

uint16_t uart_drv_available(uart_drv_t *drv) {
    if (drv == NULL) return 0;
    
    // ★ 只有 DATA_READY 且未锁定，数据才有效 ★
    if (drv->state == UART_DRV_DATA_READY && drv->locked == 0) {
        return drv->rx_len;
    }
    return 0;
}

uart_drv_state_t uart_drv_get_state(uart_drv_t *drv) {
    return drv->state;
}

void uart_drv_reset(uart_drv_t *drv) {
    HAL_UART_DMAStop(drv->huart);
    __HAL_UART_CLEAR_FLAG(drv->huart, UART_FLAG_ORE | UART_FLAG_FE | UART_FLAG_NE);
    
    drv->state = UART_DRV_IDLE;
    drv->locked = 0;
    drv->rx_len = 0;
    
    rs485_rx_enable(drv);
    start_rx(drv);
}

void uart_drv_get_cfg(uart_drv_t *drv, uart_drv_cfg_t *cfg) {
    if (drv && cfg) *cfg = drv->cfg;
}

// ===========================
// 中断入口
// ===========================

void uart_drv_on_tx_done(UART_HandleTypeDef *huart) {
    uart_drv_t *drv = uart_drv_find(huart);
    if (drv == NULL || drv->state != UART_DRV_TX_BUSY) return;
    
    drv->state = UART_DRV_IDLE;
    rs485_rx_enable(drv);
    
    if (drv->on_sent) drv->on_sent(drv);
    
    start_rx(drv);
}

void uart_drv_on_idle(UART_HandleTypeDef *huart) {
    uart_drv_t *drv = uart_drv_find(huart);
    if (drv == NULL) return;
    if (drv->state == UART_DRV_TX_BUSY) return;
    
    // ★ 如果已经有数据未被处理，忽略新的空闲中断 ★
    if (drv->state == UART_DRV_DATA_READY) {
        __HAL_UART_CLEAR_IDLEFLAG(huart);
        return;
    }
    
    // ★ 如果被锁定，说明上层正在处理数据，忽略 ★
    if (drv->locked) {
        __HAL_UART_CLEAR_IDLEFLAG(huart);
        return;
    }

    /* ★★★ 关键修复：HAL 的 ReceiveToIdle_DMA 在 DMA 半传输(HT, 128字节) 和
     *     全传输(TC, 256字节) 时也会回调本函数，而不只是 IDLE（帧结束）。
     *     若把 HT 误判为帧结束，会把 >128 字节的大帧从中间截断 → CRC 错误。
     *     因此只有在真正的 IDLE（或恰好填满缓冲区的 TC）时才提交数据包。★★★ */
    {
        HAL_UART_RxEventTypeTypeDef rx_evt = HAL_UARTEx_GetRxEventType(huart);
        if (rx_evt != HAL_UART_RXEVENT_IDLE && rx_evt != HAL_UART_RXEVENT_TC) {
            /* HT 等中间事件：不要停止 DMA，也不要提交数据包，等真正的帧结束 */
            __HAL_UART_CLEAR_IDLEFLAG(huart);
            return;
        }
    }

    if (drv->huart->hdmarx != NULL) {
        drv->rx_len = UART_DRV_BUF_SIZE - __HAL_DMA_GET_COUNTER(drv->huart->hdmarx);
    }
    
    if (drv->rx_len > 0) {
        HAL_UART_DMAStop(drv->huart);
        drv->state = UART_DRV_DATA_READY;
        // ★ locked 默认为 0，等待上层调用 get_packet ★
        
        if (drv->on_recv) {
            drv->on_recv(drv, drv->rx_buf, drv->rx_len);
        }
    }
    
    __HAL_UART_CLEAR_IDLEFLAG(huart);
}

void uart_drv_on_error(UART_HandleTypeDef *huart) {
    uart_drv_t *drv = uart_drv_find(huart);
    if (drv == NULL) return;
    
    drv->state = UART_DRV_ERROR;
    if (drv->on_error) drv->on_error(drv);
    
    uart_drv_reset(drv);
}