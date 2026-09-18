# oled12864 — SSD1306 OLED 驱动（128×64，I2C）

> 版本：V1.1 ｜ 依赖：`chip.oop_i2c` ｜ 类别：display
> 板无关：I2C 引脚由 `OLED_I2C_Init()` 注入；字模由用户按 `oled_font_t` 注入。驱动**不内置任何字库**，零 HAL 直调。

---

## 一、功能边界

| 类别 | 函数 | 说明 |
|---|---|---|
| 总线/初始化 | `OLED_I2C_Init` / `OLED_12832_Init` / `OLED_12864_Init` / `OLED_ON` / `OLED_OFF` | 绑定 I2C 引脚、初始化 SSD1306 |
| 缓冲/刷新 | `OLED_Fill` / `OLED_CLS` / `OLED_Refresh` / `OLED_RefreshDiff` | 操作 `oled_page_buf[8][128]` 页缓冲，统一发屏 |
| 像素原语 | `OLED_SetPixel` | 写页缓冲的某一点（on/off） |
| 字模注册 | `OLED_RegisterFont` | 注入用户字模 `oled_font_t` |
| 文本/位图 | `OLED_DrawChar` / `OLED_DrawString` / `OLED_DrawBitmap` | 自包含渲染，直接描到页缓冲 |

**设计哲学（与 `led_matrix` 一致）**：驱动提供「像素原语 + 字模注册 + 文本/位图渲染」，字符大小、字符类型、阴阳由字模描述符携带；**字模字节数据由用户自带**（项目资产，不入 SDK）。不使用任何统一 gfx 中间层 —— 各显示驱动独立，链路就是「用户 → 设备 → 执行」。

---

## 二、字模格式（`oled_font_t` 即用户填入的“指定格式”）

```c
typedef struct {
    const uint8_t *table;       /* 字模首地址（asc2_1608 / zhcn_2020 ...） */
    uint8_t  cell_w, cell_h;    /* 单字宽高（"字符大小"由这里定义，不写死函数名） */
    uint8_t  bytes_per_row;     /* 每行字节数 = ceil(cell_w/8) */
    uint8_t  first, last;       /* 首/末字符编码（ASCII） */
    oled_font_enc_t enc;        /* OLED_FONT_ASCII / OLED_FONT_GBK */
    uint32_t (*offset_of)(uint8_t hi, uint8_t lo); /* GBK 查表（可选） */
} oled_font_t;
```

**字模字节排布约定（务必对齐，否则显示错乱）**：
- 逐行（row-major），从上到下；
- 每一行 `bytes_per_row` 字节，**MSB 在前**（字节 bit7 = 该行最左像素，bit0 = 最右像素）；
- `bytes_per_row = ceil(cell_w / 8)`，例如 8×16 → 1 字节/行，16×16 → 2 字节/行；
- 单字总字节数 = `cell_h * bytes_per_row`；
- ASCII 字形按编码连续排列（偏移 = `(c - first) * cell_h * bytes_per_row`）；中文 GBK 由 `offset_of(hi, lo)` 自行查表。

---

## 三、用法（任务中）

```c
/* 1) 用户自带字模（示例：8×16 ASCII，数据布局需符合上面"字模字节排布约定"） */
extern const uint8_t asc2_1608[95][16];

/* 2) 按 oled_font_t 描述并注册 */
oled_font_t f_ascii = {
    .table = (const uint8_t *)asc2_1608,
    .cell_w = 8, .cell_h = 16, .bytes_per_row = 1,
    .first = ' ', .last = '~',
    .enc = OLED_FONT_ASCII, .offset_of = NULL
};
OLED_RegisterFont(0, &f_ascii);

/* 3) 初始化显示后绘制 */
OLED_I2C_Init(scl_port, scl_pin, sda_port, sda_pin, delay_us);
OLED_12864_Init();
OLED_CLS();

OLED_DrawString(0, 0, "Hello", 0, 0);     /* inverse=0 正常 */
OLED_DrawString(0, 16, "INV",    0, 1);     /* inverse=1 阴阳反显 */
OLED_DrawBitmap(64, 0, w, h, my_bmp, 0);

OLED_Refresh();   /* 一次性推送，省 I2C 流量 */
```

`OLED_DrawChar` 画单字、`OLED_DrawBitmap` 画 1-bit 位图（w×h，行字节数 = `ceil(w/8)`，MSB 在前）；`inverse` 参数实现阴阳反显。

---

## 四、避坑点

1. **刷新时机**：`OLED_DrawString/Char/Bitmap` 只改 `oled_page_buf`，**不立即发屏**；画完一批后必须调 `OLED_Refresh()`（全量）或 `OLED_RefreshDiff()`（差异）推送。这与 `OLED_Fill/CLS` 立即写屏不同，文档需注意。
2. **阴阳（inverse）语义**：采用"叠加"语义——只点亮字形对应的点，不清除背景。在已清屏/已知背景上语义确定；不要在已有内容之上依赖 inverse 来"擦除"旧字。
3. **GBK 编码安全**：中文用 `offset_of(hi, lo)` 显式查表，**不要用中文字面量当 key**（见 Font 模块 GB2312 字面量坑）。驱动本身不依赖任何源码字符集。
4. **字模数据放用户工程**：`asc2_*` / `zhcn_*` 等是项目资产，留在应用层，通过 `oled_font_t` 注入；不要试图塞进驱动层（会让每个 pull 该模块的工程背上用不到的字库，也违反板无关红线）。
5. **F103 裸机可用**：本驱动只用 `chip.oop_i2c`（软 I2C），不依赖 RTOS；初始化时序里的 `oop_DelayMS` 基于 `oop_dwt`（Cortex-M3/M4 有 DWT，F103 属 M3，裸机可用）。若落到 M0+（F0/G0）无 DWT，需把 `delay` 改成注入回调——当前版本未做该注入，F103/M3/M4 安全。
6. **I2C 地址**：默认 `0x78`（可通过 0Ω 电阻改为 `0x7A`），如需改地址直接改 `oled_driver.h` 的 `OLED_ADDRESS`。

---

## 五、与旧 bsp_i2c 版本

取代旧 `Hardware/OLED12864`（依赖 `bsp_i2c` 与具体引脚）。新驱动坐 `chip.oop_i2c` 软总线，引脚注入、板无关、可单测。
