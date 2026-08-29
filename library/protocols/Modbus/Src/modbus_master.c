#include "modbus_master.h"
#include "SEGGER_RTT_Log.h"
#include <string.h>

/* ===========================
 * 内部：主机请求（唯一发送入口）
 * =========================== */
// master_request_async 改成非阻塞
static int master_request_async(modbus_t *ctx, uint8_t func_code,
                                    const uint8_t *req_data, uint16_t req_len,
                                    uint8_t *resp_buf, uint16_t *resp_len,
                                    uint32_t timeout_ms) {
    (void)timeout_ms;   /* 超时统一由 check_line_status 按 ctx->response_timeout 处理 */
    if (!ctx || ctx->role != MODBUS_ROLE_MASTER) return -1;
    if (ctx->state == MODBUS_STATE_WAITING_RESPONSE) return -2;
    if (ctx->line_state == MODBUS_LINE_DISCONNECTED) return -3;
    
    ctx->tx_buf[0] = ctx->slave_addr;
    ctx->tx_buf[1] = func_code;
    if (req_data && req_len > 0) {
        memcpy(ctx->tx_buf + 2, req_data, req_len);
    }
    ctx->tx_len = 2 + req_len;   /* PDU 长度（不含帧封装） */
    
    MODBUS_LOG("Master send: func=0x%02X", func_code);
    HEX_LOG("TX: ", ctx->tx_buf, ctx->tx_len);
    
    ctx->transaction.req_data = ctx->tx_buf;
    ctx->transaction.req_len = ctx->tx_len;
    ctx->transaction.resp_data = resp_buf;
    ctx->transaction.resp_len = resp_len;
    ctx->transaction.func_code = func_code;
    ctx->transaction.pending = true;
    ctx->transaction.completed = false;
    ctx->transaction.result = -1;
    if (resp_len) *resp_len = 0;
    
    uint8_t *send_buf = ctx->tx_buf;
    uint16_t send_len = ctx->tx_len;
    if (ctx->transport.frame_tx) {
        send_len = ctx->transport.frame_tx(ctx->transport.ctx,
                                           ctx->tx_buf, ctx->tx_len,
                                           ctx->tx_frame, MODBUS_BUF_SIZE);
        send_buf = ctx->tx_frame;
    }
    int ret = ctx->transport.send(ctx->transport.ctx, send_buf, send_len);
    if (ret != 0) {
        ctx->transaction.pending = false;
        return ret;
    }
    
    ctx->state = MODBUS_STATE_WAITING_RESPONSE;
    MODBUS_LOG("[MASTER] state set to WAITING_RESPONSE");
    ctx->send_tick = ctx->transport.get_tick();
    ctx->timeout_count = 0;
    
    // ★★★ 不阻塞，直接返回，由 modbus_process 处理超时 ★★★
    return 0;
}

/* ===========================
 * 统一总线仲裁器（2026-08-18）
 *
 * 所有发往总线的帧都是作业：周期读段（多段寄存器/线圈）、一次性写、
 * 一次性读。调度每 tick 从高优先级向低优先级挑一帧：
 *   1. 防饿死提升：周期读错过最后期限（等待 > 2×period）→ 无条件先发
 *   2. 高优一次性作业：写 → 手动读（FIFO）
 *   3. 周期读：最早到期（EDF）
 * 帧节拍由 min_frame_gap_ms 全局约束（默认 = poll_interval = 100ms，
 * 即"查询指令 100ms 一条"）。应答由 modbus_core 解析出后回调
 * mb_on_master_response，按 transaction.job 路由到各作业。
 * =========================== */
#define MB_POLL_REG_MAX    8
#define MB_POLL_COIL_MAX   4
#define MB_WRITE_JOB_MAX   12   /* >= boot-push 一次性入队数（5 块+4 线圈=9）+ DOP107 并发帧 */
#define MB_READ_ONCE_MAX   4

