/**
 * @file    ymodem.h
 * @brief   YMODEM 收发引擎（非阻塞状态机，零硬件依赖）
 * @version V1.0
 * @date    2026-09-18
 *
 * 为什么不直接搬参考工程：`refe/Ymodem-master`（= STM32F0xx_IAP）是**阻塞**的 ——
 * 它在一个函数里死等包头、死等 ACK。BL 里勉强能用，装进带协议栈的 APP 就不行：
 * 一次传输几秒到几十秒，看门狗会先咬人，别的任务也全停了。
 * 这里改成**非阻塞状态机**：调用方反复调 `ymodem_recv_process()`，每次只做
 * 「把已经到达的字节吃掉」这一件事。
 *
 * **零硬件依赖**：收字节 / 发字节 / 时基全部由调用方注入（`ymodem_io_t`），
 * 所以本模块可以在 PC 上跑：
 *   - 单测里把两个引擎对接（发送 → 内存通道 → 接收），端到端验数据一致性
 *   - 也能造出「ACK 丢一个」「包重发一次」「CRC 坏一包」这些真机上很难复现的场景
 *
 * 协议要点（易错处都标出来了）：
 *   · 包 = `SOH|STX` + `seq` + `~seq` + data + `crc16(2)`（CRC16-XMODEM，poly 0x1021 初值 0）
 *   · **SOH 恒配 128 字节数据，STX 恒配 1024**。拿 SOH 发 1024 字节是最经典的上手错误：
 *     接收端按 128 解析，整包数据错位，而且 CRC 还「能过」（它校验的是被截断的 128 字节）。
 *   · 接收方先发 `'C'`(0x43) 表示「用 CRC16、请开始」；序号 0 是头包（文件名 + 大小）
 *   · **重复包（seq 与上一包相同）必须重新 ACK，且不重复写数据** ——
 *     ACK 丢失时发送方会重发同一包，这是协议正常行为，不是错误
 *   · EOT → ACK → 对端再发一个**空第 0 包** → ACK，一个文件的传输才算结束
 *   · 超时要**重发上一次的响应**（重发 'C' / ACK / NAK），而不是无脑发 NAK
 *   · 重试超限 → 连发 `CAN CAN` 中止，两侧都要能从中恢复
 */

#ifndef __YMODEM_H
#define __YMODEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 协议常量 ---------------- */

#define YMODEM_SOH        0x01u   /* 128 字节数据包的起始字节 */
#define YMODEM_STX        0x02u   /* 1024 字节数据包的起始字节 */
#define YMODEM_EOT        0x04u   /* 传输结束 */
#define YMODEM_ACK        0x06u
#define YMODEM_NAK        0x15u
#define YMODEM_CAN        0x18u   /* 连发两个 = 中止 */
#define YMODEM_C          0x43u   /* 'C'：请求用 CRC16 开始 */
#define YMODEM_ABORT1     0x41u   /* 'A'：用户中止 */
#define YMODEM_ABORT2     0x61u   /* 'a'：用户中止 */

#define YMODEM_DATA_128   128u
#define YMODEM_DATA_1K    1024u
#define YMODEM_PKT_MAX    YMODEM_DATA_1K
#define YMODEM_OVERHEAD   5u      /* 包类型(1) + 序号(1) + 序号反码(1) + CRC16(2) */
#define YMODEM_FRAME_MAX  (YMODEM_OVERHEAD + YMODEM_PKT_MAX)

#define YMODEM_NAME_MAX   64u     /* 协议上限是 255；我们只存关心的那一段 */
#define YMODEM_PAD_BYTE   0x1Au   /* 包尾填充（DOS EOF），发送侧用 */

#define YMODEM_DEF_RETRY      10u
#define YMODEM_DEF_TIMEOUT_MS 1000u

/* ---------------- 返回码 ---------------- */

#define YMODEM_BUSY          (1)   /* 还没结束，继续调 process */

typedef enum {
    YMODEM_OK           =  0,
    YMODEM_ERR_PARAM    = -1,
    YMODEM_ERR_IO       = -2,   /* 注入的 write 失败 */
    YMODEM_ERR_TIMEOUT  = -3,   /* 重试次数用尽 */
    YMODEM_ERR_PROTO    = -4,   /* 包结构 / CRC / 序号不可恢复 */
    YMODEM_ERR_ABORT    = -5,   /* 对端发 CAN 或 'A'/'a' */
    YMODEM_ERR_USER     = -6,   /* on_header / on_data / on_pull 回调请求中止 */
    YMODEM_ERR_NOSPACE  = -7,   /* 上层拒绝（如目标区放不下） */
} ymodem_ret_t;

