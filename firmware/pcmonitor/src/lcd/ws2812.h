/**
 ****************************************************************************************
 * @file ws2812.h
 * @brief WS2812 单线 LED 驱动 (GPIO 位翻转)
 ****************************************************************************************
 */
#ifndef _WS2812_H_
#define _WS2812_H_

#include <stdint.h>
#include "gpio.h"

/** 初始化某个引脚为 WS2812 数据线 (port: 0/1/2) */
void ws2812_init(int port, GPIO_PORT gp, GPIO_PIN pin);

/** 发送数据: data 为 GRB 顺序, 每颗灯 3 字节, n = 灯珠数量 */
void ws2812_send(int port, GPIO_PIN pin, const uint8_t *data, int n);

/** 熄灭 2 颗灯 */
void ws2812_off(int port, GPIO_PIN pin);

/** 直接把引脚拉高/拉低 (用于探测普通 LED) */
void ws2812_pin(int port, GPIO_PIN pin, int level);

/** 调整位时序 (高电平 NOP 个数), 用于兼容不同批次芯片 */
void ws2812_set_timing(uint8_t t0h, uint8_t t1h);

#endif /* _WS2812_H_ */
