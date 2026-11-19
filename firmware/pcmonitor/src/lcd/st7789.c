/**
 ****************************************************************************************
 * @file st7789.c
 * @brief ST7789 240x320 IPS 软件 SPI 驱动
 ****************************************************************************************
 */

#include "user_config.h"
#include "st7789.h"

/* 面板版本 (编译时选择, 默认侧灯版):
 *   0 = 侧灯版:   MADCTL=0x60, 需要 INVON (本屏反显)
 *   1 = 中间灯版: 左右镜像 (MADCTL=0x20), 颜色正常 (INVOFF)
 * 编译: PANEL_MIDDLE=1 python build.py ... */
#ifndef PANEL_MIDDLE
#define PANEL_MIDDLE    0
#endif

/* ---------------------------------------------------------------- 寄存器直写 */
#define REG16(a)        (*(volatile uint16_t *)(a))
#define GPIO_DATA(p)    REG16(0x50003000u + ((unsigned)(p) << 5))
#define GPIO_SET(p)     REG16(0x50003002u + ((unsigned)(p) << 5))
#define GPIO_RESET(p)   REG16(0x50003004u + ((unsigned)(p) << 5))

#define P0_SET      GPIO_SET(0)
#define P0_RESET    GPIO_RESET(0)
#define P1_SET      GPIO_SET(1)
#define P1_RESET    GPIO_RESET(1)
#define P2_SET      GPIO_SET(2)
#define P2_RESET    GPIO_RESET(2)

#define SCK_MASK    (1u << 1)   /* P0_1 */
#define DC_MASK     (1u << 7)   /* P0_7 */
#define SDA_MASK    (1u << 0)   /* P2_0 */
#define CS_MASK     (1u << 1)   /* P2_1 */
#define RST_MASK    (1u << 0)   /* P1_0 = 排线座 U5 第10脚 nRST (原厂 pinout P10=U5.10 nRST) */

/* 面板上电/复位: 原厂固件上电后把 P2_3 / P2_5 / P2_9 / P0_2 全部拉高,
 * 且初始化时先全部拉低 30ms 再拉高 (保证无论哪个脚接的是 PWR_EN / RESX 都能得到复位沿)。
 * 实测: 只打 P2_3 时面板不显示(黑屏但有背光), 必须保留这四个脚一起打。 */
#define PWR_MASK    ((1u << 3) | (1u << 5) | (1u << 9))   /* P2_3, P2_5, P2_9 */
#define PWR0_MASK   (1u << 2)                             /* P0_2 */

/* ---------------------------------------------------------------- 基础工具 */
static void lcd_delay_ms(int ms)
{
    volatile int i;
    while (ms-- > 0)
    {
        for (i = 0; i < 5000; i++)
        {
            __asm volatile ("nop");
        }
    }
}

static inline void sck_high(void) { P0_SET = SCK_MASK; }
static inline void sck_low(void)  { P0_RESET = SCK_MASK; }
static inline void cs_low(void)   { P2_RESET = CS_MASK; }
static inline void cs_high(void)  { P2_SET = CS_MASK; }
static inline void dc_cmd(void)   { P0_RESET = DC_MASK; }
static inline void dc_data(void)  { P0_SET = DC_MASK; }

/* MSB first, 原厂 0x07fc8370 的逐位时序: 置数据 -> SCK 高 -> SCK 低 */
static inline void bb_write(uint8_t b)
{
    int i;
    for (i = 7; i >= 0; i--)
    {
        if (b & (1u << i))
        {
            P2_SET = SDA_MASK;
        }
        else
        {
            P2_RESET = SDA_MASK;
        }
        sck_high();
        sck_low();
    }
}

static void lcd_cmd(uint8_t c)
{
    dc_cmd();
    cs_low();
    bb_write(c);
    cs_high();
}

static void lcd_data8(uint8_t d)
{
    dc_data();
    cs_low();
    bb_write(d);
    cs_high();
}

/* ---------------------------------------------------------------- 引脚配置 */
static void pin_init(void)
{
    /* 复用脚: P0_0/P0_3/P0_5/P0_6 是外部 Flash 硬件 SPI, 不要去动 */
    GPIO_ConfigurePin(GPIO_PORT_0, GPIO_PIN_1, OUTPUT, PID_GPIO, 0);   /* SCK  */
    GPIO_ConfigurePin(GPIO_PORT_2, GPIO_PIN_0, OUTPUT, PID_GPIO, 0);   /* SDA  */
    GPIO_ConfigurePin(GPIO_PORT_2, GPIO_PIN_1, OUTPUT, PID_GPIO, 1);   /* CS   */
    GPIO_ConfigurePin(GPIO_PORT_0, GPIO_PIN_7, OUTPUT, PID_GPIO, 0);   /* DC   */
    GPIO_ConfigurePin(GPIO_PORT_1, GPIO_PIN_0, OUTPUT, PID_GPIO, 1);   /* RST  */

    GPIO_ConfigurePin(GPIO_PORT_2, GPIO_PIN_3, OUTPUT, PID_GPIO, 1);
    GPIO_ConfigurePin(GPIO_PORT_0, GPIO_PIN_2, OUTPUT, PID_GPIO, 1);
    GPIO_ConfigurePin(GPIO_PORT_2, GPIO_PIN_5, OUTPUT, PID_GPIO, 1);
    GPIO_ConfigurePin(GPIO_PORT_2, GPIO_PIN_9, OUTPUT, PID_GPIO, 1);
}

