/**
 ****************************************************************************************
 * @file ui_cfg.c
 * @brief 界面 / LED 运行配置
 ****************************************************************************************
 */
#include <string.h>

#include "user_config.h"
#include "ui_cfg.h"
#include "monitor_ui.h"

cfg_t g_cfg;

/* 存储接口 (epd/spi_flash.c) */
int flash_cfg_read(void *buf, int len);
int flash_cfg_write(const void *buf, int len);

static uint16_t cfg_checksum(const cfg_t *c)
{
    const uint8_t *p = (const uint8_t *)c;
    unsigned len = sizeof(cfg_t) - sizeof(c->checksum);
    unsigned i;
    uint16_t s = 0;

    for (i = 0; i < len; i++)
        s = (uint16_t)(s + p[i]);
    return s;
}

void cfg_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    g_cfg.magic      = CFG_MAGIC;
    g_cfg.version    = CFG_VERSION;
    g_cfg.field_mask = FIELD_ALL;

    /* 与旧版硬编码配色一致 */
    g_cfg.pal[PAL_BG]      = 0x0000;
    g_cfg.pal[PAL_TEXT]    = 0xFFFF;
    g_cfg.pal[PAL_DIM]     = 0x8410;
    g_cfg.pal[PAL_LINE]    = 0x2124;
    g_cfg.pal[PAL_BAR_LO]  = 0x07E0;
    g_cfg.pal[PAL_BAR_MID] = 0xFFE0;
    g_cfg.pal[PAL_BAR_HI]  = 0xF800;
    g_cfg.pal[PAL_TIME]    = 0xFFFF;
    g_cfg.pal[PAL_ACCENT]  = 0x07FF;

    /* 与旧版 LED 行为一致 */
    g_cfg.led_en           = 1;
    g_cfg.overheat_temp    = 95;
    g_cfg.overheat_color   = LEDC_R;
    g_cfg.overheat_times   = 3;
    g_cfg.overheat_cool    = 600;
    g_cfg.hourly_en        = 1;
    g_cfg.hourly_color     = LEDC_B;
    g_cfg.hourly_times     = 1;

    /* 默认显示万年历 (可再用 0xC8 切回监控面板 0) */
    g_cfg.card             = CARD_CALENDAR;

    /* 预设 0: 白光闪 2 次, 0.5 秒一步 */
    g_cfg.preset[0][0] = LEDC_R | LEDC_G | LEDC_B;
    g_cfg.preset[0][1] = 2;
    g_cfg.preset[0][2] = 5;

    g_cfg.checksum = cfg_checksum(&g_cfg);
}

void cfg_apply(void)
{
    g_cfg.magic    = CFG_MAGIC;
    g_cfg.version  = CFG_VERSION;
    g_cfg.checksum = cfg_checksum(&g_cfg);
    ui_repaint_all();
}

int cfg_load(void)
{
    cfg_t tmp;
    int ok;

    ok = flash_cfg_read(&tmp, sizeof(tmp));
    if (ok && tmp.magic == CFG_MAGIC && tmp.version == CFG_VERSION &&
        tmp.checksum == cfg_checksum(&tmp))
    {
        memcpy(&g_cfg, &tmp, sizeof(g_cfg));
#if STANDALONE_CALENDAR
        /* 旧配置里可能存了监控/时钟/画册卡片, 独立模式下强制回到万年历 */
        g_cfg.card = CARD_CALENDAR;
#endif
        return 1;
    }

    cfg_defaults();
    return 0;
}

int cfg_save(void)
{
    g_cfg.magic    = CFG_MAGIC;
    g_cfg.version  = CFG_VERSION;
    g_cfg.checksum = cfg_checksum(&g_cfg);
    return flash_cfg_write(&g_cfg, sizeof(g_cfg));
}
