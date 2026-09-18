/**
 * @file    cli_core.h
 * @brief   串口命令行核心：命令注册 + 行解析（传输无关，无 HAL / OS 依赖）
 * @version V1.0
 * @date    2026-09-18
 *
 * @note    定位
 *          把「命令表 + 行解析 + 防回显自激」从各工程的 uart_cli.c 里抽出来。
 *          本模块不认识任何业务，也不认识 UART/HAL——工程只提供两样东西：
 *            1) 传输怎么读、怎么写（cli_transport_t）
 *            2) 每条命令干什么（cli_cmd_t[] 静态命令表）
 *          各业务模块自带 xxx_cli_cmds[]，app 侧只做聚合注册，不必改本模块。
 *
 * @note    ★ 四条「防回显自激」的闸门（缺一不可，全部在本模块内实现）
 *          1) 一次 cli_process() 最多执行一条命令，执行完立即返回，
 *             不吃排队的后续行；
 *          2) 执行完调用 transport.flush()，丢弃发送 + 打印期间涌入的回显
 *             与耦合噪声字节；
 *          3) 每条命令可用 guard_ms 设定最小间隔，窗口内的重复触发一律丢弃；
 *          4) 不认识的行「静默丢弃」，绝不回打印。
 *
 *          为什么 4) 是关键：一旦链路上存在回显（上位机终端回显 / TX-RX 走线
 *          串扰 / 发码时 38kHz 载波耦合进接收线），「未知命令」的报错会被回显
 *          再解析、再报错，形成自激放大，表现就是一直刷屏。被丢弃的行改为
 *          限速摘要上报（drop_report_ms 一条），既能定位原因又不会自激。
 *
 * @note    使用流程
 *          @code
 *          static const cli_cmd_t my_cmds[] = { ... };
 *          cli_t cli;                            // 静态分配，无 malloc
 *
 *          cli_init(&cli, &my_transport);        // 注入传输（可先 init 后注册）
 *          cli_register(&cli, my_cmds, sizeof(my_cmds)/sizeof(my_cmds[0]));
 *
 *          while (1) { cli_process(&cli); }      // 主循环 / 任务里轮询
 *          @endcode
 *
 * @note    硬约束
 *          handler 运行在调用 cli_process() 的上下文（通常是主循环），**不得阻塞**。
 *          需要长时间动作时，handler 只置标志或发请求，由对应任务去执行
 *          （与 ir_transmitter_task_send_last 那种「任务层真干活」的分工一致）。
 *
 * @note    与 chip.oop_uart 的关系
 *          oop_uart 是 DMA + 空闲中断的「整包」模型，一次拿到一整包；本模块
 *          用 cli_feed() 接这种包即可（不需要 getc 逐字节拉）。而 getc 逐字节
 *          拉更适用于「直接轮询 USART 寄存器」的轻量场景。两种都支持。
 *          另注意：一个 uart_drv_t 只能有一个消费者，若该串口已被别的设备独占，
 *          CLI 需换串口或与设备合并为同一个分发任务。
 */

#ifndef __CLI_CORE_H
#define __CLI_CORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================
 * 编译期可调参数
 * =========================== */

/** 一行（命令 + 参数）最大字节数，含结尾 '\0' */
#ifndef CLI_LINE_MAX
#define CLI_LINE_MAX            64
#endif

/** 一行最多拆出的词数（argv[0] 是命令名本身） */
#ifndef CLI_MAX_ARGS
#define CLI_MAX_ARGS            6
#endif

/** 单个 cli 实例能挂的命令总数上限（同时是去抖时间戳表的大小） */
#ifndef CLI_MAX_CMDS
#define CLI_MAX_CMDS            16
#endif

/** 单个 cli 实例能注册的命令表个数上限（每个模块一张表） */
#ifndef CLI_MAX_TABLES
#define CLI_MAX_TABLES          4
#endif

/** 单次输出的最大字节数（cli_printf 的格式化缓冲，超长截断） */
#ifndef CLI_OUT_MAX
#define CLI_OUT_MAX             128
#endif

/** 未知命令限速上报的默认间隔（ms）；0 = 每条都报（不建议） */
#ifndef CLI_DROP_REPORT_MS
#define CLI_DROP_REPORT_MS      3000
#endif

/* ===========================
 * 类型
 * =========================== */

typedef struct cli_instance cli_t;

/**
 * @brief 命令处理函数的返回值
 * @note  返回 CLI_RET_BAD_ARG 时，核心会自动打印该命令的 usage，
 *        所以 handler 里不必再手写「参数错了」的提示（去重复）。
 */
typedef enum {
    CLI_RET_OK = 0,         /**< 执行成功 */
    CLI_RET_BAD_ARG,        /**< 参数不合法，核心代打 usage */
    CLI_RET_FAIL,           /**< 执行失败（原因由 handler 自己说明） */
} cli_ret_t;

/**
 * @brief 命令处理函数
 * @param cli  所属实例（用 cli_printf 输出，保持传输无关）
 * @param argc 词数，>= 1；argv[0] 是命令名本身
 * @param argv 词数组（指向行缓冲，仅在本次调用内有效，勿保存指针）
 * @note  禁止阻塞。
 */
typedef cli_ret_t (*cli_handler_t)(cli_t *cli, int argc, char **argv);

/**
 * @brief 命令描述（静态表，零拷贝注册；表放 flash 即可）
 */
typedef struct {
    const char  *name;      /**< 命令名，匹配时大小写不敏感 */
    const char  *usage;     /**< 参数用法，如 "<0|1>"；无参数填 NULL */
    const char  *help;      /**< 一行说明；可填 NULL */
    cli_handler_t handler;  /**< 处理函数，禁止阻塞 */
    uint16_t     guard_ms;  /**< 同名命令最小间隔(ms)；0 = 不限制 */
} cli_cmd_t;

