/**
 * @file    spi_nor_flash.h
 * @brief   通用 SPI NOR Flash 驱动（W25Q / GD25Q / BY25Q 等同族器件）
 * @version V1.0
 * @date    2026-09-18
 *
 * 定位：**板外器件**（devices 层）。坐 chip.oop_spi 之上，CS/WP 走 chip.oop_gpio，
 *       全程不直调 HAL_SPI_* —— HAL 只在 chip 层出现。
 *
 * 用法：
 *   1. 工程侧用 CubeMX 配好 SPI（全双工主机）、CS 与 WP 引脚；
 *   2. `oop_spi_dev_init(&s_spi, &hspi2)` + `oop_gpio_init_output(&s_cs, CS_PORT, CS_PIN, false)`
 *      （CS 低有效 → active_high 传 false，之后 `oop_gpio_write(&s_cs, true)` 即选中）；
 *   3. `spi_nor_create(&s_nor, &cfg)` 完成探测（JEDEC ID + 容量）；
 *   4. 之后 read/write/erase 都是**分单位阻塞**的：每写完一页、擦完一个扇区/块都会
 *      轮询 WIP 至完成。长操作靠 `spi_nor_set_idle_cb()` 注入的空闲回调喂看门狗，
 *      回调返回 false 可中止当前操作。
 *
 * 为什么不做异步状态机：SPI NOR 的单个单位操作本身就很短（页写 ~0.7 ms、
 * 4 KB 扇区擦 ~45 ms、64 KB 块擦 ~150 ms），而一个完整 OTA 镜像要写上千页 ——
 * 真正的问题不是「单次阻塞」，而是「一次调用阻塞太久」。因此这里刻意把粒度
 * 做成「单位操作阻塞 + 空闲回调」，由上层决定一次喂多少字节，天然满足喂狗需求，
 * 也避免为一块只要几十行的器件驱动引入一整套状态机。
 *
 * 未实现（如需要请先补，别在工程侧另开一份）：
 *   - SFDP 探测（容量按 JEDEC 容量码推导，个别非标器件用 cfg.size 兜底）
 *   - QSPI / 双线四线模式（本驱动只用单线 SPI）
 *   - 4 字节地址的 EN4B 自动下达（`cfg.addr_bytes = 4` 时假定器件已处于 4 字节模式）
 *   - 安全寄存器 / OTP / 保护位设置
 */

#ifndef __SPI_NOR_FLASH_H
#define __SPI_NOR_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include "oop_spi.h"        /* oop_spi_dev_t（chip 层 SPI 封装） */
#include "oop_gpio_drv.h"   /* gpio_dev_t（CS / WP） */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 返回码（负数为错误） */
typedef enum {
    SPI_NOR_OK           =  0,
    SPI_NOR_ERR_PARAM    = -1,   /*!< 空指针 / 参数非法 */
    SPI_NOR_ERR_RANGE    = -2,   /*!< 越出器件容量，或地址未按单位对齐 */
    SPI_NOR_ERR_IO       = -3,   /*!< SPI 传输失败（总线/接线问题） */
    SPI_NOR_ERR_TIMEOUT  = -4,   /*!< WIP 轮询超时（器件没响应或已损坏） */
    SPI_NOR_ERR_ABORT    = -5,   /*!< 空闲回调要求中止 */
    SPI_NOR_ERR_WEL      = -6,   /*!< 写使能位未置起（器件被保护 / 总线不可信） */
    SPI_NOR_ERR_MISMATCH = -7,   /*!< 读回比对不一致 */
} spi_nor_ret_t;

/** @brief JEDEC ID（命令 0x9F 读回 3 字节） */
typedef struct {
    uint8_t  mfr;            /*!< 厂商：0xEF=Winbond、0xC8=GigaDevice、0x68=Boya、0x1C=EON */
    uint8_t  mem_type;       /*!< 存储类型 */
    uint8_t  capacity_code;  /*!< 容量码：0x17 = 2^23 = 8 MB、0x18 = 16 MB …… */
    uint32_t size;           /*!< 由容量码推导的容量；代码为 0 时取 cfg.size */
} spi_nor_id_t;

/**
 * @brief 空闲回调：在每个 WIP 等待循环里被反复调用
 * @param user  注册时传入的上下文
 * @return true = 继续等待；false = 请求中止当前长操作（函数返回 SPI_NOR_ERR_ABORT）
 * @note  典型用途：喂看门狗、更新进度、响应取消。**不得阻塞**。
 */
typedef bool (*spi_nor_idle_cb_t)(void *user);

/** @brief 创建参数（全部由调用方注入，本模块不引用任何工程全局） */
typedef struct {
    oop_spi_dev_t *spi;          /*!< 必填：已 oop_spi_dev_init 的 SPI 实例 */
    gpio_dev_t    *cs;           /*!< 必填：片选（active_high=false 的低有效脚） */
    gpio_dev_t    *wp;           /*!< 可选：写保护脚（NULL 表示硬件直接接地/不控） */
    uint32_t       size;         /*!< 预期容量（字节）；0 = 由 JEDEC 容量码推导 */
    bool           fast_read;    /*!< true = 0x0B(FAST_READ)+8 dummy（快，推荐）；
                                      false = 0x03(READ)（慢，但老器件兼容性最好） */
    uint8_t        addr_bytes;   /*!< 寻址字节数：3（<=16 MB，默认）或 4（>16 MB） */
    uint32_t       timeout_ms;   /*!< 单个单位操作的 WIP 超时（0 = 默认 2000 ms） */
    uint32_t       page_size;    /*!< 页大小（0 = 默认 256） */
    uint32_t       sector_size;  /*!< 扇区大小（0 = 默认 4096） */
    uint32_t       block_size;   /*!< 块大小（0 = 默认 65536） */
} spi_nor_cfg_t;

