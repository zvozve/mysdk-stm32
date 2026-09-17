/* ============================================================
 * delta_dop_107.c - Delta DOP-107 系列 HMI 屏 波形显示驱动（功能封装层）
 *
 * 只负责"按屏规则把波形/参考线/控制指令写到 HMI"，不含任何业务判定。
 *
 * ★ 核心约定（2026-08-18 起，统一总线仲裁器）：
 *   本驱动不再自行检查 state==IDLE / 直接发包，所有写出都通过
 *   modbus_master_*_async 入队（高优先级一次性写作业，FIFO），
 *   由仲裁器在总线空闲时逐帧发出；完成/失败由 done 回调通知，
 *   驱动在 done 里推进自身状态机（清 pending / 发下一帧）。
 *   —— 与读轮询不再抢线：写作业天然优先于周期读。
 *
 * SDK 版（迁入 devices/delta_dop_107）：
 *   - bsp_dwt.h -> chip/oop_dwt.h（bsp_GetCycleCount -> oop_GetCycleCount，
 *     bsp_IsTimeout -> oop_IsTimeout）
 *   - HMI 寄存器地址仍来自用户配置头 hmi_modbus_addr.h（工程提供，随 include path
 *     可见），与 board_cfg.h 同理；RTT 标签 HMI_LOG 定义在本驱动头 delta_dop_107.h
 *     内（通用头 SEGGER_RTT_Log.h 只保留 RTTSYS/ERR/WARN/INFO/DBG/HEX）。
 * ============================================================ */

#include "delta_dop_107.h"
#include "modbus_master.h"
#include "oop_dwt.h"            /* oop_GetCycleCount / oop_IsTimeout：诊断心跳节流 */
#include "SEGGER_RTT_Log.h"

#define DOP107_FLUSH_CHUNKS   ((DOP107_WAVE_LEN + DOP107_WAVE_CHUNK - 1) / DOP107_WAVE_CHUNK)  /* 3 */
#define DOP107_LINE_ALL_MASK  0x0Fu
#define DOP107_WAVE_CH_N      2   /* A(0)/B(1) 两路 */

static modbus_t *g_master = NULL;

/* ---- pending 状态（Poll 入队；done 里清） ---- */
static uint16_t g_pending_ctrl   = 0;     /* 0=无；1=刷新；256=清除 */
static int8_t   g_pending_marker = -1;    /* -1=无；0/1=线圈4 */
static uint8_t  g_pending_lines  = 0;     /* 待写参考线掩码 */
static uint16_t g_line_mv[DOP107_LINE_N] = {0};   /* 各参考线当前显示值（重发用） */

/* ---- 参考线在途管理：pending（待写）→ inflight（等 done）---- */
static uint8_t  g_line_inflight = 0;      /* 在途掩码（防止同一线重复入队/丢更新） */
static uint16_t g_line_pair[DOP107_LINE_N][2];   /* 每线独立 {mv,mv} 缓冲，避免互相覆盖 */

/* ---- 屏控制在途值（done 里据它做 clear→refresh 补发） ---- */
static uint16_t g_ctrl_inflight = 0;

/* ---- 波形分帧状态机（done 驱动推进） ----
 * g_flush_stage[2][LEN] 双缓冲：一次 flush 请求时【同刻】抓 A(路0)/B(路1)
 * 快照，分帧期间 A/B 都引用各自缓冲——消除旧版"B 在 A 传完后才抓取"
 * 导致的 ~75ms 双通道错位（2026-08-19 用户拍板：双通道必须同步连续）。 */
static uint16_t g_flush_stage[2][DOP107_WAVE_LEN]; /* [0]=A 路, [1]=B 路 */
static int8_t   g_flush_ch     = -1;              /* -1=空闲；0/1=正在写出的通道 */
static uint8_t  g_flush_chunk  = 0;               /* 当前通道已发出的帧数 */
static bool     g_flush_req    = false;           /* 有待写请求 */
static bool     g_flush_busy   = false;           /* 当前帧在途（等 done） */
static bool     g_wave_enabled = false;           /* ADC 动作波形写出总开关 */
static uint32_t g_flush_retry_t0 = 0;             /* 入队失败退避起点（槽满/断线不每轮死等） */
#define DOP107_FLUSH_RETRY_US   100000            /* 入队失败后 100ms 再试 */

static dop107_wave_source_t g_wave_source = NULL;
static dop107_wave_flush_done_t g_flush_done_cb = NULL;
static dop107_wave_active_t  g_wave_active_cb = NULL;   /* NULL=全部上传；否则按回调判定掩码 */
static bool g_frame_dbg = false;   /* 帧级 RTT 调试打印（默认关） */

