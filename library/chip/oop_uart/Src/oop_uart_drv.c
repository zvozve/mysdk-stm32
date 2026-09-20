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

// ===========================
// TX 拥有者映射（解决"同 huart 多 uart_drv_t 实例"的发送完成回调歧义）
//   一个物理 UART 上可能同时注册多个 uart_drv_t（例如同一 RS485 总线的
//   主机实例 + 从机实例）。HAL 的 DMA 发送完成回调只携带 huart，
//   若用 uart_drv_find(huart) 返回"第一个匹配实例"，会误清其它实例的
//   TX_BUSY，导致其中一方发送永远返回 -2。
//   因此：uart_drv_send 启动 DMA 前把"当前 TX 拥有者"记入下表；
//         uart_drv_on_tx_done 直接取出该拥有者，避免歧义。
// ===========================
#define UART_DRV_MAX_HUARTS   8
static struct {
    UART_HandleTypeDef *huart;
    uart_drv_t *owner;   /* 当前正在该 huart 上发送的实例，NULL=空闲 */
} s_tx_owner[UART_DRV_MAX_HUARTS];

/* 发送启动时登记当前 TX 拥有者（同一 huart 同一时刻仅一个） */
static void uart_drv_set_tx_owner(UART_HandleTypeDef *huart, uart_drv_t *drv) {
    for (int i = 0; i < UART_DRV_MAX_HUARTS; i++) {
        if (s_tx_owner[i].huart == huart) { s_tx_owner[i].owner = drv; return; }
    }
    for (int i = 0; i < UART_DRV_MAX_HUARTS; i++) {
        if (s_tx_owner[i].huart == NULL) {
            s_tx_owner[i].huart = huart;
            s_tx_owner[i].owner = drv;
            return;
        }
    }
}

/* 发送完成（或中止）时取回并清空 TX 拥有者 */
static uart_drv_t* uart_drv_take_tx_owner(UART_HandleTypeDef *huart) {
    for (int i = 0; i < UART_DRV_MAX_HUARTS; i++) {
        if (s_tx_owner[i].huart == huart) {
            uart_drv_t *owner = s_tx_owner[i].owner;
            s_tx_owner[i].owner = NULL;
            return owner;
        }
    }
    return NULL;
}

/* 仅清空（中止/复位中途发送时调用，避免残留脏拥有者） */
static void uart_drv_clear_tx_owner(UART_HandleTypeDef *huart) {
    for (int i = 0; i < UART_DRV_MAX_HUARTS; i++) {
        if (s_tx_owner[i].huart == huart) { s_tx_owner[i].owner = NULL; return; }
    }
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
// 7 位数据软件模拟（F4 硬件无 7 位字长，用 8N1 + 软件校验实现，支持 7E1 / 7O1）
//   线格式：1 起始 + 7 数据位 + 1 校验位(偶/奇) + 1 停止 = 10 位，与 8N1 等长（不会报 Frame error）。
//   收到字节 v = data_7 | (parity<<7)；发出字节 = data_7 | (parity_bit(data_7)<<7)。
// ===========================
static uint8_t uart_7bit_parity_bit(uint8_t d, uint8_t odd) {
    uint8_t c = 0;
    for (uint8_t m = 1; m; m <<= 1) if (d & m) c++;
    uint8_t even_bit = (c & 1U) ? 1U : 0U;   // 偶校验位：使 (数据1的个数 + 校验位) 为偶数
    return odd ? (uint8_t)(even_bit ^ 1U) : even_bit;  // 奇校验 = 偶校验位取反
}

// RX：把 8N1 收下的字节还原为 7 位数据；校验成立当且仅当 popcount(v) 符合指定校验
static void uart_7bit_decode(uint8_t *buf, uint16_t len, uint8_t odd, uint8_t *parity_err) {
    uint8_t pe = 0;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t v = buf[i];
        uint8_t ones = 0;
        for (uint8_t m = 1; m; m <<= 1) if (v & m) ones++;
        uint8_t want_even = (ones % 2 == 0);
        if (odd ? want_even : !want_even) pe = 1;   // 奇校验要求 popcount 为奇；偶校验要求为偶
        buf[i] = v & 0x7FU;                         // 还原 7 位数据（bit7 为校验位，丢弃）
    }
    if (parity_err) *parity_err = pe;
}