typedef enum {
    MB_JOB_NONE = 0,
    MB_JOB_WRITE,
    MB_JOB_READ_ONCE,
    MB_JOB_REG_RANGE,
    MB_JOB_COIL_RANGE,
} mb_job_kind_t;

/* 在途作业引用：调度时挂到 ctx->transaction.job，应答时按此路由 */
typedef struct {
    uint8_t kind;
    void *ptr;
} mb_job_ref_t;

/* 周期读段：一段寄存器 / 一段线圈 */
typedef struct {
    uint16_t start, count;
    uint16_t *shadow;          /* 回读镜像（同时为最近值 + 变更基准），下标相对 start */
    uint16_t shadow_cap;
    uint32_t period_ms, phase_ms;
    uint32_t next_due;
    bool baseline_done;
    bool in_use;
} mb_reg_range_t;

typedef struct {
    uint16_t start, count;
    uint8_t *shadow;           /* 位镜像（字节） */
    uint16_t shadow_cap;
    uint32_t period_ms, phase_ms;
    uint32_t next_due;
    bool baseline_done;
    bool in_use;
} mb_coil_range_t;

/* 一次性写作业 */
typedef struct {
    uint8_t func;              /* 06 / 10 / 05 */
    uint16_t addr, count;
    union {
        uint16_t value;        /* 单寄存器/线圈 */
        const uint16_t *regs;  /* 多寄存器：调用方持有，done 回调前必须保持有效 */
    } data;
    bool in_use;
    uint16_t seq;              /* 入队序号：仲裁器按最小 seq 选择（真 FIFO） */
    mb_job_done_t done;
} mb_write_job_t;

/* 一次性读作业（未注册地址也可读） */
typedef struct {
    uint8_t func;              /* 03 / 01 */
    uint16_t addr, count;
    void *buf;                 /* 调用方缓冲（寄存器=words 数组，线圈=字节数组） */
    uint16_t buf_cap;
    bool in_use;
    uint16_t seq;              /* 入队序号（与写作业同计数器） */
    mb_job_done_t done;
} mb_read_once_t;

typedef struct {
    mb_reg_range_t  regs[MB_POLL_REG_MAX];
    mb_coil_range_t coils[MB_POLL_COIL_MAX];
    mb_write_job_t  writes[MB_WRITE_JOB_MAX];
    mb_read_once_t  reads[MB_READ_ONCE_MAX];
    mb_job_ref_t    cur;               /* 当前在途作业引用 */
    uint32_t min_frame_gap_ms;
    uint32_t last_send_tick;
    uint16_t job_seq;                  /* 一次性作业入队序号（写+读共用，单调递增） */
} mb_arbiter_t;

/* 每主机实例独立仲裁器（原单全局 g_arbiter 在多主机下被后一次 init 的 memset 清掉、
 * 所有实例 master_priv 指向同一块 → 多主机配置互相覆盖；表现为"最后初始化的主机
 * 覆盖前面所有主机"，故出现 func=0x01 等反常）。改为按实例分配的小池，仅主机实例占用，
 * 从机/服务器 slot 不占 RAM。 */
#define MB_ARBITER_POOL_MAX   6   /* RTU 2 + TCP client 2 = 4，留余量 */
static mb_arbiter_t g_arbiter_pool[MB_ARBITER_POOL_MAX];
static int          g_arbiter_next = 0;

static mb_arbiter_t *mb_get_arbiter(modbus_t *ctx) {
    return (ctx && ctx->master_priv) ? (mb_arbiter_t *)ctx->master_priv : NULL;
}

/* ===========================
 * 应答数据落地（按作业类型）
 * =========================== */

