/**
 * @file    ota_flow.h
 * @brief   流程骨架：取数 → 写目标区 → 双重校验 → 提交（BL 与 APP 共用）
 * @version V1.0
 * @date    2026-09-18
 *
 * **非阻塞步进式**：调用方反复调 `ota_flow_step()`，每次只做一小块（读一块 /
 * 擦一个擦除单位 / 校验一块），单次耗时有上限。这样：
 *   - 448 KB 镜像的长擦除被切成多步，步与步之间应用可以喂狗、刷进度、响应取消；
 *   - 不需要为「长操作」写一整套异步状态机；
 *   - 与 SDK 既有 `xxx_process()` 惯例一致。
 *
 * 状态序列：
 *   IDLE → OPEN → HDR → DECIDE → ERASE → WRITE → VERIFY_MEM → VERIFY_FLASH → COMMIT → DONE
 *                                              ▲                ▲
 *                                        第 2 关（内存 CRC）  第 3 关（读回 Flash 重算 CRC）
 *
 * 源格式（自动识别，无需调用方指定）：
 *   · .otapkg（magic=OTAP）：走原有 80 字节包头 + 段表解析，第 2/3 关比对包头段表里的 CRC32。
 *   · 裸 bin（任意非 OTAP 开头）：整段即镜像，长度取源声明的 total_size，从偏移 0 起。
 *     若源在 open 时给了外部期望 CRC32（`info.expect_crc32 != 0`，如 UART 的「元数据优先」），
 *     第 2/3 关都按它校验（能发现 PC 端源文件本身损坏）；否则退回「内存 CRC == 闪存 CRC」
 *     自校（只证明「收到的 == 落盘的」，不能发现源文件损坏）。
 *
 * 第 2 关与第 3 关的区别是最容易漏的设计点：增量 CRC 只证明「收到的报文对」，
 * 读回 CRC 才证明「落到 Flash 的字节对」。两关都要，别省。
 *
 * 缓冲：搬运缓冲由**调用方提供**（`cfg->buf`），本模块不动态分配、也不占大栈。
 */

#ifndef __OTA_FLOW_H
#define __OTA_FLOW_H

#include <stdint.h>
#include "ota_common.h"
#include "ota_area.h"
#include "ota_cfg.h"
#include "ota_image.h"
#include "ota_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 流程状态 */
typedef enum {
    OTA_FLOW_IDLE         = 0,
    OTA_FLOW_OPEN         = 1,   /*!< 打开源、取流信息 */
    OTA_FLOW_HDR          = 2,   /*!< 读 80 字节（裸 bin 时为镜像头），识别 otapkg / 裸 bin */
    OTA_FLOW_DECIDE       = 3,   /*!< 选目标区 + 找段 + 定位到段数据 */
    OTA_FLOW_ERASE        = 4,   /*!< 擦目标区（每步一个擦除单位） */
    OTA_FLOW_WRITE        = 5,   /*!< 收数并写入（每步一块） */
    OTA_FLOW_VERIFY_MEM   = 6,   /*!< 第 2 关：内存累计 CRC + 长度 */
    OTA_FLOW_VERIFY_FLASH = 7,   /*!< 第 3 关：读回重算 CRC */
    OTA_FLOW_COMMIT       = 8,   /*!< 写状态区（置 pending_action / trial） */
    OTA_FLOW_DONE         = 9,
    OTA_FLOW_FAILED       = 10,
} ota_flow_state_t;

/** @brief 单步返回 */
typedef enum {
    OTA_FLOW_BUSY = 0,   /*!< 还没完，继续调 step */
    OTA_FLOW_OK   = 1,   /*!< 全部完成（可在 COMMIT 后复位生效） */
    OTA_FLOW_ERR  = -1,  /*!< 出错，看 ota_flow_t.err */
} ota_flow_ret_t;

/* 前置声明：实例结构里要存 cfg，而 cfg 的回调又要拿到实例指针，故先声明类型 */
typedef struct ota_flow_s ota_flow_t;

/**
 * @brief 进度/状态上报（可 NULL）
 * @note  在**每个 step 之后**调用，是喂看门狗、刷进度、上报 MQTT 的落点。
 *        想中止就不要再调 step（或直接调 ota_flow_abort()），回调本身不承担取消语义。
 */
