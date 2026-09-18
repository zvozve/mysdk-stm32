# protocols/cli — 串口命令行核心

把「命令表 + 行解析 + 防回显自激」从各工程的 `uart_cli.c` 里抽出来的通用模块。
本模块**不认识任何业务，也不认识 UART/HAL**；工程只提供两样东西：

1. 传输怎么读、怎么写 —— 注入 `cli_transport_t`
2. 每条命令干什么 —— 注册 `cli_cmd_t[]` 静态命令表

业务模块自带 `xxx_cli_cmds[]`，app 侧只做聚合注册，不必改本模块。

```
app / 各模块
  │  ① 注册命令表（可多张，累加）        cli_register(&cli, xxx_cli_cmds, N)
  │  ② 周期轮询                        cli_process(&cli)  或  cli_feed(&cli, pkt, len)
  ▼
protocols/cli（本模块）  ← 零业务、零 HAL
  │  transport.getc / .write / .flush
  ▼
USART 寄存器直读 / chip.oop_uart 整包 / RTT / USB CDC / 上位机管道 …
```

依赖：仅 `chip.oop_dwt`（`oop_GetTickMS()`，做去抖窗口与限速上报的时基）。

---

## 为什么值得单独成模块

不是「为了复用」这么笼统。真正的原因是下面这四条闸门，是在真实链路上一路啃出来的答案，
不该让每个工程重新踩一遍：

| 闸门 | 做法 | 不加会怎样 |
|---|---|---|
| 1 一次一条 | 一次 `cli_process()` 最多执行一条命令，执行完立即返回 | 队列里积压的多条命令被一口气执行完，表现为「连发」 |
| 2 执行后排空 | 执行完调 `transport.flush()`，丢掉发送+打印期间涌入的字节 | 自己刚打出去的日志被回显回来，当成新命令解析 |
| 3 同名去抖 | 每条命令可设 `guard_ms`，窗口内重复触发一律丢弃 | 上位机回显 / 按键抖动导致同一条命令连发 |
| 4 未知静默 | 不认识的行**绝不回打印**，只做限速摘要 | **自激放大**：报错被回显→再解析→再报错→一直刷屏 |

第 4 条是关键。只要链路上存在回显（终端本地回显、TX-RX 走线串扰、发码时 38 kHz
载波耦合进接收线），"未知命令" 的报错就会自己喂自己。被丢弃的行改为**限速摘要**
（默认 3000 ms 一条，`cli_set_drop_report_ms()` 可调），既能定位原因又不会自激。

顺带把两处旧实现的粗糙处一并修正了（有意为之，不是兼容性问题）：

- **参数用空白分隔**（`txcar 38000`），不再把整行空白删光后粘连（旧实现是 `TXCAR38000`，
  而且 `T X` 会被规范化成 `TX` 命中命令——那是剥离空白的副产物，不是设计）。
- **只有命令名大小写不敏感**，参数原样保留。旧实现把整行转大写，将来加 `wifi <SSID>`
  这类命令会把大小写敏感的参数改坏。
- **数字解析带溢出检查**：`42949672960` 这类超范围输入返回参数错误，而不是静默回绕成
  一个「看起来合法」的值。

---

## API 速览

```c
/* --- 生命周期 --- */
void cli_init(cli_t *cli, const cli_transport_t *transport);
int  cli_register(cli_t *cli, const cli_cmd_t *cmds, uint16_t n);   /* 可多次，累加 */

/* --- 输入 --- */
void     cli_process(cli_t *cli);                       /* 从 transport.getc 逐字节拉 */
uint16_t cli_feed(cli_t *cli, const uint8_t *b, uint16_t len);  /* 整包推入 */
void     cli_reset_input(cli_t *cli);                   /* 丢弃未完成的行 */

/* --- 输出 --- */
void cli_puts(cli_t *cli, const char *s);
void cli_printf(cli_t *cli, const char *fmt, ...);
void cli_print_usage(cli_t *cli, const cli_cmd_t *cmd);
void cli_print_help(cli_t *cli);                        /* 可注册成一条 help 命令 */

/* --- 参数解析 --- */
bool cli_parse_u32(const char *s, uint32_t *out);
bool cli_parse_u32_range(const char *s, uint32_t min, uint32_t max, uint32_t *out);
```

命令表：

```c
typedef cli_ret_t (*cli_handler_t)(cli_t *cli, int argc, char **argv);   /* argv[0] = 命令名 */

typedef struct {
    const char  *name;       /* 大小写不敏感 */
    const char  *usage;      /* "<0|1>"；无参数填 NULL */
    const char  *help;       /* 一行说明，可 NULL */
    cli_handler_t handler;
    uint16_t     guard_ms;   /* 同名命令最小间隔；0 = 不限制 */
} cli_cmd_t;
```

handler 返回 `CLI_RET_BAD_ARG` 时，核心会自动打印 `[CLI] usage: <name> <usage>`——
参数报错文案不必在每个 handler 里重复写。

传输接口：

```c
typedef struct {
    int  (*getc)(void *ctx);                              /* 非阻塞；无数据返回 -1 */
    void (*write)(void *ctx, const char *s, uint16_t n);   /* 输出，发完再返回 */
    void (*flush)(void *ctx);                              /* 排空接收端；可 NULL */
    void *ctx;                                             /* 原样回传，用来带句柄 */
} cli_transport_t;
```

