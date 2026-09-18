/**
 * @file    cli_core.c
 * @brief   串口命令行核心实现（命令注册 + 行解析 + 防回显自激四道闸）
 * @version V1.0
 * @date    2026-09-18
 *
 * 依赖：chip.oop_dwt 的 oop_GetTickMS()（去抖窗口与限速上报的时基）。
 *       不依赖 HAL、不依赖任何具体串口——传输由 cli_transport_t 注入。
 */
#include "cli_core.h"
#include "oop_dwt.h"        /* oop_GetTickMS：去抖窗口 / 限速上报的时基 */

#include <stdarg.h>         /* va_list / va_start / va_end（cli_printf） */
#include <stdio.h>          /* vsnprintf：cli_printf 的格式化 */
#include <string.h>         /* memcpy / strlen */

/* ===========================
 * 内部工具
 * =========================== */

/** 命令名比对：大小写不敏感（参数不做此处理，保留原样） */
static bool cli_name_eq(const char *a, const char *b)
{
    if ((a == NULL) || (b == NULL)) {
        return false;
    }
    while ((*a != '\0') && (*b != '\0')) {
        char x = *a;
        char y = *b;
        if ((x >= 'a') && (x <= 'z')) {
            x = (char)(x - ('a' - 'A'));
        }
        if ((y >= 'a') && (y <= 'z')) {
            y = (char)(y - ('a' - 'A'));
        }
        if (x != y) {
            return false;
        }
        a++;
        b++;
    }
    return ((*a == '\0') && (*b == '\0'));
}

/**
 * @brief 按「扁平序号」查找命令（跨多张表累加计数）
 * @param ordinal_out 命中时的扁平序号，用于索引去抖时间戳表
 * @retval 命中返回命令指针；未命中返回 NULL
 */
static const cli_cmd_t *cli_find(cli_t *cli, const char *name, uint16_t *ordinal_out)
{
    uint16_t ordinal = 0U;

    for (uint8_t t = 0U; t < cli->table_cnt; t++) {
        const cli_cmd_t *tab = cli->tables[t].tab;
        for (uint16_t i = 0U; i < cli->tables[t].n; i++, ordinal++) {
            if ((tab[i].name != NULL) && cli_name_eq(tab[i].name, name)) {
                if (ordinal_out != NULL) {
                    *ordinal_out = ordinal;
                }
                return &tab[i];
            }
        }
    }
    return NULL;
}

/**
 * @brief 记录一条「不认识的行」，限速上报
 * @note  绝不逐条回打印。回显链路上逐条报错会自激放大（见头文件说明），
 *        所以这里只在间隔到达时输出一条摘要，且摘要内容规范化后不等于
 *        任何命令名，不会再次触发执行。
 */
static void cli_drop(cli_t *cli, const char *name)
{
    cli->drop_cnt++;

    size_t n = (name != NULL) ? strlen(name) : 0U;
    if (n > (size_t)(CLI_LINE_MAX - 1U)) {
        n = (size_t)(CLI_LINE_MAX - 1U);
    }
    if (n != 0U) {
        memcpy(cli->drop_last, name, n);
    }
    cli->drop_last[n] = '\0';

    uint32_t now = oop_GetTickMS();
    if ((cli->drop_report_ms == 0U) ||
        ((uint32_t)(now - cli->drop_report_ms) >= (uint32_t)cli->drop_report_interval_ms)) {
        cli->drop_report_ms = now;
        cli_printf(cli, "[CLI] ignored %lu line(s), last=\"%s\"\r\n",
                   (unsigned long)cli->drop_cnt, cli->drop_last);
    }
}

