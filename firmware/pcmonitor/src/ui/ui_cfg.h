/**
 ****************************************************************************************
 * @file ui_cfg.h
 * @brief 界面 / LED 运行配置 (由上位机通过 BLE 0xC0~0xC7 修改, 可保存到 SPI Flash)
 ****************************************************************************************
 */
#ifndef _UI_CFG_H_
#define _UI_CFG_H_

#include <stdint.h>

/* ---- 显示字段位 (0xC0) ---- */
#define FIELD_CLOCK     (1u << 0)
#define FIELD_CPU       (1u << 1)
#define FIELD_GPU       (1u << 2)
#define FIELD_RAM       (1u << 3)
#define FIELD_DISK      (1u << 4)
#define FIELD_NETRATE   (1u << 5)
#define FIELD_NETNAME   (1u << 6)
#define FIELD_HOST      (1u << 7)
#define FIELD_CPUTEMP   (1u << 8)
#define FIELD_GPUTEMP   (1u << 9)
#define FIELD_ALL       0x03FFu

/* ---- 调色板索引 (0xC1) ---- */
#define PAL_BG          0
#define PAL_TEXT        1
#define PAL_DIM         2
#define PAL_LINE        3
#define PAL_BAR_LO      4
#define PAL_BAR_MID     5
#define PAL_BAR_HI      6
#define PAL_TIME        7
#define PAL_ACCENT      8
#define PAL_N           9

/* ---- LED 颜色位 (0xC2/0xC3/0xC4/0xC5) ---- */
#define LEDC_OFF        0
#define LEDC_R          1
#define LEDC_G          2
#define LEDC_B          4

/* ---- 卡片 (0xC8) ---- */
#define CARD_MONITOR    0      /* 监控面板 (默认) */
#define CARD_CLOCK      1      /* 纯时钟 (固件原生绘制) */
#define CARD_WEATHER    2      /* 天气 (上位机渲染成图推送) */
#define CARD_TODO       3      /* TODO (上位机渲染成图推送) */
#define CARD_GALLERY    4      /* 画册 (上位机渲染成图推送) */
#define CARD_CALENDAR   5      /* 万年历 (固件离线绘制, 公历/农历/节气/节日/月历) */
#define CARD_N          6

/* 独立运行模式: 设备上电就是万年历, 不依赖电脑常开。
 * 1 = 锁定万年历卡片、不显示"主机未连接"、不生成模拟的 PC 监控数据;
 *     BLE 仍保持可连接, 只用于 0x91 对时。 */
#define STANDALONE_CALENDAR  1

#define CFG_SLOTS       4      /* 自定义灯效预设槽 */
#define CFG_VERSION     4      /* 4: 独立万年历模式, 使旧版 Flash 配置失效 */
#define CFG_MAGIC       0x314D4350u   /* "PCM1" */

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t field_mask;          /* FIELD_* 位或 */
    uint16_t pal[PAL_N];          /* RGB565 */
    uint8_t  led_en;              /* 0 = 关闭全部 RGB LED */
    uint8_t  overheat_temp;       /* 触发温度 ℃ */
    uint8_t  overheat_color;      /* LEDC_* 位或 */
    uint8_t  overheat_times;
    uint16_t overheat_cool;       /* 冷却秒 */
    uint8_t  hourly_en;
    uint8_t  hourly_color;
    uint8_t  hourly_times;
    uint8_t  card;                /* CARD_* 当前卡片 */
    uint8_t  preset[CFG_SLOTS][3];/* 颜色, 次数, 周期(100ms) */
    uint16_t checksum;            /* 前面所有字节之和 */
} cfg_t;

extern cfg_t g_cfg;

/** 恢复出厂默认配置 (不写 Flash) */
void cfg_defaults(void);

/** 应用配置: 重绘界面 + 让 LED 规则生效 */
void cfg_apply(void);

/** 从 SPI Flash 读取配置, 返回 1 = 读取成功, 0 = 使用默认值 */
int cfg_load(void);

/** 把当前配置写入 SPI Flash, 返回 1 = 成功 */
int cfg_save(void);

#endif /* _UI_CFG_H_ */