/* 参考线 序号 → 起始寄存器地址（每条占 2 个寄存器） */
static const uint16_t g_line_addr[DOP107_LINE_N] = {
    MB_REG_LINE_ACT_A, MB_REG_LINE_BNC_A, MB_REG_LINE_ACT_B, MB_REG_LINE_BNC_B,
};

/* ========== 参考线 done（按线号分发，避免闭包问题） ========== */
static void dop107_line_finish(uint8_t i) {
    g_line_inflight &= (uint8_t)~(1u << i);
    /* 波形正在分帧写出时（g_flush_ch>=0）不立即刷屏：否则参考线完成会提前
     * 写 W1002=1，屏重绘"半帧"（只有部分 chunk 已发），与未发出的 chunk 拼出
     * 断层（曲线1 在 W200 处直接断裂）。6 帧全部发完由 dop107_wave_frame_done
     * 统一写 W1002，整帧到位才重绘。线变更会在下一轮刷屏时一并生效，不会丢更新。 */
    if (g_flush_ch < 0) {
        g_pending_ctrl = MB_HMI_CTRL_REFRESH;
    }
}
static void dop107_line_done_0(int r) { (void)r; dop107_line_finish(0); }
static void dop107_line_done_1(int r) { (void)r; dop107_line_finish(1); }
static void dop107_line_done_2(int r) { (void)r; dop107_line_finish(2); }
static void dop107_line_done_3(int r) { (void)r; dop107_line_finish(3); }
static const mb_job_done_t g_line_done_tab[DOP107_LINE_N] = {
    dop107_line_done_0, dop107_line_done_1, dop107_line_done_2, dop107_line_done_3,
};

/* ========== 波形分帧 done（推进状态机） ========== */
static void dop107_wave_enqueue_next(void);
static int8_t dop107_next_active_ch(int8_t from);   /* 前向声明：供 frame_done/poll 使用 */
static void dop107_wave_frame_done(int result) {
    g_flush_busy = false;
    /* 发送失败（超时/异常/断线，result!=0）：不推进状态机，退避 100ms 后重试同一帧。
     * 曾因无条件推进导致失败的 chunk0 被跳过 → 屏上旧段与后续新段拼出 100 点断层；
     * 通道2(B)在 A 路 3 帧后才启动，总线已稳定，故从未命中该窗口。 */
    if (result != 0) {
        g_flush_retry_t0 = oop_GetCycleCount();
        if (g_frame_dbg) {
            HMI_LOG("wave frame ch=%u chunk=%u FAILED result=%d (retry same chunk)",
                    g_flush_ch, g_flush_chunk, result);
        }
        return;
    }
    if (++g_flush_chunk < DOP107_FLUSH_CHUNKS) {
        dop107_wave_enqueue_next();          /* 当前通道下一帧 */
        return;
    }
    /* 当前通道发完：切到下一个激活通道；无则收尾刷新。
     * 未接(掩码)通道经回调判定被跳过，不会给没接的一路空输出波形。 */
    int8_t nx = dop107_next_active_ch(g_flush_ch);
    if (nx >= 0) {
        g_flush_ch    = nx;
        g_flush_chunk = 0;
        dop107_wave_enqueue_next();
        return;
    }
    /* 两路都发完 → 收尾刷新 */
    g_flush_ch     = -1;
    g_flush_chunk  = 0;
    g_pending_ctrl = MB_HMI_CTRL_REFRESH;
    HMI_LOG("wave flush DONE (A:W%u..W%u B:W%u..W%u), screen refresh queued",
            MB_REG_WAVE_A, MB_REG_WAVE_A + DOP107_WAVE_LEN - 1,
            MB_REG_WAVE_B, MB_REG_WAVE_B + DOP107_WAVE_LEN - 1);
    if (g_flush_done_cb) g_flush_done_cb();
}

/* ========== 生命周期 ========== */

void DOP107_Init(void) {
    g_master          = NULL;
    g_pending_ctrl    = 0;
    g_pending_marker  = -1;
    g_pending_lines   = 0;
    g_line_inflight   = 0;
    g_ctrl_inflight   = 0;
    g_flush_ch        = -1;
    g_flush_chunk     = 0;
    g_flush_req       = false;
    g_flush_busy      = false;
    g_flush_retry_t0  = 0;
    g_wave_enabled    = false;
    g_wave_source     = NULL;
    for (uint8_t i = 0; i < DOP107_LINE_N; i++) g_line_mv[i] = 0;
}

void DOP107_AttachMaster(modbus_t *m) {
    g_master = m;
}

void DOP107_SetWaveSource(dop107_wave_source_t cb) {
    g_wave_source = cb;
}

void DOP107_SetWaveActiveCb(dop107_wave_active_t cb) {
    g_wave_active_cb = cb;
}