typedef void (*ota_flow_report_cb)(ota_flow_t *f, void *user);

/** @brief 流程配置 */
typedef struct {
    ota_source_t  *source;       /*!< 必填：取数后端 */
    uint8_t       *buf;          /*!< 必填：调用方提供的搬运缓冲 */
    uint32_t       buf_len;      /*!< 建议 4096；不得小于 OTA_PKG_HDR_SIZE */
    uint8_t        running_slot; /*!< 当前运行的槽（仅 A/B 拓扑用得上） */
    ota_flow_report_cb report;   /*!< 进度上报，可 NULL */
    void          *user;         /*!< 传给 report 的上下文 */
} ota_flow_cfg_t;

/** @brief 流程实例（调用方持有；含包头/段/校验状态，故不要放栈上临时变量） */
struct ota_flow_s {
    /* ---- 对外可见 ---- */
    ota_flow_state_t state;
    ota_ret_t        err;           /*!< 失败原因 */
    ota_act_t        act;           /*!< SWITCH / MOVE */
    uint8_t          target_slot;   /*!< SWITCH：目标槽号 */
    uint8_t          target_area;   /*!< 运行区在表里的下标 */
    uint8_t          stage_area;    /*!< MOVE：暂存区在表里的下标 */
    uint32_t         total;         /*!< 待写字节数 */
    uint32_t         written;       /*!< 已写字节数 */
    uint32_t         erased;        /*!< 已擦字节数 */
    uint32_t         verified;      /*!< 已回读字节数 */
    uint32_t         crc_mem;       /*!< 第 2 关结果 */
    uint32_t         crc_flash;     /*!< 第 3 关结果 */
    uint32_t         fw_ver;        /*!< 镜像版本 */
    uint32_t         bytes_in;      /*!< 从源读入的总字节数（含包头/跳过） */

    /* ---- 私有 ---- */
    const ota_flow_cfg_t *cfg;
    ota_pkg_hdr_t          hdr;
    ota_seg_t              seg;
    uint32_t               data_off;      /*!< 段数据在包内偏移（裸 bin 时为 0） */
    uint8_t                raw;          /*!< 1 = 源是裸 bin（无 .otapkg 头），整段即镜像 */
    uint8_t                pending[OTA_PKG_HDR_SIZE]; /*!< 裸 bin：do_hdr 多读的镜像头部，须原样写回 */
    uint32_t               pending_len;
    uint32_t               pending_off;
    uint32_t               src_total;     /*!< 源声明的总字节数（open 时从 info.total_size 取得） */
    uint32_t               expect_crc32;  /*!< 外部期望 CRC32（0 = 无）；裸 bin 有它则按它校验 */
    const ota_flash_t     *dst_flash;     /*!< 下载落脚点介质 */
    uint32_t               dst_off;       /*!< 落脚点在介质内的起始偏移 */
    uint32_t               erase_end;     /*!< 擦除终点（介质内偏移） */
    uint32_t               erase_cur;     /*!< 擦除游标 */
    ota_target_t           tgt;
    ota_verifier_t         v_mem;
    ota_crc32_state_t      st_mem;
    ota_verifier_t         v_flash;
    ota_crc32_state_t      st_flash;
};

/**
 * @brief  启动流程
 * @return OTA_OK / OTA_ERR_PARAM
 * @note   需先 ota_init()；不会立刻读数据，第一次工作发生在首个 ota_flow_step()。
 */
int ota_flow_start(ota_flow_t *f, const ota_flow_cfg_t *cfg);

/**
 * @brief  推进一步（每次调用耗时受 cfg->buf_len 与一个擦除单位限制）
 * @return OTA_FLOW_BUSY / OTA_FLOW_OK / OTA_FLOW_ERR
 */
ota_flow_ret_t ota_flow_step(ota_flow_t *f);

/** @brief 进度 0~100（写阶段占 0~70%，回读校验占 70~100%） */
uint8_t ota_flow_progress(const ota_flow_t *f);

/** @brief 中止（关闭源、置 FAILED） */
int ota_flow_abort(ota_flow_t *f);

/** @brief 状态名（日志用） */
const char *ota_flow_state_name(ota_flow_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_FLOW_H */