/* 寄存器段：大端字节序逐寄存器拼装，首帧做基线、之后逐字比对触发变更回调 */
static void mb_fill_reg_range(modbus_t *ctx, mb_reg_range_t *r) {
    if (ctx->rx_len < 3) return;
    uint16_t n = ctx->rx_buf[2];
    uint16_t cnt = n / 2;
    if (cnt > r->count) cnt = r->count;
    if (cnt > r->shadow_cap) cnt = r->shadow_cap;
    if (cnt == 0) return;

    if (!r->baseline_done) {
        for (uint16_t i = 0; i < cnt; i++) {
            r->shadow[i] = (uint16_t)((ctx->rx_buf[3 + i * 2] << 8) |
                                      ctx->rx_buf[3 + i * 2 + 1]);
        }
        r->baseline_done = true;
        return;
    }
    if (!ctx->on_master_reg_change) {
        for (uint16_t i = 0; i < cnt; i++) {
            r->shadow[i] = (uint16_t)((ctx->rx_buf[3 + i * 2] << 8) |
                                      ctx->rx_buf[3 + i * 2 + 1]);
        }
        return;
    }
    for (uint16_t i = 0; i < cnt; i++) {
        uint16_t nv = (uint16_t)((ctx->rx_buf[3 + i * 2] << 8) |
                                 ctx->rx_buf[3 + i * 2 + 1]);
        if (nv != r->shadow[i]) {
            ctx->on_master_reg_change((uint16_t)(r->start + i), r->shadow[i], nv);
            r->shadow[i] = nv;
        }
    }
}

/* 线圈段：位级比对，回调传绝对地址 */
static void mb_fill_coil_range(modbus_t *ctx, mb_coil_range_t *c) {
    if (ctx->rx_len < 3) return;
    uint16_t n = ctx->rx_buf[2];
    uint16_t maxb = (uint16_t)((c->count + 7) / 8);
    if (n > maxb) n = maxb;
    if (n > c->shadow_cap) n = c->shadow_cap;
    if (n == 0) return;

    if (!c->baseline_done) {
        memcpy(c->shadow, ctx->rx_buf + 3, n);
        c->baseline_done = true;
        return;
    }
    if (!ctx->on_master_coil_change) {
        memcpy(c->shadow, ctx->rx_buf + 3, n);
        return;
    }
    for (uint16_t i = 0; i < c->count; i++) {
        uint8_t bi = (uint8_t)(i / 8);
        if (bi >= n) break;
        uint8_t bit = (uint8_t)(i % 8);
        bool nv = (ctx->rx_buf[3 + bi] >> bit) & 0x01;
        bool ov = (c->shadow[bi] >> bit) & 0x01;
        if (nv != ov) {
            ctx->on_master_coil_change((uint16_t)(c->start + i), ov, nv);
            if (nv) c->shadow[bi] |= (uint8_t)(1u << bit);
            else    c->shadow[bi] &= (uint8_t)~(1u << bit);
        }
    }
}

/* 一次性读：应答填调用方缓冲（大端拼装） */
static void mb_fill_read_once(modbus_t *ctx, mb_read_once_t *ro) {
    if (ctx->rx_len < 3 || !ro->buf) return;
    uint16_t n = ctx->rx_buf[2];
    if (ro->func == MODBUS_FC_READ_HOLDING_REGS) {
        uint16_t cnt = n / 2;
        if (cnt > ro->buf_cap) cnt = ro->buf_cap;
        uint16_t *dst = (uint16_t *)ro->buf;
        for (uint16_t i = 0; i < cnt; i++) {
            dst[i] = (uint16_t)((ctx->rx_buf[3 + i * 2] << 8) |
                                ctx->rx_buf[3 + i * 2 + 1]);
        }
    } else {
        if (n > ro->buf_cap) n = ro->buf_cap;
        memcpy(ro->buf, ctx->rx_buf + 3, n);
    }
}

/* ===========================
 * 应答钩子（modbus_core 在事务完成时回调）
 * =========================== */