/* 返回 from 之后下一个应上传波形的通道；无则 -1（本轮 flush 完成）。
 * g_wave_active_cb==NULL 时所有通道都视为激活。 */
static int8_t dop107_next_active_ch(int8_t from) {
    for (int8_t c = (int8_t)(from + 1); c < DOP107_WAVE_CH_N; c++) {
        if (g_wave_active_cb == NULL || g_wave_active_cb((uint8_t)c)) return c;
    }
    return -1;
}

bool DOP107_IsLinkUp(void) {
    if (!g_master) return false;
    if (modbus_get_last_activity(g_master) == 0) return false;
    if (modbus_get_line_state(g_master) == MODBUS_LINE_DISCONNECTED) return false;
    return true;
}

void DOP107_SetWaveFlushDoneCb(dop107_wave_flush_done_t cb) {
    g_flush_done_cb = cb;
}

void DOP107_SetFrameDebug(bool en) {
    g_frame_dbg = en;
}

/* ========== 屏控制 / 错误标记（仅登记，Poll 入队） ========== */

void DOP107_ScreenRefresh(void) {
    g_pending_ctrl = MB_HMI_CTRL_REFRESH;
}

void DOP107_ScreenClear(void) {
    g_pending_ctrl = MB_HMI_CTRL_CLEAR;
}

void DOP107_SetErrMarker(bool on) {
    g_pending_marker = on ? 1 : 0;
}

static void dop107_ctrl_done(int result) {
    /* 清除后紧跟刷新一次，确保屏端重绘（仅当期间没有更新的控制请求） */
    if (result == 0 && g_ctrl_inflight == MB_HMI_CTRL_CLEAR && g_pending_ctrl == 0) {
        g_pending_ctrl = MB_HMI_CTRL_REFRESH;
    }
}

/* ========== 参考线（屏规则：每条 2 寄存器 {mv,mv}） ========== */

void DOP107_WriteLine(uint8_t line_id, uint16_t mv) {
    if (line_id >= DOP107_LINE_N) return;
    g_line_mv[line_id] = mv;
    g_pending_lines |= (uint8_t)(1u << line_id);
}

/* 每次调用最多入队一条线；done 里清 inflight 位并设刷新（不丢新请求：
 * 若在途期间又有 WriteLine，pending 位会重新置起，done 后自动补发新值） */
static void dop107_lines_push(void) {
    if (!g_pending_lines || !g_master) return;
    /* 链路还没出现过有效应答时不发，避免初始化帧丢在空链路上 */
    if (modbus_get_last_activity(g_master) == 0) return;
    if (modbus_get_line_state(g_master) == MODBUS_LINE_DISCONNECTED) return;

    for (uint8_t i = 0; i < DOP107_LINE_N; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(g_pending_lines & bit)) continue;
        if (g_line_inflight & bit) continue;      /* 该线上一帧还在途 */

        g_line_pair[i][0] = g_line_mv[i];
        g_line_pair[i][1] = g_line_mv[i];         /* 2 端点同一阈值 → 水平参考线 */
        int ret = modbus_master_write_regs_async(g_master, g_line_addr[i], 2,
                                                 g_line_pair[i], g_line_done_tab[i]);
        if (ret == 0) {
            g_pending_lines &= (uint8_t)~bit;
            g_line_inflight |= bit;
            HMI_LOG("ref line W%u,W%u = %u enqueued",
                    g_line_addr[i], g_line_addr[i] + 1, g_line_mv[i]);
        }
        return;   /* 一次只入队一条，等 done 后再入队下一条 */
    }
}

/* ========== 动作波形刷新（分帧排队） ========== */

void DOP107_WaveEnable(bool en) {
    g_wave_enabled = en;
    HMI_LOG("ADC wave write %s", en ? "ENABLED" : "DISABLED");
}

/* 刷新波形：总开关关闭时不登记（释放总线给读轮询） */
void DOP107_WaveFlush(void) {
    if (!g_wave_enabled) return;
    g_flush_req = true;
}