/** @brief 事件（on_event 回调） */
typedef enum {
    YMODEM_EV_START = 0,   /*!< 开始握手 */
    YMODEM_EV_HEADER,      /*!< arg = 头包声明的文件大小 */
    YMODEM_EV_PACKET,      /*!< arg = 本包有效载荷字节数 */
    YMODEM_EV_RETRY,       /*!< arg = 本次重试计数 */
    YMODEM_EV_DUP,         /*!< arg = 被去重掉的重复包序号 */
    YMODEM_EV_DONE,        /*!< arg = 总字节数 */
} ymodem_event_t;

/** @brief 注入的通道（三个回调都必须给） */
typedef struct {
    /** @brief 读一个字节：1 = 读到，0 = 暂无（非阻塞） */
    int      (*read_byte)(void *ctx, uint8_t *b);
    /** @brief 写一段字节：返回 0 = 成功 */
    int      (*write)(void *ctx, const uint8_t *buf, uint32_t len);
    /** @brief 毫秒时基（允许回绕，内部按无符号差值比较） */
    uint32_t (*tick)(void *ctx);
    void     *ctx;
} ymodem_io_t;

/** @brief 头包解析结果 */
typedef struct {
    char     name[YMODEM_NAME_MAX];   /*!< 文件名（超长截断） */
    uint32_t size;                    /*!< 文件字节数；0 = 头包没写大小 */
} ymodem_file_t;

/* ============================================================
 *  接收侧
 * ============================================================ */

typedef enum {
    YMODEM_RECV_IDLE = 0,
    YMODEM_RECV_HANDSHAKE,   /*!< 反复发 'C' 等首包 */
    YMODEM_RECV_HEADER,      /*!< 头包已收到，等数据（此处与 DATA 同义，保留以便日志） */
    YMODEM_RECV_DATA,        /*!< 正在收数据包 */
    YMODEM_RECV_FINAL,       /*!< 已 ACK EOT，等结束用的空第 0 包 */
    YMODEM_RECV_DONE,
    YMODEM_RECV_FAILED,
} ymodem_recv_state_t;

typedef struct {
    ymodem_recv_state_t st;
    ymodem_ret_t        err;

    const ymodem_io_t  *io;
    void               *user;

    /**
     * @brief 头包回调：**在回 ACK 之前**调用，返回非 0 表示拒收（发 CAN 中止）。
     * @note  必须在 ACK 前判「目标区放不下」这类问题 —— 一旦 ACK 了，对端就开始
     *        灌数据，那时再拒绝只能靠 CAN，场面很难看。
     */
    int  (*on_header)(void *user, const ymodem_file_t *f);
    /** @brief 数据回调：返回非 0 表示中止（发 CAN） */
    int  (*on_data)(void *user, const uint8_t *data, uint32_t len);
    /** @brief 事件回调（可 NULL） */
    void (*on_event)(void *user, ymodem_event_t ev, uint32_t arg);

    uint8_t  retry_max;
    uint16_t timeout_ms;

    /* ---- 私有：收包 ---- */
    uint8_t  frame[YMODEM_FRAME_MAX];
    uint32_t fill;          /*!< 已攒字节数 */
    uint32_t expect;        /*!< 本帧总长；0 = 还没拿到包类型 */
    uint8_t  last_seq;
    uint8_t  has_last;
    uint8_t  last_resp;     /*!< 上一次发出的响应字节，超时重发它 */
    uint8_t  can_cnt;
    uint16_t retry;
    uint32_t t_ref;
    uint8_t  has_header;

    /* ---- 结果 ---- */
    ymodem_file_t file;
    uint32_t got;           /*!< 累计收到的有效字节 */
    uint32_t pkts;          /*!< 收到的数据包数 */
    uint32_t dup;           /*!< 被去重掉的重复包数 */
} ymodem_recv_t;

/**
 * @brief  启动接收
 * @param  y         上下文（调用方持有，**勿放栈上**：含 1 KB 级帧缓冲）
 * @param  io        注入通道
 * @param  user      传给三个回调的上下文
 * @return YMODEM_OK / YMODEM_ERR_PARAM
 * @note   本函数会立刻发第一个 'C'，所以通道必须先就绪。
 */
int ymodem_recv_start(ymodem_recv_t *y, const ymodem_io_t *io, void *user,
                      int (*on_header)(void *user, const ymodem_file_t *f),
                      int (*on_data)(void *user, const uint8_t *data, uint32_t len),
                      void (*on_event)(void *user, ymodem_event_t ev, uint32_t arg));

/** @brief 调整重试上限与超时（不调则用默认 10 次 / 1000 ms） */
void ymodem_recv_set_opts(ymodem_recv_t *y, uint8_t retry_max, uint16_t timeout_ms);