// TX：把 7 位数据字节拼成线格式（仅低 7 位有效）
static void uart_7bit_encode(uint8_t *buf, uint16_t len, uint8_t odd) {
    for (uint16_t i = 0; i < len; i++) {
        uint8_t d = buf[i] & 0x7FU;
        buf[i] = d | (uart_7bit_parity_bit(d, odd) << 7);
    }
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
        HAL_StatusTypeDef rc = HAL_UARTEx_ReceiveToIdle_DMA(drv->huart, drv->rx_buf, UART_DRV_BUF_SIZE);
        if (rc != HAL_OK) {
            /* ★ 静默死锁陷阱（2026-09-18）：HAL_UARTEx_ReceiveToIdle_DMA 只在
             *   ReceptionType 最终为 TOIDLE 时才打开 USART_CR1_IDLEIE；一旦这里返回
             *   非 HAL_OK（最常见是 huart->RxState 不是 READY），接收等于没武装——
             *   现象是「一个字节都收不到，且全流程无任何报错」。必须报出来。★ */
            UART_LOG("%s RX arm FAILED rc=%d (RxState=0x%X) -> will receive NOTHING",
                     uart_drv_get_name(drv->huart), (int)rc, (unsigned)drv->huart->RxState);
        }
    } else {
        UART_LOG("%s no DMA (hdmarx=NULL) -> RX not started (IT branch disabled)",
                 uart_drv_get_name(drv->huart));
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
    drv->emulate_7bit = 0;
    drv->emulate_7bit_parity = 0;

    drv->cfg.baudrate = huart->Init.BaudRate;
    drv->cfg.word_length = huart->Init.WordLength;
    drv->cfg.stop_bits = huart->Init.StopBits;
    drv->cfg.parity = huart->Init.Parity;
    
    uart_drv_register_instance(drv);
    
    rs485_rx_enable(drv);
    start_rx(drv);

    /* 一行讲清「这一路到底武装好了没」：baud 来自 CubeMX 生成代码（不是 board_cfg），
       RxState=BUSY_RX 才说明 ReceiveToIdle_DMA 真的在跑（否则收不到任何数据）。 */
    UART_LOG("%s init: baud=%lu, dmarx=%s, rx_armed=%s, buf=%u",
             uart_drv_get_name(huart), (unsigned long)huart->Init.BaudRate,
             (huart->hdmarx != NULL) ? "yes" : "no",
             (huart->RxState == HAL_UART_STATE_BUSY_RX) ? "yes" : "no",
             (unsigned)UART_DRV_BUF_SIZE);
}