static void mb_on_master_response(modbus_t *ctx) {
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a || !ctx->transaction.job) return;
    mb_job_ref_t *ref = (mb_job_ref_t *)ctx->transaction.job;
    int res = ctx->transaction.result;

    if (res != 0) {
        /* 失败（超时/异常）：一次性作业以失败收尾；周期读等下一周期重试 */
        if (ref->kind == MB_JOB_WRITE) {
            mb_write_job_t *w = (mb_write_job_t *)ref->ptr;
            w->in_use = false;
            if (w->done) w->done(res);
        } else if (ref->kind == MB_JOB_READ_ONCE) {
            mb_read_once_t *ro = (mb_read_once_t *)ref->ptr;
            ro->in_use = false;
            if (ro->done) ro->done(res);
        }
        ctx->transaction.job = NULL;
        return;
    }

    switch (ref->kind) {
        case MB_JOB_WRITE: {
            mb_write_job_t *w = (mb_write_job_t *)ref->ptr;
            w->in_use = false;
            if (w->done) w->done(0);
            break;
        }
        case MB_JOB_READ_ONCE: {
            mb_read_once_t *ro = (mb_read_once_t *)ref->ptr;
            mb_fill_read_once(ctx, ro);
            ro->in_use = false;
            if (ro->done) ro->done(0);
            break;
        }
        case MB_JOB_REG_RANGE:
            mb_fill_reg_range(ctx, (mb_reg_range_t *)ref->ptr);
            break;
        case MB_JOB_COIL_RANGE:
            mb_fill_coil_range(ctx, (mb_coil_range_t *)ref->ptr);
            break;
        default:
            break;
    }
    ctx->transaction.job = NULL;
}

/* ===========================
 * 调度：发一帧
 * =========================== */

/* 登记在途作业引用并发送；返回 master_request_async 的结果 */
static int mb_dispatch(modbus_t *ctx, mb_arbiter_t *a, uint8_t kind, void *ptr,
                       uint8_t func_code, const uint8_t *req, uint16_t req_len) {
    a->cur.kind = kind;
    a->cur.ptr = ptr;
    ctx->transaction.job = &a->cur;
    int ret = master_request_async(ctx, func_code, req, req_len, NULL, NULL,
                                   ctx->response_timeout);
    if (ret != 0) {
        ctx->transaction.job = NULL;
        /* ★ 发送失败（TX_BUSY / 断线等）：立即释放作业槽并回调 done，
         * 避免作业永久 in_use 把写队列 12 槽占满（曾导致波形 flush
         * enqueue ret=-2 无限重试"卡死"）；上层（DOP107/boot-push）收到
         * done 失败后下轮自行重新入队。周期读段常驻，仅跳过本轮。 */
        if (kind == MB_JOB_WRITE) {
            mb_write_job_t *w = (mb_write_job_t *)ptr;
            w->in_use = false;
            if (w->done) w->done(ret);
        } else if (kind == MB_JOB_READ_ONCE) {
            mb_read_once_t *ro = (mb_read_once_t *)ptr;
            ro->in_use = false;
            if (ro->done) ro->done(ret);
        }
        return ret;
    }
    a->last_send_tick = ctx->transport.get_tick();
    return 0;
}

static void mb_send_write(modbus_t *ctx, mb_arbiter_t *a, int wi) {
    mb_write_job_t *w = &a->writes[wi];
    uint8_t req[256];
    uint16_t n = 0;
    switch (w->func) {
        case MODBUS_FC_WRITE_SINGLE_REG:
        case MODBUS_FC_WRITE_SINGLE_COIL:
            req[n++] = (uint8_t)(w->addr >> 8);
            req[n++] = (uint8_t)(w->addr & 0xFF);
            req[n++] = (uint8_t)(w->data.value >> 8);
            req[n++] = (uint8_t)(w->data.value & 0xFF);
            break;
        case MODBUS_FC_WRITE_MULTIPLE_REGS:
            req[n++] = (uint8_t)(w->addr >> 8);
            req[n++] = (uint8_t)(w->addr & 0xFF);
            req[n++] = (uint8_t)(w->count >> 8);
            req[n++] = (uint8_t)(w->count & 0xFF);
            req[n++] = (uint8_t)(w->count * 2);
            for (uint16_t i = 0; i < w->count; i++) {
                req[n++] = (uint8_t)(w->data.regs[i] >> 8);
                req[n++] = (uint8_t)(w->data.regs[i] & 0xFF);
            }
            break;
        default:
            w->in_use = false;
            if (w->done) w->done(-1);
            return;
    }
    (void)mb_dispatch(ctx, a, MB_JOB_WRITE, w, w->func, req, n);
}

