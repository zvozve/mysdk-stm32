# LED Matrix 驱动（ICN2027 + TC7258 + SM245，64x64 RGB）

板外 RGB LED 点阵屏驱动，坐 chip 层 `oop_gpio` / `oop_dwt` 之上。板无关：所有控制引脚由
`LED_Matrix_Init()` 注入，不引用任何 CubeMX / HAL 符号。自带字模注册 + 文本/位图渲染，用户填字模即可开箱用。

## 硬件
- ICN2027：列/颜色移位驱动（R1/G1/B1/R2/G2/B2 + SCLK + LAT + OE）
- TC7258：行译码（A~E 五线选 32 行，上下各 32 = 64 行）
- SM245：输出使能 OE

## 引脚注入（board_cfg.h 提供）
把工程实际的 GPIO 端口/引脚填进 `led_matrix_pins_t`，传 `LED_Matrix_Init(&pins)`。
参考工程原接线（仅示例，不写死）：
SM245_OE=F0/F11，ICN2027_LAT=G0，ICN2027_SCLK=F7，ICN2027_OE=F8，
R1/G1/B1=F1/F12/F2，R2/G2/B2=F3/F13/F4，TC7258 A~E=F5/F14/F6/F15/G1。

## 用法
```c
// 1) 在工程里填自己的字模（按 lm_font_t 格式）
const lm_font_t f_ascii = { asc2_1608, 8, 16, 1, ' ', '~', LM_FONT_ASCII, NULL };
LED_Matrix_RegisterFont(0, &f_ascii);

// 2) 初始化（引脚注入）
LED_Matrix_Init(&pins);

// 3) 在任务里画（Clear 为"整屏填充该色"，需先填充背景色再画前景）
LED_Matrix_Clear(LM_WHITE);                 // 整屏填白底
LED_Matrix_DrawString(0, 0, "Hello", 0, LM_RED);
LED_Matrix_DrawBitmap(0, 32, 64, 32, my_bmp, LM_GREEN);
LED_Matrix_Refresh();                       // 阻塞整屏扫描一遍（测试用）
// 或周期性：定时器每 LM_SCAN_LINE_US 调一次 LED_Matrix_ScanTick()（无闪屏）
```

`lm_color_t`：LM_WHITE / LM_RED / LM_GREEN / LM_BLUE / LM_YELLOW。
中文（GBK）：`encoding=LM_FONT_GBK` 且提供 `offset_of(hi,lo)` 回调指到字模偏移即可。

## 坑位
- OE 低有效：SM245 OE 初始化即常使能；ICN2027 OE 在逐行切换时翻转（关输出→换行使能），否则串影。
- 刷新率 ≈ 32 行 × `LM_SCAN_LINE_US`；520us≈60Hz。用 `ScanTick` 周期驱动最稳，单次 `Refresh` 会阻塞。
- 帧缓冲占用：颜色 64×64=4KB + 帧 64×8=512B，合计约 4.6KB RAM，注意栈/全局区。
- 该模块自包含、不依赖外部 gfx；oled12864 维持各自独立（各做各的）。
