/**
 ****************************************************************************************
 * @file ws2812.c
 * @brief WS2812/WS2812B 单线 LED 驱动 (GPIO 位翻转, 800kHz)
 *
 * DA14585 主频 16MHz, 一位 1.25µs ≈ 20 个周期:
 *   T0H ≈ 0.35µs   T0L ≈ 0.90µs
 *   T1H ≈ 0.70µs   T1L ≈ 0.55µs
 * 发送期间关中断, 否则时序被打断会导致颜色错乱。
 ****************************************************************************************
 */

#include "user_config.h"
#include "ws2812.h"

#define REG16(a)        (*(volatile uint16_t *)(a))

/* GPIO 端口寄存器: 每个端口步进 0x20 字节 (DATA / SET / RESET 各 16 位) */
#define P0_SET          REG16(0x50003002u)
#define P0_RESET        REG16(0x50003004u)
#define P1_SET          REG16(0x50003022u)
#define P1_RESET        REG16(0x50003024u)
#define P2_SET          REG16(0x50003042u)
#define P2_RESET        REG16(0x50003044u)

static inline void ws_set(int port, uint16_t mask)
{
    if (port == 0)      P0_SET = mask;
    else if (port == 1) P1_SET = mask;
    else                P2_SET = mask;
}

static inline void ws_clr(int port, uint16_t mask)
{
    if (port == 0)      P0_RESET = mask;
    else if (port == 1) P1_RESET = mask;
    else                P2_RESET = mask;
}

void ws2812_init(int port, GPIO_PORT gp, GPIO_PIN pin)
{
    ws_clr(port, (uint16_t)(1u << pin));         /* 先拉低, 避免上电闪一下 */
    GPIO_ConfigurePin(gp, pin, OUTPUT, PID_GPIO, 0);
}

/* 可调时序: 高电平 NOP 数 (每位总长约 20 周期), 用于扫描不同芯片的容差 */
static uint8_t ws_t0h = 1;
static uint8_t ws_t1h = 5;

void ws2812_set_timing(uint8_t t0h, uint8_t t1h)
{
    ws_t0h = t0h;
    ws_t1h = t1h;
}

/* 发送一个字节 (MSB first) */
static void ws_byte(int port, uint16_t mask, uint8_t b)
{
    int i, k;
    for (i = 7; i >= 0; i--)
    {
        if (b & (1u << i))
        {
            ws_set(port, mask);
            for (k = 0; k < ws_t1h; k++) __NOP();
            ws_clr(port, mask);
            for (k = ws_t1h; k < 12; k++) __NOP();
        }
        else
        {
            ws_set(port, mask);
            for (k = 0; k < ws_t0h; k++) __NOP();
            ws_clr(port, mask);
            for (k = ws_t0h; k < 12; k++) __NOP();
        }
    }
}

/* data 按 GRB 顺序, 每颗灯 3 字节 (G,R,B), n = 灯珠数量 */
void ws2812_send(int port, GPIO_PIN pin, const uint8_t *data, int n)
{
    /* 注意: GPIO_PIN_n 是引脚序号(0..9), 不是位掩码, 这里要左移 */
    uint16_t mask = (uint16_t)(1u << pin);
    int i;

    GLOBAL_INT_DISABLE();
    for (i = 0; i < n * 3; i++) ws_byte(port, mask, data[i]);
    GLOBAL_INT_RESTORE();

    /* 复位信号 >50µs */
    {
        volatile int d = 400;
        while (d-- > 0) { }
    }
}

void ws2812_off(int port, GPIO_PIN pin)
{
    static const uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
    ws2812_send(port, pin, zero, 2);
}

/* 直接把引脚拉高/拉低: 只写 DATA 寄存器, 不重配引脚(避免影响复用脚)
 * 注意: GPIO_PIN_n 是引脚序号(0..9), 不是位掩码 */
void ws2812_pin(int port, GPIO_PIN pin, int level)
{
    uint16_t mask = (uint16_t)(1u << pin);
    if (level) ws_set(port, mask);
    else       ws_clr(port, mask);
}