static void mb_send_read_once(modbus_t *ctx, mb_arbiter_t *a, int ri) {
    mb_read_once_t *ro = &a->reads[ri];
    uint8_t req[4];
    req[0] = (uint8_t)(ro->addr >> 8);
    req[1] = (uint8_t)(ro->addr & 0xFF);
    req[2] = (uint8_t)(ro->count >> 8);
    req[3] = (uint8_t)(ro->count & 0xFF);
    (void)mb_dispatch(ctx, a, MB_JOB_READ_ONCE, ro, ro->func, req, 4);
}

static void mb_send_reg_range(modbus_t *ctx, mb_arbiter_t *a, int ri) {
    mb_reg_range_t *r = &a->regs[ri];
    uint8_t req[4];
    req[0] = (uint8_t)(r->start >> 8);
    req[1] = (uint8_t)(r->start & 0xFF);
    req[2] = (uint8_t)(r->count >> 8);
    req[3] = (uint8_t)(r->count & 0xFF);
    if (mb_dispatch(ctx, a, MB_JOB_REG_RANGE, r,
                    MODBUS_FC_READ_HOLDING_REGS, req, 4) == 0) {
        /* 发出即排下次（无应答时也按周期重试，与旧版 poll_interval 语义一致） */
        r->next_due = a->last_send_tick + r->period_ms;
    }
}

static void mb_send_coil_range(modbus_t *ctx, mb_arbiter_t *a, int ci) {
    mb_coil_range_t *c = &a->coils[ci];
    uint8_t req[4];
    req[0] = (uint8_t)(c->start >> 8);
    req[1] = (uint8_t)(c->start & 0xFF);
    req[2] = (uint8_t)(c->count >> 8);
    req[3] = (uint8_t)(c->count & 0xFF);
    if (mb_dispatch(ctx, a, MB_JOB_COIL_RANGE, c,
                    MODBUS_FC_READ_COILS, req, 4) == 0) {
        c->next_due = a->last_send_tick + c->period_ms;
    }
}

/* ===========================
 * 仲裁器 tick（挂到 ctx->poll_callback，由 modbus_process 末尾调用）
 * =========================== */
