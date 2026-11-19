/**
 ****************************************************************************************
 * @file st7789.h
 * @brief ST7789 240x320 IPS 屏驱动 (BMS-Screen / DA14585)
 *
 * 引脚由反汇编原厂 OTA 固件确认（fw_orig_ram.bin, 0x07fc77d4 板级初始化表）:
 *   LCD_SCK  = P0_1   (软件模拟, 原厂 0x07fc8370 逐位翻转)
 *   LCD_SDA  = P2_0   (软件模拟)
 *   LCD_CS   = P2_1   (低有效)
 *   LCD_DC   = P0_7   (1=数据, 0=命令)
 *   电源/复位/背光使能 = P2_3 / P0_2 / P2_5 / P2_9 (原厂上电后全部拉高)
 *
 * 原厂初始化序列 (0x07fc7b54): 0x11 -> 120ms -> 0x36=0x28 -> 0x3A=0x55 -> 0x20 -> 0x29
 * MADCTL=0x28 表示 MV|BGR, 即 320(宽) x 240(高) 横向显示。
 ****************************************************************************************
 */
#ifndef _ST7789_H_
#define _ST7789_H_

#include <stdint.h>

#define LCD_W 320
#define LCD_H 240

/* RGB565 */
#define LCD_BLACK   0x0000
#define LCD_WHITE   0xFFFF
#define LCD_RED     0xF800
#define LCD_GREEN   0x07E0
#define LCD_BLUE    0x001F
#define LCD_YELLOW  0xFFE0
#define LCD_CYAN    0x07FF
#define LCD_MAGENTA 0xF81F
#define LCD_GRAY    0x8410
#define LCD_ORANGE  0xFD20

/** 初始化 GPIO + 面板上电/复位 + ST7789 初始化序列 */
void st7789_init(void);

/** 全屏填充 */
void st7789_fill(uint16_t color);

/** 矩形填充 (含端点) */
void st7789_fill_rect(int x0, int y0, int x1, int y1, uint16_t color);

/** 设置 MADCTL (0x36): 方向/镜像/颜色顺序 */
void st7789_set_madctl(uint8_t madctl);

/** 打开绘图窗口并进入 RAMWR, 之后用 st7789_push_pixels 连续送像素 */
void st7789_start_write(int x0, int y0, int x1, int y1);

/** 连续送 n 个 RGB565 像素 (需先 st7789_start_write) */
void st7789_push_pixels(const uint16_t *px, int n);

/** 结束一次连续写入 (拉高 CS) */
void st7789_end_write(void);

/** 上电自检画面: 四角标记 + 色带, 用于确认引脚/方向/偏移 */
void st7789_selftest(void);

#endif /* _ST7789_H_ */