/** 把一行按空白切词（就地写 '\0'），命令名与参数一并放进 argv */
static int cli_tokenize(char *line, char **argv, int argv_cap)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0') {
        while ((*p == ' ') || (*p == '\t')) {
            p++;                                   /* 跳过连续空白（折叠） */
        }
        if (*p == '\0') {
            break;
        }
        if (argc < argv_cap) {
            argv[argc++] = p;
        }
        while ((*p != '\0') && (*p != ' ') && (*p != '\t')) {
            p++;
        }
        if (*p != '\0') {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

/**
 * @brief 执行一行
 * @retval true  = 确实执行了一条命令（调用方应立刻结束本轮，见闸门 1）
 *         false = 未执行（无命令 / 参数非法 / 去抖拦截 / 未知命令）
 */
static bool cli_execute(cli_t *cli, char *line)
{
    char *argv[CLI_MAX_ARGS];
    int argc = cli_tokenize(line, argv, CLI_MAX_ARGS);

    if (argc == 0) {
        return false;                              /* 空行 */
    }

    uint16_t ordinal = 0U;
    const cli_cmd_t *cmd = cli_find(cli, argv[0], &ordinal);
    if (cmd == NULL) {
        cli_drop(cli, argv[0]);                    /* 闸门 4：静默丢弃 + 限速摘要 */
        return false;
    }
    if (ordinal >= (uint16_t)CLI_MAX_CMDS) {
        /* cli_register 已挡住超限，这里只是防御性检查，避免越界写时间戳表 */
        return false;
    }

    /* 闸门 3：同名命令最小间隔，窗口内的重复（多为回显）一律不执行 */
    if ((cmd->guard_ms != 0U) && (cli->guard_valid[ordinal] != 0U)) {
        uint32_t now = oop_GetTickMS();
        if ((uint32_t)(now - cli->guard_last_ms[ordinal]) < (uint32_t)cmd->guard_ms) {
            cli_printf(cli, "[CLI] %s ignored (guard %ums)\r\n",
                       cmd->name, (unsigned)cmd->guard_ms);
            return false;
        }
    }

    cli_ret_t ret = cmd->handler(cli, argc, argv);
    if (ret == CLI_RET_BAD_ARG) {
        cli_print_usage(cli, cmd);                 /* 用法提示由核心统一输出 */
    }
    cli->guard_valid[ordinal]  = 1U;
    cli->guard_last_ms[ordinal] = oop_GetTickMS();
    return true;
}

/** 吃一个字节；返回 true 表示本轮已执行了一条命令（调用方应停止喂入） */
static bool cli_feed_byte(cli_t *cli, uint8_t c)
{
    if ((c == '\r') || (c == '\n')) {
        if (cli->line_len == 0U) {
            return false;                          /* 空行（如 CRLF 的第二个字节） */
        }
        cli->line[cli->line_len] = '\0';
        cli->line_len = 0U;

        if (cli->overflow != 0U) {
            /* 超长行：内容已被截断，执行它就是执行一条残缺命令，宁可丢弃 */
            cli->overflow = 0U;
            cli_drop(cli, cli->line);
            return false;
        }

        bool executed = cli_execute(cli, cli->line);

        /* 闸门 2：执行完排空接收端，丢掉发送 + 打印期间涌入的回显/噪声。
           不执行（拒绝/未知）时排空反而会吃掉用户紧跟着敲的下一条命令。 */
        if (executed && (cli->transport.flush != NULL)) {
            cli->transport.flush(cli->transport.ctx);
        }
        return executed;
    }

    /* 只接受可打印 ASCII；控制字符与非 ASCII 一律按噪声丢弃
       （38kHz 载波耦合、波特率毛刺都会产生这类字节） */
    if ((c < 0x20U) || (c > 0x7EU)) {
        return false;
    }

    if (cli->line_len < (uint16_t)(CLI_LINE_MAX - 1U)) {
        cli->line[cli->line_len] = (char)c;
        cli->line_len++;
    } else {
        cli->overflow = 1U;                        /* 超出容量：丢弃至行尾 */
    }
    return false;
}

/* ===========================
 * 生命周期与注册
 * =========================== */

void cli_init(cli_t *cli, const cli_transport_t *transport)
{
    if (cli == NULL) {
        return;
    }
    memset(cli, 0, sizeof(*cli));
    if (transport != NULL) {
        cli->transport = *transport;
    }
    cli->drop_report_interval_ms = (uint16_t)CLI_DROP_REPORT_MS;

    /* 上电时先清一次，丢掉从复位到串口就绪之间攒下的噪声 */
    if (cli->transport.flush != NULL) {
        cli->transport.flush(cli->transport.ctx);
    }
}

int cli_register(cli_t *cli, const cli_cmd_t *cmds, uint16_t n)
{
    if ((cli == NULL) || (cmds == NULL) || (n == 0U)) {
        return -1;
    }
    if (cli->table_cnt >= (uint8_t)CLI_MAX_TABLES) {
        return -1;
    }
    if (((uint16_t)cli->cmd_total + n) > (uint16_t)CLI_MAX_CMDS) {
        return -1;                                 /* 不静默截断，见头文件说明 */
    }

    cli->tables[cli->table_cnt].tab = cmds;
    cli->tables[cli->table_cnt].n   = n;
    cli->table_cnt++;
    cli->cmd_total = (uint16_t)(cli->cmd_total + n);
    return 0;
}

/* ===========================
 * 输入
 * =========================== */

void cli_process(cli_t *cli)
{
    if ((cli == NULL) || (cli->transport.getc == NULL)) {
        return;
    }

    int c;
    while ((c = cli->transport.getc(cli->transport.ctx)) >= 0) {
        if (cli_feed_byte(cli, (uint8_t)c)) {
            return;                                /* 闸门 1：一次最多执行一条 */
        }
    }
}

uint16_t cli_feed(cli_t *cli, const uint8_t *buf, uint16_t len)
{
    if ((cli == NULL) || (buf == NULL) || (len == 0U)) {
        return 0U;
    }

    for (uint16_t i = 0U; i < len; i++) {
        if (cli_feed_byte(cli, buf[i])) {
            return (uint16_t)(i + 1U);             /* 已执行一条，剩余交给调用方 */
        }
    }
    return len;
}

void cli_reset_input(cli_t *cli)
{
    if (cli == NULL) {
        return;
    }
    cli->line_len = 0U;
    cli->overflow = 0U;
}

/* ===========================
 * 输出
 * =========================== */

void cli_puts(cli_t *cli, const char *s)
{
    if ((cli == NULL) || (s == NULL) || (cli->transport.write == NULL)) {
        return;
    }
    size_t n = strlen(s);
    if (n != 0U) {
        cli->transport.write(cli->transport.ctx, s, (uint16_t)n);
    }
}

void cli_printf(cli_t *cli, const char *fmt, ...)
{
    if ((cli == NULL) || (fmt == NULL) || (cli->transport.write == NULL)) {
        return;
    }

    char buf[CLI_OUT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return;
    }
    if (n > (int)(sizeof(buf) - 1U)) {
        n = (int)(sizeof(buf) - 1U);               /* vsnprintf 已按容量截断并补 '\0' */
    }
    cli->transport.write(cli->transport.ctx, buf, (uint16_t)n);
}

void cli_print_usage(cli_t *cli, const cli_cmd_t *cmd)
{
    if ((cli == NULL) || (cmd == NULL) || (cmd->name == NULL)) {
        return;
    }
    if (cmd->usage != NULL) {
        cli_printf(cli, "[CLI] usage: %s %s\r\n", cmd->name, cmd->usage);
    } else {
        cli_printf(cli, "[CLI] usage: %s\r\n", cmd->name);
    }
}

void cli_print_help(cli_t *cli)
{
    if (cli == NULL) {
        return;
    }
    cli_printf(cli, "[CLI] %u command(s):\r\n", (unsigned)cli->cmd_total);

    for (uint8_t t = 0U; t < cli->table_cnt; t++) {
        const cli_cmd_t *tab = cli->tables[t].tab;
        for (uint16_t i = 0U; i < cli->tables[t].n; i++) {
            const cli_cmd_t *cmd = &tab[i];
            if (cmd->name == NULL) {
                continue;
            }
            cli_printf(cli, "  %s%s%s%s%s\r\n",
                       cmd->name,
                       (cmd->usage != NULL) ? " " : "",
                       (cmd->usage != NULL) ? cmd->usage : "",
                       (cmd->help != NULL) ? "  - " : "",
                       (cmd->help != NULL) ? cmd->help : "");
        }
    }
}

void cli_set_drop_report_ms(cli_t *cli, uint16_t ms)
{
    if (cli != NULL) {
        cli->drop_report_interval_ms = ms;
    }
}

/* ===========================
 * 参数解析工具
 * =========================== */

bool cli_parse_u32(const char *s, uint32_t *out)
{
    if ((s == NULL) || (out == NULL)) {
        return false;
    }

    uint32_t v   = 0U;
    bool     got = false;

    for (const char *p = s; (*p >= '0') && (*p <= '9'); p++) {
        uint32_t d = (uint32_t)(*p - '0');
        /* 溢出检查：宁可报参数错，也不要静默回绕成一个「看起来合法」的值 */
        if (v > ((0xFFFFFFFFUL - d) / 10UL)) {
            return false;
        }
        v = (v * 10U) + d;
        got = true;
    }

    if (!got) {
        return false;
    }
    *out = v;
    return true;
}

bool cli_parse_u32_range(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
    uint32_t v;
    if (!cli_parse_u32(s, &v)) {
        return false;
    }
    if ((v < min) || (v > max)) {
        return false;
    }
    *out = v;
    return true;
}