static void mb_arbiter_tick(modbus_t *ctx) {
    if (!ctx) return;
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a) return;
    if (ctx->state != MODBUS_STATE_IDLE) return;
    if (ctx->line_state == MODBUS_LINE_DISCONNECTED) return;   /* 断线不发，等重连 */

    uint32_t now = ctx->transport.get_tick();

    /* 帧节拍约束：查询指令 100ms 一条（读写一视同仁） */
    if (a->min_frame_gap_ms > 0) {
        if (now - a->last_send_tick < a->min_frame_gap_ms) return;
    }

    /* ---- 0. 防饿死：周期读错过最后期限（等待 > 2×period）→ 无条件提升 ---- */
    {
        int sel_reg = -1, sel_coil = -1;
        int32_t best = INT32_MIN;
        for (int i = 0; i < MB_POLL_REG_MAX; i++) {
            mb_reg_range_t *r = &a->regs[i];
            if (!r->in_use) continue;
            int32_t d = (int32_t)(now - r->next_due);
            if (d >= (int32_t)(r->period_ms * 2) && d > best) {
                best = d; sel_reg = i;
            }
        }
        for (int i = 0; i < MB_POLL_COIL_MAX; i++) {
            mb_coil_range_t *c = &a->coils[i];
            if (!c->in_use) continue;
            int32_t d = (int32_t)(now - c->next_due);
            if (d >= (int32_t)(c->period_ms * 2) && d > best) {
                best = d; sel_coil = i;
            }
        }
        if (sel_reg >= 0) { mb_send_reg_range(ctx, a, sel_reg); return; }
        if (sel_coil >= 0) { mb_send_coil_range(ctx, a, sel_coil); return; }
    }

    /* ---- 1. 高优一次性作业：写 → 手动读（真 FIFO，按入队序号最小优先） ----
     * ⚠️ 2026-08-19 修复：原实现"槽号优先"（for i=0 找第一个 in_use）在波形
     * 背靠背滚动下会饿死其他写作业——波形帧 done 回调里立即入队下一帧，永远
     * 抢回最小槽位，sel 反转回写/参考线等后入队的大槽作业永远轮不到（实机：
     * 改 sel 后屏上到位/反弹值不刷新）。改为选 seq 最小 = 先入队先发。 */
    {
        int    best_w = -1;
        uint16_t best_seq = 0xFFFF;
        for (int i = 0; i < MB_WRITE_JOB_MAX; i++) {
            if (a->writes[i].in_use && a->writes[i].seq < best_seq) {
                best_seq = a->writes[i].seq;
                best_w   = i;
            }
        }
        if (best_w >= 0) { mb_send_write(ctx, a, best_w); return; }
    }
    {
        int    best_r = -1;
        uint16_t best_seq = 0xFFFF;
        for (int i = 0; i < MB_READ_ONCE_MAX; i++) {
            if (a->reads[i].in_use && a->reads[i].seq < best_seq) {
                best_seq = a->reads[i].seq;
                best_r   = i;
            }
        }
        if (best_r >= 0) { mb_send_read_once(ctx, a, best_r); return; }
    }

    /* ---- 2. 周期读：最早到期（EDF；寄存器段平局优先） ---- */
    {
        int sel_reg = -1, sel_coil = -1;
        uint32_t best = 0xFFFFFFFFu;
        for (int i = 0; i < MB_POLL_REG_MAX; i++) {
            mb_reg_range_t *r = &a->regs[i];
            if (!r->in_use) continue;
            if ((int32_t)(now - r->next_due) < 0) continue;      /* 未到期 */
            if (r->next_due < best) { best = r->next_due; sel_reg = i; }
        }
        for (int i = 0; i < MB_POLL_COIL_MAX; i++) {
            mb_coil_range_t *c = &a->coils[i];
            if (!c->in_use) continue;
            if ((int32_t)(now - c->next_due) < 0) continue;
            if (c->next_due < best) { best = c->next_due; sel_coil = i; }
        }
        if (sel_reg >= 0) { mb_send_reg_range(ctx, a, sel_reg); return; }
        if (sel_coil >= 0) { mb_send_coil_range(ctx, a, sel_coil); return; }
    }
}

/* ===========================
 * 初始化
 * =========================== */
void modbus_master_init(modbus_t *ctx, const modbus_master_config_t *cfg) {
    if (!ctx || !cfg) return;

    modbus_init(ctx);
    modbus_set_role(ctx, MODBUS_ROLE_MASTER);
    modbus_set_slave_addr(ctx, cfg->target_slave_addr);
    modbus_set_timeouts(ctx, cfg->response_timeout_ms, 0, cfg->max_retries);
    modbus_set_reconnect_interval(ctx, cfg->reconnect_interval_ms);

    ctx->poll_interval = cfg->poll_interval_ms ? cfg->poll_interval_ms : 100;

    if (g_arbiter_next >= MB_ARBITER_POOL_MAX) {
        MODBUS_LOG("Master init FAILED: arbiter pool exhausted (%d)", MB_ARBITER_POOL_MAX);
        return;
    }
    mb_arbiter_t *a = &g_arbiter_pool[g_arbiter_next++];
    memset(a, 0, sizeof(*a));
    a->min_frame_gap_ms = cfg->min_frame_gap_ms ? cfg->min_frame_gap_ms
                                                : ctx->poll_interval;

    ctx->master_priv = a;
    ctx->on_master_response = mb_on_master_response;
    ctx->poll_callback = mb_arbiter_tick;

    MODBUS_LOG("Master arbiter initialized, target=%d, poll=%lu ms, gap=%lu ms",
               cfg->target_slave_addr, (unsigned long)ctx->poll_interval,
               (unsigned long)a->min_frame_gap_ms);
}