编译期容量（可在工程编译选项里覆盖）：
`CLI_LINE_MAX` 64 / `CLI_MAX_ARGS` 6 / `CLI_MAX_CMDS` 16 / `CLI_MAX_TABLES` 4 /
`CLI_OUT_MAX` 128 / `CLI_DROP_REPORT_MS` 3000。
`cli_register()` 超限时返回 `-1` 而**不静默截断**——静默丢命令会变成「命令注册了但执行
不了」，比编译期就报错难查得多。

---

## 用法示例

### 1. 传输：直接轮询 USART 寄存器（最轻量，零中断零 DMA）

传输实现属于工程侧（只有工程侧能碰 HAL/寄存器），本模块只消费它：

```c
static int cli_uart_getc(void *ctx)
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)ctx;
    USART_TypeDef *u = h->Instance;
    if ((u->SR & USART_SR_RXNE) != 0U) {
        return (int)(u->DR & 0xFFU);
    }
    return -1;
}

static void cli_uart_write(void *ctx, const char *s, uint16_t n)
{
    (void)HAL_UART_Transmit((UART_HandleTypeDef *)ctx, (uint8_t *)s, n, HAL_MAX_DELAY);
}

static void cli_uart_flush(void *ctx)   /* 闸门 2 的落点 */
{
    USART_TypeDef *u = ((UART_HandleTypeDef *)ctx)->Instance;
    for (uint32_t i = 0U; i < 128U; i++) {
        uint32_t sr = u->SR;                       /* F1：读 SR 再读 DR 即清 ORE/NE/FE/PE */
        if ((sr & (USART_SR_RXNE | USART_SR_ORE)) == 0U) {
            break;
        }
        (void)u->DR;
    }
}
```

### 2. 命令表（每个模块自带一张）

```c
static cli_ret_t cmd_status(cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    cli_printf(cli, "[IR] tx_cnt=%lu mode=%u\r\n", (unsigned long)s_tx_cnt, s_mode);
    return CLI_RET_OK;
}

static cli_ret_t cmd_txcar(cli_t *cli, int argc, char **argv)
{
    uint32_t hz;
    if ((argc != 2) || !cli_parse_u32_range(argv[1], 1000U, 60000U, &hz)) {
        return CLI_RET_BAD_ARG;                    /* 核心代打 usage */
    }
    ir_transmitter_set_carrier(&g_tx, hz);
    cli_printf(cli, "[IR] carrier=%luHz\r\n", (unsigned long)hz);
    return CLI_RET_OK;
}

static const cli_cmd_t ir_cli_cmds[] = {
    { "status", NULL,             "dump transmitter state", cmd_status, 0U   },
    { "txcar",  "<1000..60000>",  "set carrier Hz",         cmd_txcar,  0U   },
    { "tx",     NULL,             "replay last frame",      cmd_tx,     500U }, /* 500ms 去抖 */
};
```

### 3. 装配与轮询

```c
static cli_t s_cli;

void uart_cli_init(void)
{
    static cli_transport_t t;                      /* 或直接给静态初值 */
    t.getc  = cli_uart_getc;
    t.write = cli_uart_write;
    t.flush = cli_uart_flush;
    t.ctx   = BOARD_LOG_UART;

    cli_init(&s_cli, &t);
    (void)cli_register(&s_cli, ir_cli_cmds, sizeof(ir_cli_cmds) / sizeof(ir_cli_cmds[0]));
}

void uart_cli_poll(void) { cli_process(&s_cli); }  /* 主循环里周期调用 */
```

---

## 与 chip.oop_uart 集成（整包模型）

`oop_uart` 是 DMA + 空闲中断的**整包**模型，一次拿到一整包，不必逐字节拉：

```c
static void on_recv(uart_drv_t *drv, uint8_t *data, uint16_t len)
{
    (void)cli_feed(&s_cli, data, len);              /* 也可以在主循环里取包再喂 */
}
```

要点与限制：

- **一个 `uart_drv_t` 只能有一个消费者**。若该串口已被 HLK-RM58S 之类的设备独占，
  CLI 要么换串口，要么与该设备合并进同一个分发任务（按帧特征分流）。
- 整包模型下没有「排空接收端」这个动作（字节已被 DMA 收走），`flush` 可填 `NULL`；
  闸门 2 退化为「靠闸门 3 去抖 + 闸门 1 一次一条」兜住回显。
- 若上位机在一包里塞了多条命令，`cli_feed()` 执行完第一条就返回，剩余字节由调用方
  处置（返回值即实际消费字节数）；按本模块的哲学，剩余部分当回显丢弃更安全。

---

## 硬约束

- **handler 不得阻塞**。它运行在调用 `cli_process()` 的上下文（通常是主循环）。
  需要长动作时，handler 只置标志或发请求，由对应任务去执行。
- `cli_transport_t` 内部按值拷贝，但 `ctx` 指向的对象需由调用方保证生命周期。
- `cli_printf()` 格式化到固定的 `CLI_OUT_MAX` 缓冲，超长会截断（不动态分配）。

---

## 首次接入的验证清单

1. 上电后敲一条未知命令（如 `foo`）：**应当安静**，且每 `drop_report_ms` 最多出现一条
   `[CLI] ignored N line(s)` 摘要。若开始刷屏，说明闸门 4 被绕过了（有人在 handler 里
   直接 `printf` 了未知命令的报错）。
2. 敲 `tx`（带 `guard_ms` 的命令）连按：窗口内的触发应被丢弃并提示 `ignored (guard Nms)`。
3. 确认 `flush` 真的把回显吃掉了：执行完一条命令后不应立刻出现「未知命令」的摘要。