/**
 * @brief  推进一步
 * @return YMODEM_BUSY / YMODEM_OK / 错误码（<0）
 * @note   每次调用把「此刻已到达」的字节全部吃掉，不会等。所以调用频率决定吞吐上限，
 *         建议放在 1 ms 级的轮询里。
 */
int ymodem_recv_process(ymodem_recv_t *y);

/** @brief 主动中止（发送 CAN CAN 并置 FAILED） */
void ymodem_recv_abort(ymodem_recv_t *y);

/** @brief 状态名（日志用） */
const char *ymodem_recv_state_name(ymodem_recv_state_t s);

/* ============================================================
 *  发送侧
 * ============================================================ */

typedef enum {
    YMODEM_SEND_IDLE = 0,
    YMODEM_SEND_WAIT_C,      /*!< 等接收方的 'C' */
    YMODEM_SEND_HEADER,      /*!< 头包已发，等 ACK */
    YMODEM_SEND_DATA,        /*!< 数据包已发，等 ACK */
    YMODEM_SEND_EOT,         /*!< EOT 已发，等 ACK */
    YMODEM_SEND_FINAL,       /*!< 结束用的空第 0 包已发，等 ACK */
    YMODEM_SEND_DONE,
    YMODEM_SEND_FAILED,
} ymodem_send_state_t;

typedef struct {
    ymodem_send_state_t st;
    ymodem_ret_t        err;

    const ymodem_io_t  *io;
    void               *user;

    /**
     * @brief 取数据：填最多 len 字节，*got 为实际字节数（0 = 没有更多数据）
     * @return 非 0 表示中止
     */
    int  (*on_pull)(void *user, uint8_t *buf, uint32_t len, uint32_t *got);
    void (*on_event)(void *user, ymodem_event_t ev, uint32_t arg);

    const char *file_name;   /*!< 头包里的文件名，必填 */
    uint32_t    file_size;   /*!< 声明的大小；0 = 不写（对端只能按流处理） */
    uint8_t     use_1k;      /*!< 1 = 用 STX/1024，0 = 用 SOH/128 */
    uint8_t     retry_max;
    uint16_t    timeout_ms;

    /* ---- 私有 ---- */
    uint8_t  frame[YMODEM_FRAME_MAX];
    uint32_t frame_len;
    uint8_t  seq;
    uint32_t sent;
    uint8_t  last_pkt_eot;   /*!< 上一包已把数据取完 → 下一状态是 EOT */
    uint16_t retry;
    uint32_t t_ref;
    uint8_t  can_cnt;
} ymodem_send_t;

/**
 * @brief  启动发送
 * @param  y           上下文（**勿放栈上**）
 * @param  file_name   文件名（头包用）
 * @param  file_size   声明大小；0 = 未知
 * @note   本函数不立刻发东西：先等对端发 'C'（YMODEM 规定由接收方起头）。
 */
int ymodem_send_start(ymodem_send_t *y, const ymodem_io_t *io, void *user,
                      const char *file_name, uint32_t file_size,
                      int (*on_pull)(void *user, uint8_t *buf, uint32_t len, uint32_t *got),
                      void (*on_event)(void *user, ymodem_event_t ev, uint32_t arg));

/** @brief 调参（不调则用默认 10 次 / 1000 ms；use_1k 默认 1） */
void ymodem_send_set_opts(ymodem_send_t *y, uint8_t use_1k, uint8_t retry_max, uint16_t timeout_ms);

/** @brief 推进一步：YMODEM_BUSY / YMODEM_OK / 错误码（<0） */
int ymodem_send_process(ymodem_send_t *y);

/** @brief 主动中止（发 CAN CAN） */
void ymodem_send_abort(ymodem_send_t *y);

const char *ymodem_send_state_name(ymodem_send_state_t s);

/* ---------------- 工具（也供上层自测用） ---------------- */

/** @brief CRC16-XMODEM（poly 0x1021，初值 0，与 lrzsz / sb 一致） */
uint16_t ymodem_crc16(const uint8_t *data, uint32_t len);

/** @brief 按协议组一帧（seq/data；data_len 决定 SOH 还是 STX）。返回帧长 */
uint32_t ymodem_build_frame(uint8_t *out, uint8_t seq, const uint8_t *data, uint32_t data_len);

/**
 * @brief 校验并拆一帧
 * @param  frame,frame_len  完整帧（含头尾）
 * @param  seq_out          输出序号
 * @param  data_out         输出数据指针（指向 frame 内部）
 * @param  data_len_out     输出数据长度（128 或 1024）
 * @return YMODEM_OK / YMODEM_ERR_PROTO
 */
int ymodem_check_frame(const uint8_t *frame, uint32_t frame_len,
                       uint8_t *seq_out, const uint8_t **data_out, uint32_t *data_len_out);

#ifdef __cplusplus
}
#endif

#endif /* __YMODEM_H */