/* ===========================
 * 多段轮询注册
 * =========================== */
int modbus_master_add_reg_range(modbus_t *ctx, uint16_t start, uint16_t count,
                                uint16_t *shadow, uint16_t shadow_cap,
                                uint32_t period_ms, uint32_t phase_ms) {
    if (!ctx || !shadow || count == 0 || count > 125) return -1;
    if (!ctx->transport.get_tick) return -4;    /* 传输层未挂载 */
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a) return -2;
    for (int i = 0; i < MB_POLL_REG_MAX; i++) {
        if (a->regs[i].in_use) continue;
        mb_reg_range_t *r = &a->regs[i];
        r->in_use = true;
        r->start = start;
        r->count = count;
        r->shadow = shadow;
        r->shadow_cap = shadow_cap;
        r->period_ms = period_ms ? period_ms : ctx->poll_interval;
        r->phase_ms = phase_ms;
        r->next_due = ctx->transport.get_tick() + phase_ms;
        r->baseline_done = false;
        return 0;
    }
    return -3;   /* 表满 */
}

int modbus_master_add_coil_range(modbus_t *ctx, uint16_t start, uint16_t count,
                                 uint8_t *shadow, uint16_t shadow_cap,
                                 uint32_t period_ms, uint32_t phase_ms) {
    if (!ctx || !shadow || count == 0 || count > 2000) return -1;
    if (!ctx->transport.get_tick) return -4;
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a) return -2;
    for (int i = 0; i < MB_POLL_COIL_MAX; i++) {
        if (a->coils[i].in_use) continue;
        mb_coil_range_t *c = &a->coils[i];
        c->in_use = true;
        c->start = start;
        c->count = count;
        c->shadow = shadow;
        c->shadow_cap = shadow_cap;
        c->period_ms = period_ms ? period_ms : ctx->poll_interval;
        c->phase_ms = phase_ms;
        c->next_due = ctx->transport.get_tick() + phase_ms;
        c->baseline_done = false;
        return 0;
    }
    return -3;
}

/* ===========================
 * 一次性写作业
 * =========================== */
int modbus_master_write_reg_async(modbus_t *ctx, uint16_t addr, uint16_t val,
                                  mb_job_done_t done) {
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a) return -1;
    if (ctx->line_state == MODBUS_LINE_DISCONNECTED) return -3;   /* 断线：不入队防积压冻结 */
    for (int i = 0; i < MB_WRITE_JOB_MAX; i++) {
        if (a->writes[i].in_use) continue;
        mb_write_job_t *w = &a->writes[i];
        w->func = MODBUS_FC_WRITE_SINGLE_REG;
        w->addr = addr;
        w->count = 1;
        w->data.value = val;
        w->seq = ++a->job_seq;
        w->done = done;
        w->in_use = true;
        return 0;
    }
    return -2;   /* 槽满 */
}

int modbus_master_write_regs_async(modbus_t *ctx, uint16_t addr, uint16_t count,
                                   const uint16_t *vals, mb_job_done_t done) {
    if (!vals || count == 0 || count > 123) return -1;
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a) return -2;
    if (ctx->line_state == MODBUS_LINE_DISCONNECTED) return -3;   /* 断线：不入队防积压冻结 */
    for (int i = 0; i < MB_WRITE_JOB_MAX; i++) {
        if (a->writes[i].in_use) continue;
        mb_write_job_t *w = &a->writes[i];
        w->func = MODBUS_FC_WRITE_MULTIPLE_REGS;
        w->addr = addr;
        w->count = count;
        w->data.regs = vals;
        w->seq = ++a->job_seq;
        w->done = done;
        w->in_use = true;
        return 0;
    }
    return -2;
}