/* 入队当前帧；done 回调推进状态机。失败（槽满/断线）则退避 100ms 后下轮重试同一帧 */
static void dop107_wave_enqueue_next(void) {
    if (!g_master || g_flush_ch < 0 || g_flush_busy) return;
    /* 入队失败退避：断线/槽满时不每轮死等刷日志（曾出现 enqueue ret=-2 刷屏+队列冻结） */
    if (g_flush_retry_t0 && !oop_IsTimeout(g_flush_retry_t0, DOP107_FLUSH_RETRY_US)) return;

    uint16_t base = (g_flush_ch == 0) ? MB_REG_WAVE_A : MB_REG_WAVE_B;
    uint16_t off  = (uint16_t)(g_flush_chunk * DOP107_WAVE_CHUNK);
    uint16_t n    = (uint16_t)(DOP107_WAVE_LEN - off);
    if (n > DOP107_WAVE_CHUNK) n = DOP107_WAVE_CHUNK;

    int wr = modbus_master_write_regs_async(g_master, (uint16_t)(base + off), n,
                                            &g_flush_stage[g_flush_ch][off], dop107_wave_frame_done);
    if (wr != 0) {
        /* -3=断线（等重连自愈）；其他=槽满等，退避后重试 */
        g_flush_retry_t0 = oop_GetCycleCount();
        if (g_frame_dbg) {
            HMI_LOG("wave frame ch=%u chunk=%u W%u..W%u x%u enqueue ret=%d (will retry)",
                    g_flush_ch, g_flush_chunk,
                    (unsigned)(base + off), (unsigned)(base + off + n - 1), n, wr);
        }
        return;
    }
    g_flush_retry_t0 = 0;
    g_flush_busy = true;
    HMI_LOG("wave frame ch=%u chunk=%u W%u..W%u x%u (first=%u last=%u) TX",
            g_flush_ch, g_flush_chunk,
            (unsigned)(base + off), (unsigned)(base + off + n - 1), n,
            g_flush_stage[g_flush_ch][off], g_flush_stage[g_flush_ch][off + n - 1]);
}

static void dop107_wave_flush_poll(void) {
    if (!g_master || !g_wave_enabled) return;

    if (g_flush_ch < 0) {                 /* 本轮空闲：看有没有新请求 */
        if (!g_flush_req) return;
        g_flush_req   = false;
        /* 从第一个激活通道开始；无激活通道(都掩码) → 不抓不写，本轮跳过 */
        g_flush_ch = dop107_next_active_ch(-1);
        if (g_flush_ch < 0) {
            HMI_LOG("wave flush SKIP: no active channel (all masked)");
            return;
        }
        g_flush_chunk = 0;
        /* ★ 双缓冲同刻抓取：仅对激活通道取快照，两路时间基准一致 */
        if (g_wave_source) {
            for (int8_t c = 0; c < DOP107_WAVE_CH_N; c++) {
                if (g_wave_active_cb == NULL || g_wave_active_cb((uint8_t)c))
                    g_wave_source((uint8_t)c, g_flush_stage[c], DOP107_WAVE_LEN);
            }
        }
        HMI_LOG("wave flush START ch=%d (A: W%u..W%u)",
                g_flush_ch, MB_REG_WAVE_A, MB_REG_WAVE_A + DOP107_WAVE_LEN - 1);
    }

    /* 链路还没活不急着发 */
    if (modbus_get_last_activity(g_master) == 0) return;
    if (modbus_get_line_state(g_master) == MODBUS_LINE_DISCONNECTED) return;

    dop107_wave_enqueue_next();
}

/* ========== 周期轮询：登记 → 入队（仲裁器统一调度发送） ========== */

void DOP107_Poll(void) {
    if (!g_master) return;

    /* 诊断心跳（每 ~500ms）：链路状态 / 队列占用 / 波形进度。
     * line=1 超时、2 断线；jobs=待发+在途作业总数（=12 说明队列冻结）。 */
    {
        static uint32_t hb_t0 = 0;
        if (oop_IsTimeout(hb_t0, 500000)) {
            hb_t0 = oop_GetCycleCount();
            HMI_LOG("poll hb: line=%d st=%d jobs=%d wave_en=%d flush_req=%d ch=%d chunk=%d "
                    "lines=0x%X inflight=0x%X ctrl=%u marker=%d",
                    (int)modbus_get_line_state(g_master),
                    (int)modbus_get_state(g_master),
                    modbus_master_pending_jobs(g_master),
                    g_wave_enabled, g_flush_req,
                    g_flush_ch, g_flush_chunk, g_pending_lines,
                    g_line_inflight, g_pending_ctrl, g_pending_marker);
        }
    }

    /* 屏控制（高优，先入队） */
    if (g_pending_ctrl) {
        g_ctrl_inflight = g_pending_ctrl;
        if (modbus_master_write_reg_async(g_master, MB_REG_HMI_CTRL,
                                          g_pending_ctrl, dop107_ctrl_done) == 0) {
            g_pending_ctrl = 0;
        }
    }

    /* 错误标记线圈 */
    if (g_pending_marker != -1) {
        if (modbus_master_write_coil_async(g_master, MB_COIL_ERR_MARK,
                                           g_pending_marker != 0, NULL) == 0) {
            g_pending_marker = -1;
        }
    }

    /* 参考线优先于波形：设值一改尽快上屏 */
    dop107_lines_push();

    /* 动作波形分帧写出（默认屏蔽，见 DOP107_WaveEnable） */
    if (g_wave_enabled) {
        dop107_wave_flush_poll();
    }
}