/* ---------------------------------------------------------------- 上电/复位 */
static void panel_power_on(void)
{
    /* 1) 电源脚 + nRST 一起拉低 */
    P2_RESET = PWR_MASK;
    P0_RESET = PWR0_MASK;
    P1_RESET = RST_MASK;
    lcd_delay_ms(30);

    /* 2) 先给电, 让面板电源稳定 */
    P2_SET = PWR_MASK;
    P0_SET = PWR0_MASK;
    lcd_delay_ms(10);

    /* 3) 释放 nRST 产生上升沿; ST7789 复位后至少要等 120ms */
    P1_SET = RST_MASK;
    lcd_delay_ms(120);
}

/* ---------------------------------------------------------------- 窗口/数据 */
static void lcd_set_window(int x0, int y0, int x1, int y1)
{
    lcd_cmd(0x2A);                      /* CASET */
    lcd_data8((uint8_t)(x0 >> 8)); lcd_data8((uint8_t)x0);
    lcd_data8((uint8_t)(x1 >> 8)); lcd_data8((uint8_t)x1);

    lcd_cmd(0x2B);                      /* RASET */
    lcd_data8((uint8_t)(y0 >> 8)); lcd_data8((uint8_t)y0);
    lcd_data8((uint8_t)(y1 >> 8)); lcd_data8((uint8_t)y1);

    lcd_cmd(0x2C);                      /* RAMWR */
}

void st7789_set_madctl(uint8_t madctl)
{
    lcd_cmd(0x36);
    lcd_data8(madctl);
}

void st7789_start_write(int x0, int y0, int x1, int y1)
{
    lcd_set_window(x0, y0, x1, y1);
    dc_data();
    cs_low();
}

void st7789_push_pixels(const uint16_t *px, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        uint16_t v = px[i];
        bb_write((uint8_t)(v >> 8));
        bb_write((uint8_t)(v & 0xFF));
    }
}

void st7789_end_write(void)
{
    cs_high();
}

void st7789_fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    int n;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_W - 1) x1 = LCD_W - 1;
    if (y1 > LCD_H - 1) y1 = LCD_H - 1;
    if (x0 > x1 || y0 > y1) return;

    n = (x1 - x0 + 1) * (y1 - y0 + 1);

    lcd_set_window(x0, y0, x1, y1);
    dc_data();
    cs_low();

    while (n-- > 0)
    {
        bb_write((uint8_t)(color >> 8));
        bb_write((uint8_t)(color & 0xFF));
    }
    cs_high();
}

void st7789_fill(uint16_t color)
{
    st7789_fill_rect(0, 0, LCD_W - 1, LCD_H - 1, color);
}

/* ---------------------------------------------------------------- 初始化 */
void st7789_init(void)
{
    pin_init();
    panel_power_on();

    lcd_cmd(0x01);              /* SWRESET (补充, 原厂依赖上电复位) */
    lcd_delay_ms(150);

    lcd_cmd(0x11);              /* SLPOUT */
    lcd_delay_ms(150);

    lcd_cmd(0x36);              /* MADCTL */
#if PANEL_MIDDLE
    lcd_data8(0x28);            /* 中间灯版: MV | BGR -> 横屏 + 左右镜像 + 红蓝交换 */
#else
    lcd_data8(0x60);            /* 侧灯版: MX | MV -> 320x240 横屏(正向, RGB 顺序) */
#endif

    lcd_cmd(0x3A);              /* COLMOD */
    lcd_data8(0x55);            /* 16bit RGB565 */

#if PANEL_MIDDLE
    lcd_cmd(0x20);              /* 中间灯版: INVOFF, 颜色正常 */
#else
    lcd_cmd(0x21);              /* 侧灯版: INVON, 本屏需反显否则白底 */
#endif

    lcd_cmd(0x29);              /* DISPON */
    lcd_delay_ms(50);
}

/* ---------------------------------------------------------------- 自检画面 */
void st7789_selftest(void)
{
    /* 四角色块 + 中间十字, 用于确认引脚/方向/偏移 */
    st7789_fill(LCD_BLACK);

    st7789_fill_rect(0, 0, 39, 39, LCD_RED);                    /* 左上 红 */
    st7789_fill_rect(LCD_W - 40, 0, LCD_W - 1, 39, LCD_GREEN);  /* 右上 绿 */
    st7789_fill_rect(0, LCD_H - 40, 39, LCD_H - 1, LCD_BLUE);   /* 左下 蓝 */
    st7789_fill_rect(LCD_W - 40, LCD_H - 40, LCD_W - 1, LCD_H - 1, LCD_WHITE);

    st7789_fill_rect(LCD_W / 2 - 2, 0, LCD_W / 2 + 2, LCD_H - 1, LCD_YELLOW);
    st7789_fill_rect(0, LCD_H / 2 - 2, LCD_W - 1, LCD_H / 2 + 2, LCD_YELLOW);

    /* 顶部 8 段色带, 确认颜色顺序 */
    {
        static const uint16_t bars[8] = {
            LCD_WHITE, LCD_YELLOW, LCD_CYAN, LCD_GREEN,
            LCD_MAGENTA, LCD_RED, LCD_BLUE, LCD_BLACK
        };
        int i;
        int w = LCD_W / 8;
        for (i = 0; i < 8; i++)
        {
            st7789_fill_rect(i * w, 40, (i + 1) * w - 1, 79, bars[i]);
        }
    }
}