int uart_drv_reconfig(uart_drv_t *drv, const uart_drv_cfg_t *cfg) {
    if (drv == NULL || cfg == NULL) return -1;
    
    UART_HandleTypeDef *huart = drv->huart;
    
    HAL_UART_DMAStop(huart);
    uart_drv_clear_tx_owner(huart);   // ★ 中止中途发送，撤销 TX 拥有者登记 ★
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
    
    // ★ 关键修复：原代码用 HAL_UART_AbortReceive（异步，会把 gState 置为 BUSY_ABORT），
    //   紧接着的 HAL_UART_Transmit_DMA/IT 因 gState!=READY 返回 HAL_BUSY，
    //   发送 DMA 从未启动 → TxCplt 永不触发 → drv->state 永久卡在 TX_BUSY → 后续全部 -2。
    //   改用同步的 HAL_UART_DMAStop，立即把 gState 拉回 READY，发送才能正常启动。★
    HAL_UART_DMAStop(drv->huart);

    memcpy(drv->tx_buf, data, len);

    /* ★ 7 位数据软件模拟（发送侧）：把低 7 位 + 软件校验位拼成 8 位线格式再发，
     *   与 RX 的 uart_7bit_decode 对称。缺此步时 emulate_7bit 只作用于接收，
     *   发送端不带校验位 → 对端按 7E1/7O1 采样会判校验错误，7E1 透传单向失效。★ */
    if (drv->emulate_7bit) {
        uart_7bit_encode(drv->tx_buf, len, drv->emulate_7bit_parity);
    }

    rs485_tx_enable(drv);
    drv->state = UART_DRV_TX_BUSY;
    uart_drv_set_tx_owner(drv->huart, drv);   // ★ 登记 TX 拥有者，供完成回调精确路由 ★

    HAL_StatusTypeDef st;
    if (drv->huart->hdmatx != NULL) {
        st = HAL_UART_Transmit_DMA(drv->huart, drv->tx_buf, len);
    } else {
        st = HAL_UART_Transmit_IT(drv->huart, drv->tx_buf, len);
    }

    /* ★ 若启动失败（极端情况下 gState 仍 BUSY），回滚状态并回报错误，
     *   避免 drv->state 永久卡在 TX_BUSY 导致后续所有发送返回 -2 ★ */
    if (st != HAL_OK) {
        SYS_LOG("[UART] TX start FAILED hal=%d", (int)st);
        uart_drv_clear_tx_owner(drv->huart);   // ★ 启动失败，撤销拥有者登记 ★
        rs485_rx_enable(drv);
        drv->state = UART_DRV_IDLE;
        start_rx(drv);
        return -4;
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
    uart_drv_clear_tx_owner(drv->huart);   // ★ 复位中途发送，撤销 TX 拥有者登记 ★
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
    // ★ 直接取当前 TX 拥有者，避免同 huart 多实例的完成回调歧义 ★
    uart_drv_t *drv = uart_drv_take_tx_owner(huart);
    if (drv == NULL) {
        SYS_LOG("[UART] tx_done NO-OWNER (huart=%p)", (void*)huart);
        return;
    }
    if (drv->state != UART_DRV_TX_BUSY) {
        SYS_LOG("[UART] tx_done IGNORED (state=%d, not TX_BUSY)", (int)drv->state);
        return;
    }

    drv->state = UART_DRV_IDLE;
    rs485_rx_enable(drv);

    if (drv->on_sent) drv->on_sent(drv);

    start_rx(drv);
    SYS_LOG("[UART] tx_done OK (state->IDLE)");
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
        /* 收到多少字节才算「物理链路通了」的唯一硬证据（DMA+IDLE 已提交一帧） */
        UART_LOG("%s frame %u B", uart_drv_get_name(huart), (unsigned)drv->rx_len);

        // 7 位数据模拟：先把 8N1 收下的字节还原为 7 位数据并软件校验
        if (drv->emulate_7bit) {
            uint8_t pe = 0;
            uart_7bit_decode(drv->rx_buf, drv->rx_len, drv->emulate_7bit_parity, &pe);
            if (pe) {
                RTT_LOG("[UART] 7-bit(%s) parity error, drop frame",
                        drv->emulate_7bit_parity ? "ODD" : "EVEN");
                HAL_UART_DMAStop(drv->huart);
                __HAL_UART_CLEAR_IDLEFLAG(huart);
                start_rx(drv);
                return;
            }
        }
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

    /* 原来这里完全静默：线缆噪声/波特率不匹配/收发器悬空造成的 ORE/FE/NE 都看不到。
       PE=校验错 FE=帧错(停止位/波特率不对) NE=噪声 ORE=溢出(来不及取走) */
    UART_LOG("%s RX ERROR code=0x%X (PE=%d FE=%d NE=%d ORE=%d) -> reset",
             uart_drv_get_name(huart), (unsigned)huart->ErrorCode,
             (huart->ErrorCode & HAL_UART_ERROR_PE)  ? 1 : 0,
             (huart->ErrorCode & HAL_UART_ERROR_FE)  ? 1 : 0,
             (huart->ErrorCode & HAL_UART_ERROR_NE)  ? 1 : 0,
             (huart->ErrorCode & HAL_UART_ERROR_ORE) ? 1 : 0);

    drv->state = UART_DRV_ERROR;
    if (drv->on_error) drv->on_error(drv);
    
    uart_drv_reset(drv);
}