/**
 * @brief 传输接口（由使用方实现；getc 与 write 必需，flush 可选）
 * @note  ctx 会原样回传到每个回调，用来携带具体的串口/句柄，
 *        这样本模块无需知道任何硬件类型。
 */
typedef struct {
    /** 非阻塞取一个字节；无数据返回 -1 */
    int  (*getc)(void *ctx);
    /** 输出一段字节（应保证发完再返回） */
    void (*write)(void *ctx, const char *s, uint16_t n);
    /** 排空接收端积压（丢弃回显/噪声）；不需要可填 NULL */
    void (*flush)(void *ctx);
    /** 透传给上述回调的上下文 */
    void *ctx;
} cli_transport_t;

/* ===========================
 * 实例
 * =========================== */
struct cli_instance {
    /* 命令表（可多次注册，累加） */
    struct {
        const cli_cmd_t *tab;
        uint16_t         n;
    } tables[CLI_MAX_TABLES];
    uint8_t  table_cnt;
    uint16_t cmd_total;             /* 已注册命令总数（跨表累加，同时是去抖表的有效长度） */

    /* 传输 */
    cli_transport_t transport;

    /* 行缓冲 */
    char     line[CLI_LINE_MAX];
    uint16_t line_len;
    uint8_t  overflow;              /* 本行已超长，丢弃至行尾 */

    /* 去抖：按命令的「扁平序号」索引（跨表累加） */
    uint32_t guard_last_ms[CLI_MAX_CMDS];
    uint8_t  guard_valid[CLI_MAX_CMDS];

    /* 未知行统计（仅诊断，不参与控制流） */
    uint32_t drop_cnt;
    uint32_t drop_report_ms;
    uint16_t drop_report_interval_ms;
    char     drop_last[CLI_LINE_MAX];
};

/* ===========================
 * 生命周期与注册
 * =========================== */

/**
 * @brief  初始化实例并注入传输
 * @param  cli       实例（由调用方静态分配）
 * @param  transport 传输接口，内部按值拷贝；ctx 需指向调用方保证生命周期的对象
 * @note   若 transport.flush 非 NULL，初始化时会调用一次以清掉上电噪声。
 */
void cli_init(cli_t *cli, const cli_transport_t *transport);

/**
 * @brief  注册一批命令（可多次调用，累加到同一实例）
 * @param  cmds 命令表，需在实例生命周期内保持有效（通常为 static const）
 * @param  n    表内命令个数
 * @retval 0 成功；-1 参数非法，或超出 CLI_MAX_CMDS / CLI_MAX_TABLES
 * @note   超限时返回 -1 而不是静默截断——静默丢命令会导致「命令注册了但
 *         执行不了」，比编译期就发现的错误难查得多。
 */
int cli_register(cli_t *cli, const cli_cmd_t *cmds, uint16_t n);

/* ===========================
 * 输入
 * =========================== */

/**
 * @brief  从 transport.getc 逐字节拉取并解析（一次调用最多执行一条命令）
 * @note   主循环里周期调用即可。执行完一条命令会立刻返回，并调用 flush。
 */
void cli_process(cli_t *cli);

/**
 * @brief  直接推入一段已收到的字节（用于整包式传输，如 chip.oop_uart 的包）
 * @param  buf 字节缓冲
 * @param  len 字节数
 * @retval 实际消费的字节数；若中途执行了命令则立即结束，剩余未消费
 * @note   与 cli_process 共用同一套行缓冲与闸门；执行完命令同样会调 flush
 *         （若已配置）。剩下的字节是回显还是下一条命令，交给调用方判断。
 */
uint16_t cli_feed(cli_t *cli, const uint8_t *buf, uint16_t len);

/** 丢弃当前未完成的行（可用于上电、或链路异常后重新同步） */
void cli_reset_input(cli_t *cli);

/* ===========================
 * 输出
 * =========================== */

/** 输出一个以 '\0' 结尾的字符串 */
void cli_puts(cli_t *cli, const char *s);

/**
 * @brief  格式化输出（printf 风格，经 transport.write 出）
 * @note   内部用 vsnprintf 格式化到固定缓冲，超长会截断（CLI_OUT_MAX）。
 *         使用方需自行保证格式串与参数匹配（与 printf 同）。
 */
void cli_printf(cli_t *cli, const char *fmt, ...);

/** 设置「未知行限速上报」的间隔（ms）；0 = 每条都报（不建议，回显链路可能自激） */
void cli_set_drop_report_ms(cli_t *cli, uint16_t ms);

/** 打印某条命令的用法，形如 "[CLI] usage: txcar <1000..60000>" */
void cli_print_usage(cli_t *cli, const cli_cmd_t *cmd);

/** 打印全部已注册命令的清单（可注册成一条 help 命令） */
void cli_print_help(cli_t *cli);

/* ===========================
 * 参数解析工具
 * =========================== */

/**
 * @brief  解析无符号十进制数（从字符串起始，遇非数字停止）
 * @retval true 至少解析到一位数字且未溢出；false 否则（*out 不被修改）
 * @note   语义是「前缀解析」："38000x" 得 38000。这是为了兼容上位机在参数
 *         后接多余字符的情况；需要严格校验时请改用 cli_parse_u32_range 并
 *         自行检查 argv 长度。
 */
bool cli_parse_u32(const char *s, uint32_t *out);

/**
 * @brief  解析无符号十进制数并做范围检查
 * @param  min 下限（含）；max 上限（含）
 * @retval true 解析成功且 min <= v <= max
 */
bool cli_parse_u32_range(const char *s, uint32_t min, uint32_t max, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __CLI_CORE_H */