int modbus_master_write_coil_async(modbus_t *ctx, uint16_t addr, bool val,
                                   mb_job_done_t done) {
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a) return -1;
    if (ctx->line_state == MODBUS_LINE_DISCONNECTED) return -3;   /* 断线：不入队防积压冻结 */
    for (int i = 0; i < MB_WRITE_JOB_MAX; i++) {
        if (a->writes[i].in_use) continue;
        mb_write_job_t *w = &a->writes[i];
        w->func = MODBUS_FC_WRITE_SINGLE_COIL;
        w->addr = addr;
        w->count = 1;
        w->data.value = val ? 0xFF00 : 0x0000;
        w->seq = ++a->job_seq;
        w->done = done;
        w->in_use = true;
        return 0;
    }
    return -2;
}

/* ===========================
 * 一次性读作业（未注册地址也可读）
 * =========================== */
int modbus_master_read_regs_async(modbus_t *ctx, uint16_t addr, uint16_t count,
                                  uint16_t *buf, uint16_t buf_cap, mb_job_done_t done) {
    if (!buf || count == 0 || count > 125) return -1;
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a) return -2;
    if (ctx->line_state == MODBUS_LINE_DISCONNECTED) return -3;   /* 断线：不入队防积压冻结 */
    for (int i = 0; i < MB_READ_ONCE_MAX; i++) {
        if (a->reads[i].in_use) continue;
        mb_read_once_t *ro = &a->reads[i];
        ro->func = MODBUS_FC_READ_HOLDING_REGS;
        ro->addr = addr;
        ro->count = count;
        ro->buf = buf;
        ro->buf_cap = buf_cap;
        ro->seq = ++a->job_seq;
        ro->done = done;
        ro->in_use = true;
        return 0;
    }
    return -2;
}

int modbus_master_read_coils_async(modbus_t *ctx, uint16_t addr, uint16_t count,
                                   uint8_t *buf, uint16_t buf_cap, mb_job_done_t done) {
    if (!buf || count == 0 || count > 2000) return -1;
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a) return -2;
    if (ctx->line_state == MODBUS_LINE_DISCONNECTED) return -3;   /* 断线：不入队防积压冻结 */
    for (int i = 0; i < MB_READ_ONCE_MAX; i++) {
        if (a->reads[i].in_use) continue;
        mb_read_once_t *ro = &a->reads[i];
        ro->func = MODBUS_FC_READ_COILS;
        ro->addr = addr;
        ro->count = count;
        ro->buf = buf;
        ro->buf_cap = buf_cap;
        ro->seq = ++a->job_seq;
        ro->done = done;
        ro->in_use = true;
        return 0;
    }
    return -2;
}

/* ===========================
 * 配置与回调
 * =========================== */
void modbus_master_set_reg_change_callback(modbus_t *ctx,
    void (*callback)(uint16_t addr, uint16_t old_val, uint16_t new_val)) {
    if (ctx) ctx->on_master_reg_change = callback;
}

void modbus_master_set_coil_change_callback(modbus_t *ctx,
    void (*callback)(uint16_t addr, bool old_val, bool new_val)) {
    if (ctx) ctx->on_master_coil_change = callback;
}

void modbus_master_set_poll_interval(modbus_t *ctx, uint32_t interval_ms) {
    if (ctx && interval_ms > 0) ctx->poll_interval = interval_ms;
}

void modbus_master_set_min_frame_gap(modbus_t *ctx, uint32_t gap_ms) {
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (a) a->min_frame_gap_ms = gap_ms;
}

int modbus_master_pending_jobs(modbus_t *ctx) {
    mb_arbiter_t *a = mb_get_arbiter(ctx);
    if (!a) return -1;
    int n = 0;
    for (int i = 0; i < MB_WRITE_JOB_MAX; i++) if (a->writes[i].in_use) n++;
    for (int i = 0; i < MB_READ_ONCE_MAX; i++) if (a->reads[i].in_use) n++;
    for (int i = 0; i < MB_POLL_REG_MAX; i++) if (a->regs[i].in_use) n++;
    for (int i = 0; i < MB_POLL_COIL_MAX; i++) if (a->coils[i].in_use) n++;
    return n;
}