/** @brief 器件实例（由调用方持有，可多片并存） */
typedef struct {
    oop_spi_dev_t    *spi;
    gpio_dev_t       *cs;
    gpio_dev_t       *wp;
    spi_nor_id_t      id;
    uint32_t          size;
    uint32_t          page_size;
    uint32_t          sector_size;
    uint32_t          block_size;
    uint32_t          timeout_ms;
    bool              fast_read;
    uint8_t           addr_bytes;
    bool              ready;
    spi_nor_idle_cb_t idle_cb;
    void             *idle_user;
} spi_nor_dev_t;

/* ---------------- 生命周期 ---------------- */

/**
 * @brief  创建/探测：注入总线与片选，读 JEDEC ID，确定容量
 * @return SPI_NOR_OK；SPI_NOR_ERR_IO（总线不通）；SPI_NOR_ERR_PARAM（cfg 缺 SPI/CS）
 * @note   ⚠ 请先 `oop_gpio_init_output(cs, port, pin, false)` 把 CS 配好并把电平拉到
 *         非选中态，本函数不会去配置引脚（引脚真相属于工程侧）。
 */
int spi_nor_create(spi_nor_dev_t *dev, const spi_nor_cfg_t *cfg);

/**
 * @brief  重新读一次 JEDEC ID（自检 / 热插拔后重新确认）
 */
int spi_nor_read_id(spi_nor_dev_t *dev, spi_nor_id_t *out);

/** @brief 注册空闲回调（喂狗 / 进度 / 取消） */
void spi_nor_set_idle_cb(spi_nor_dev_t *dev, spi_nor_idle_cb_t cb, void *user);

/* ---------------- 查询 ---------------- */

/** @brief 容量（字节）；未成功 create 返回 0 */
uint32_t spi_nor_size(const spi_nor_dev_t *dev);

/** @brief 轮询一次 WIP（忙=false）；顺带返回总线是否可用 */
bool spi_nor_is_busy(spi_nor_dev_t *dev);

/** @brief 读状态寄存器 1（bit0=WIP、bit1=WEL、其它为保护位） */
int spi_nor_read_status(spi_nor_dev_t *dev, uint8_t *sr1);

/** @brief 是否已擦除（全 0xFF） */
int spi_nor_is_erased(spi_nor_dev_t *dev, uint32_t addr, uint32_t len);

/* ---------------- 读写擦 ---------------- */

/** @brief 读（单条命令顺序读，长度无上限，内部按 4 KB 分块推进） */
int spi_nor_read(spi_nor_dev_t *dev, uint32_t addr, void *buf, uint32_t len);

/**
 * @brief 写（内部自动按页切分，每页 WREN -> PAGE_PROGRAM -> 等 WIP）
 * @note  目标必须已擦除。**一次调用别传太大**：每页约 0.7 ms，上层建议每次 ≤ 4 KB，
 *        在调用之间喂狗/刷新进度；或用 idle_cb 代劳。
 */
int spi_nor_write(spi_nor_dev_t *dev, uint32_t addr, const void *buf, uint32_t len);

/** @brief 擦除一个扇区（4 KB，地址须扇区对齐） */
int spi_nor_erase_sector(spi_nor_dev_t *dev, uint32_t addr);

/** @brief 擦除一个块（64 KB，地址须块对齐） */
int spi_nor_erase_block(spi_nor_dev_t *dev, uint32_t addr);

/** @brief 整片擦除（耗时可达数十秒；依赖 idle_cb 喂狗，且可按返回 false 中止） */
int spi_nor_erase_chip(spi_nor_dev_t *dev);

/**
 * @brief  擦除 [addr, addr+len) 覆盖到的所有单位
 * @note   自动向两侧扩展到单位边界（可能擦掉比要求更多的内容）；
 *         能对齐到 64 KB 就用块擦，否则退化为 4 KB 扇区擦。
 */
int spi_nor_erase(spi_nor_dev_t *dev, uint32_t addr, uint32_t len);

/** @brief 读回比对（首个不一致的地址经 bad_addr 输出） */
int spi_nor_verify(spi_nor_dev_t *dev, uint32_t addr, const void *buf, uint32_t len,
                   uint32_t *bad_addr);

/* ---------------- 低层命令（给需要自己拼时序的调用方） ---------------- */

/** @brief 写使能（WREN + 校验 WEL） */
int spi_nor_write_enable(spi_nor_dev_t *dev);

/** @brief 等待 WIP 清零（超时 dev->timeout_ms，期间调用 idle_cb） */
int spi_nor_wait_ready(spi_nor_dev_t *dev);

/** @brief 进入深睡眠（功耗最低，但仍可被命令唤醒） */
int spi_nor_deep_power_down(spi_nor_dev_t *dev);

/** @brief 释放深睡眠（0xAB） */
int spi_nor_release_power_down(spi_nor_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* __SPI_NOR_FLASH_H */
