/**
 ****************************************************************************************
 * @file monitor_ui.c
 * @brief PC 资源监控界面 (ST7789 320x240, 黑底, 无帧缓冲流式绘制)
 *
 * 面板有一道竖向漏液条纹, 用全屏刻度尺实测位于逻辑坐标 x100..110。
 * 因此 UI 分成左右两块, 中间 x96..116 保持全黑, 不绘制任何内容:
 *
 *   左区 x  4.. 92   NET 下载/上传速率 + C 盘占用 + 蓝牙状态
 *   黑带 x 96..116   (漏液带保护区, 全黑)
 *   右区 x120..316   时间 + CPU + RAM + 主机名
 *
 * 字体: 由 tools/gen_font.py 用 Consolas 离线生成的 4bit alpha 抗锯齿字库,
 *       绘制时按 alpha 与黑底混合, 边缘平滑 (非点阵硬边)。
 * 刷新: 每秒只重画发生变化的字符 (逐字符差异) 与进度条的差量像素。
 ****************************************************************************************
 */

#include "user_config.h"
#include "st7789.h"
#include "monitor_ui.h"
#include "ui_cfg.h"
#include "font_aa.h"
#include "cal_font.h"
#include "host_offline.h"
#include "ws2812.h"
#include "app_easy_timer.h"

#include <string.h>

mon_stats_t g_mon;

/* 坐标标定模式: 1 = 只显示刻度尺(用于定位漏液带), 0 = 正常监控界面 */
#define UI_CALIB     0

/* LED 探测模式: 1 = 逐个点亮通道, 0 = 正常监控界面 (可用 -DUI_LED_TEST=1 覆盖) */
#ifndef UI_LED_TEST
#define UI_LED_TEST  0
#endif

/* 状态灯: 共阳 RGB, 三路都是"低电平点亮"(基极经三极管把阴极拉低)。
 * 引脚来自原厂 pinout 表 (docs/pinout.xlsx) + 万用表实测:
 *   红 = P0.2 (MCU 第 3 脚,  Q2)
 *   绿 = P2.5 (MCU 第 32 脚, Q1)
 *   蓝 = P2.9 (MCU 第 39 脚, Q4)   ← 原厂表标注 P29 = LED_Blue
 * 方向与初始电平在 st7789.c 的 pin_init() 里已配成输出高(灭)。 */
#define LED_R_PORT     0
#define LED_R_PIN      GPIO_PIN_2
#define LED_G_PORT     2
#define LED_G_PIN      GPIO_PIN_5
#define LED_B_PORT     2
#define LED_B_PIN      GPIO_PIN_9

/* ---------------------------------------------------------------- 配色
 * 全部走运行时可调色板 (上位机 0xC1 修改), 名称沿用旧常量便于阅读 */
#define C_BG        (g_cfg.pal[PAL_BG])
#define C_LINE      (g_cfg.pal[PAL_LINE])
#define C_BAR_BG    (g_cfg.pal[PAL_LINE])
#define C_WHITE     (g_cfg.pal[PAL_TEXT])
#define C_CYAN      (g_cfg.pal[PAL_ACCENT])
#define C_GREEN     (g_cfg.pal[PAL_BAR_LO])
#define C_YELLOW    (g_cfg.pal[PAL_BAR_MID])
#define C_RED       (g_cfg.pal[PAL_BAR_HI])
#define C_GRAY      (g_cfg.pal[PAL_DIM])

/* ---------------------------------------------------------------- 字库 */
typedef struct
{
    const char    *chars;
    const uint8_t *bits;
    uint8_t        adv;     /* 每字符步进宽度 */
    uint8_t        h;       /* 字形高度 */
    uint8_t        asc;     /* 基线以上的高度 */
} afont_t;

static const afont_t f_big  = { FONT_BIG_CHARS,  FONT_BIG_BITS,  FONT_BIG_ADV,  FONT_BIG_H,  FONT_BIG_ASC  };
static const afont_t f_med  = { FONT_MED_CHARS,  FONT_MED_BITS,  FONT_MED_ADV,  FONT_MED_H,  FONT_MED_ASC  };
static const afont_t f_tiny = { FONT_TINY_CHARS, FONT_TINY_BITS, FONT_TINY_ADV, FONT_TINY_H, FONT_TINY_ASC };

/* 分块扫描行缓冲: 16 行一块 (928 字节)。万年历常驻, 让出 RAM 给中文字库;
 * 53px 大字会分 4 批写入, 但大时钟每分钟才重画一次, 开销可忽略。 */
#define BLIT_ROWS   16
#define BLIT_MAX_W  29
static uint16_t gpx[BLIT_MAX_W * BLIT_ROWS];

/* 4bit alpha 位图 -> RGB565 像素块并送屏; 背景按黑色处理 */
static void blit_alpha(int x, int y, int w, int h, const uint8_t *bits, uint16_t fg)
{
    uint16_t ramp[16];
    int r, c, y0, rows, bpr = (w + 1) >> 1;
    uint16_t r5 = (uint16_t)((fg >> 11) & 0x1F);
    uint16_t g6 = (uint16_t)((fg >> 5) & 0x3F);
    uint16_t b5 = (uint16_t)(fg & 0x1F);

    if (w > BLIT_MAX_W) return;
    if (x < 0 || y < 0 || x + w > LCD_W || y + h > LCD_H) return;

    for (c = 0; c < 16; c++)
        ramp[c] = (uint16_t)((((r5 * c) / 15) << 11) | (((g6 * c) / 15) << 5) | ((b5 * c) / 15));

    for (y0 = 0; y0 < h; y0 += BLIT_ROWS)
    {
        rows = h - y0;
        if (rows > BLIT_ROWS) rows = BLIT_ROWS;

        for (r = 0; r < rows; r++)
        {
            const uint8_t *p = bits + (y0 + r) * bpr;
            uint16_t *o = gpx + r * w;
            for (c = 0; c < w; c++)
            {
                uint8_t v = p[c >> 1];
                uint8_t a = (c & 1) ? (uint8_t)(v & 0x0F) : (uint8_t)(v >> 4);
                o[c] = ramp[a];
            }
        }

        st7789_start_write(x, y + y0, x + w - 1, y + y0 + rows - 1);
        st7789_push_pixels(gpx, w * rows);
        st7789_end_write();
    }
}

static int glyph_index(const afont_t *f, char ch)
{
    int i;
    for (i = 0; f->chars[i]; i++)
        if (f->chars[i] == ch) return i;
    if (ch >= 'a' && ch <= 'z')                 /* 小写回退到大写 */
    {
        char up = (char)(ch - 32);
        for (i = 0; f->chars[i]; i++)
            if (f->chars[i] == up) return i;
    }
    return 0;                                   /* 未知字符 -> 空格 */
}

static void draw_glyph(const afont_t *f, int x, int y, char ch, uint16_t fg)
{
    int idx = glyph_index(f, ch);
    const uint8_t *bits = f->bits + (uint32_t)idx * (((f->adv + 1) >> 1) * f->h);
    blit_alpha(x, y, f->adv, f->h, bits, fg);
}

static int slen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void text_at(const afont_t *f, int x, int y, const char *s, uint16_t fg)
{
    while (*s)
    {
        draw_glyph(f, x, y, *s, fg);
        x += f->adv;
        s++;
    }
}

static void text_right(const afont_t *f, int right, int y, const char *s, uint16_t fg)
{
    text_at(f, right - slen(s) * f->adv + 1, y, s, fg);
}

static void text_center(const afont_t *f, int cx, int y, const char *s, uint16_t fg)
{
    text_at(f, cx - slen(s) * f->adv / 2, y, s, fg);
}

/* 擦除 n 个字符宽的区域 (背景黑) */
static void text_clear(const afont_t *f, int x, int y, int n)
{
    if (n > 0) st7789_fill_rect(x, y, x + n * f->adv - 1, y + f->h - 1, C_BG);
}

/* 逐字符差异重绘: 只重画内容变化的字符, 新串更短时用空格覆盖 */
static void text_diff(const afont_t *f, int x, int y, const char *s, const char *old, uint16_t fg)
{
    int i;
    for (i = 0; ; i++)
    {
        char nc = s[i];
        char oc = old[i];
        if (!nc && !oc) break;
        if (nc != oc) draw_glyph(f, x + i * f->adv, y, nc ? nc : ' ', fg);
    }
}

/* 右对齐差异重绘: 长度不变时逐字符比较, 长度变化则擦旧区再整串重画 */
static void text_right_diff(const afont_t *f, int right, int y, const char *s,
                            const char *old, uint16_t fg)
{
    int ns = slen(s);
    int no = slen(old);
    int nx = right - ns * f->adv + 1;
    int ox = right - no * f->adv + 1;

    if (nx == ox)
    {
        text_diff(f, nx, y, s, old, fg);
    }
    else
    {
        text_clear(f, ox, y, no);
        text_at(f, nx, y, s, fg);
    }
}

void ui_draw_text(int x, int y, const char *s, uint16_t fg)
{
    text_at(&f_med, x, y, s, fg);
}

void ui_draw_text_center(int cx, int y, const char *s, uint16_t fg)
{
    text_center(&f_med, cx, y, s, fg);
}

/* ---------------------------------------------------------------- 数字格式化 */
static int fmt_u(char *p, unsigned v)
{
    char t[8];
    int n = 0, i;
    if (v == 0) { p[0] = '0'; return 1; }
    while (v > 0 && n < 8) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < n; i++) p[i] = t[n - 1 - i];
    return n;
}

/* 百分比: "45%" */
static void fmt_pct(char *out, unsigned pct)
{
    int i = fmt_u(out, pct);
    out[i++] = '%';
    out[i] = 0;
}

/* 窄栏短速率: "1.2M/s" / "23M/s" / "850K/s" / "12B/s"
 * 左栏仅 89px(中号字 6 字符), 故两位数以上不再保留小数 */
static void fmt_rate_short(char *out, uint32_t bps)
{
    uint32_t v10;
    char unit, suf;
    int i = 0;
    unsigned ip, fp;

    if (bps >= 1000000UL)     { v10 = bps / 100000UL; unit = 'M'; }
    else if (bps >= 1000UL)   { v10 = bps / 100UL;    unit = 'K'; }
    else                      { v10 = bps * 10UL;     unit = 'B'; }

    if (v10 > 9999UL) v10 = 9999UL;
    ip = v10 / 10;
    fp = v10 % 10;

    i += fmt_u(out + i, ip);
    if (ip < 10)
    {
        out[i++] = '.';
        out[i++] = (char)('0' + fp);
    }
    out[i++] = unit;
    suf = (unit == 'B') ? 's' : '/';
    out[i++] = suf;
    if (unit != 'B') out[i++] = 's';
    out[i] = 0;

    /* 补齐到固定 6 字符, 让每行内容长度恒定 -> 右对齐时只逐字符差异重绘 */
    if (i == 5)
    {
        out[6] = 0;
        out[5] = out[4]; out[4] = out[3]; out[3] = out[2];
        out[2] = out[1]; out[1] = out[0]; out[0] = ' ';
    }
}

/* RAM 用量: "8.0/16.0GB" (MB -> GB 保留 1 位) */
static void fmt_ram(char *out, unsigned used_mb, unsigned total_mb)
{
    int i;
    i = fmt_u(out, used_mb / 1024);
    out[i++] = '.';
    out[i++] = (char)('0' + (used_mb % 1024) * 10 / 1024);
    out[i++] = '/';
    i += fmt_u(out + i, total_mb / 1024);
    out[i++] = '.';
    out[i++] = (char)('0' + (total_mb % 1024) * 10 / 1024);
    out[i++] = 'G'; out[i++] = 'B'; out[i] = 0;
}

static int str_diff(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a != *b);
}

/* ---------------------------------------------------------------- 布局
 * 刻度尺实测漏液带位于 x100..110, 两侧各留 2px 余量 -> 保护区 x98..112。
 */
#define LEFT_X0       4
#define LEFT_X1       96
#define RIGHT_X0      114
#define RIGHT_X1      316
#define RIGHT_CX      ((RIGHT_X0 + RIGHT_X1) / 2)

/* 左区: 网络信息与网速放在一起 (顶部第一行显示网络名/类型) */
#define NET_LBL_Y     4
#define DL_LBL_Y      24
#define DL_VAL_Y      40
#define UL_LBL_Y      72
#define UL_VAL_Y      88
#define DIV_L_Y       120
#define CK_LBL_Y      126
#define CK_VAL_Y      148
#define CK_BAR_Y      178
#define BT_ICON_X     4
#define BT_ICON_Y     206
#define HOST_X        22
#define HOST_Y        210

/* 右区: 真 52px 时钟 (HH:MM) + 26px 秒, 下面 CPU/GPU/RAM 三行 */
#define TIME_Y        2
#define DIV_R_Y       60
#define CPU_VAL_Y     68
#define CPU_LBL_Y     77
#define CPU_BAR_Y     98
#define GPU_VAL_Y     116
#define GPU_LBL_Y     125
#define GPU_BAR_Y     146
#define RAM_VAL_Y     164
#define RAM_LBL_Y     173
#define RAM_BAR_Y     194
#define RAM_USED_Y    206
#define TEMP_X        146

/* ---------------------------------------------------------------- 进度条 */
#define BAR_H         8
static const uint8_t bar_inset[BAR_H] = { 2, 1, 0, 0, 0, 0, 1, 2 };

typedef struct
{
    int x0, y0, x1;
    int last_pct;          /* -1 = 尚未绘制 */
    uint16_t last_color;
} bar_t;

static bar_t bar_cpu = { RIGHT_X0, CPU_BAR_Y, RIGHT_X1, -1, 0 };
static bar_t bar_gpu = { RIGHT_X0, GPU_BAR_Y, RIGHT_X1, -1, 0 };
static bar_t bar_ram = { RIGHT_X0, RAM_BAR_Y, RIGHT_X1, -1, 0 };
static bar_t bar_ck  = { LEFT_X0,  CK_BAR_Y,  LEFT_X1,  -1, 0 };

static uint16_t pct_color(int pct)
{
    if (pct < 60) return C_GREEN;
    if (pct < 85) return C_YELLOW;
    return C_RED;
}

/* 按行画 [xa..xb] 像素, 自动套用两端的圆角内缩 */
static void bar_paint(bar_t *b, int xa, int xb, uint16_t color)
{
    int r;
    for (r = 0; r < BAR_H; r++)
    {
        int lo = b->x0 + bar_inset[r];
        int hi = b->x1 - bar_inset[r];
        int a = (xa < lo) ? lo : xa;
        int z = (xb > hi) ? hi : xb;
        if (a <= z) st7789_fill_rect(a, b->y0 + r, z, b->y0 + r, color);
    }
}

static void bar_update(bar_t *b, int pct, uint16_t color)
{
    int w = b->x1 - b->x0 + 1;
    int neww, oldw;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    neww = w * pct / 100;
    if (b->last_pct < 0)          oldw = 0;
    else if (b->last_pct >= 100)  oldw = w;
    else                          oldw = w * b->last_pct / 100;

    if (b->last_pct < 0 || color != b->last_color)
    {
        bar_paint(b, b->x0, b->x1, C_BAR_BG);
        if (neww > 0) bar_paint(b, b->x0, b->x0 + neww - 1, color);
    }
    else if (neww > oldw)
    {
        bar_paint(b, b->x0 + oldw, b->x0 + neww - 1, color);
    }
    else if (neww < oldw)
    {
        bar_paint(b, b->x0 + neww, b->x0 + oldw - 1, C_BAR_BG);
    }
    else
    {
        return;
    }

    b->last_pct = pct;
    b->last_color = color;
}

/* ---------------------------------------------------------------- 蓝牙图标 */
static void draw_bt_icon_at(int x, int y, uint16_t color)
{
    blit_alpha(x, y, FONT_BT_W, FONT_BT_H, FONT_BT_BITS, color);
}

static void draw_bt_icon(uint16_t color)
{
    draw_bt_icon_at(BT_ICON_X, BT_ICON_Y, color);
}

/* ---------------------------------------------------------------- 主机名 */
#define HOST_MAX_CHARS 9      /* 与蝙蝠图标同行: x22..94 共 72px */

static char host_name[HOST_MAX_CHARS + 1] = "";
static uint8_t host_dirty;

void ui_set_host_name(const char *s)
{
    int i;

    for (i = 0; i < HOST_MAX_CHARS && s[i]; i++) host_name[i] = s[i];
    host_name[i] = 0;
    host_dirty = 1;                       /* 由 1Hz 刷新统一绘制, 不在蓝牙回调里刷屏 */
}

/* ---------------------------------------------------------------- 坐标标定 */
/* 全屏刻度尺: 深灰底 + 每 10px 白刻度 + 每 50px 黄刻度并标数字,
 * 青色竖线标出当前保留给漏液带的黑区 x168..192 */
static void ui_ruler(void)
{
    int x, v, i;
    char s[8];

    st7789_fill(0x18C3);                        /* 中灰底, 便于看清漏液条纹 */

    st7789_fill_rect(98, 0, 98, LCD_H - 1, C_CYAN);
    st7789_fill_rect(112, 0, 112, LCD_H - 1, C_CYAN);

    for (x = 0; x < LCD_W; x += 10)
    {
        int big = (x % 50) == 0;
        st7789_fill_rect(x, big ? 130 : 140, x, 150, big ? C_YELLOW : C_WHITE);
    }
    st7789_fill_rect(0, 151, LCD_W - 1, 151, C_WHITE);

    for (v = 0; v <= 300; v += 50)
    {
        i = fmt_u(s, (unsigned)v);
        s[i] = 0;
        x = v - i * f_tiny.adv / 2;
        if (x < 0) x = 0;
        if (x + i * f_tiny.adv > LCD_W) x = LCD_W - i * f_tiny.adv;
        text_at(&f_tiny, x, 104, s, C_WHITE);
    }

    text_at(&f_tiny, 4, 190, "X 0-319 / 10px", C_WHITE);
    text_at(&f_tiny, 4, 210, "CYAN = BAND 98-112", C_CYAN);
}

/* ---------------------------------------------------------------- 网络名称位图 */
/* PC 端把 "WIFI 梦创_5G" 这类含中文的名称渲染成 1bpp 位图分块下发,
 * 每行 (w+7)/8 字节, 逐行从上到下, 每字节高位在左 */
#define NET_BMP_W_MAX  96
#define NET_BMP_H      15
#define NET_BMP_MAX    (NET_BMP_W_MAX / 8 * NET_BMP_H)   /* 192 字节 */

static uint8_t  net_bmp[NET_BMP_MAX];
static uint8_t  net_bmp_w;
static uint8_t  net_bmp_len;
static uint8_t  net_bmp_dirty;
static uint8_t  net_bmp_ready;

void ui_net_bmp_chunk(uint8_t w, uint8_t total, uint8_t offset, const uint8_t *d, uint8_t n)
{
    uint8_t i;

    if (w == 0 || w > NET_BMP_W_MAX) return;
    if (total == 0 || total > NET_BMP_MAX) return;
    if ((int)offset + n > total) return;

    if (offset == 0)
    {
        net_bmp_w = w;
        net_bmp_len = total;
        for (i = 0; i < NET_BMP_MAX; i++) net_bmp[i] = 0;   /* 清掉上一帧残留 */
    }
    for (i = 0; i < n; i++) net_bmp[offset + i] = d[i];

    if ((int)offset + n >= total)
    {
        net_bmp_ready = 1;
        net_bmp_dirty = 1;
    }
}

/* 1bpp 位图 -> RGB565 (高位在左, 每行 (w+7)/8 字节); 背景按黑底填充 */
static void blit_bmp1(int x, int y, int w, int h, const uint8_t *bits, uint16_t fg)
{
    int rb = (w + 7) / 8;
    int rows, y0, r, c;

    if (w <= 0 || h <= 0 || w > LCD_W) return;

    rows = (BLIT_MAX_W * BLIT_ROWS) / w;
    if (rows < 1) rows = 1;

    for (y0 = 0; y0 < h; y0 += rows)
    {
        int n = h - y0;
        if (n > rows) n = rows;

        for (r = 0; r < n; r++)
        {
            uint16_t *o = gpx + r * w;
            for (c = 0; c < w; c++)
            {
                uint8_t b = bits[(y0 + r) * rb + (c >> 3)];
                o[c] = (b & (0x80 >> (c & 7))) ? fg : C_BG;
            }
        }

        st7789_start_write(x, y + y0, x + w - 1, y + y0 + n - 1);
        st7789_push_pixels(gpx, w * n);
        st7789_end_write();
    }
}

static void draw_net_bmp(void)
{
    if (!net_bmp_ready) return;

    st7789_fill_rect(LEFT_X0, NET_LBL_Y, LEFT_X0 + NET_BMP_W_MAX - 1,
                     NET_LBL_Y + NET_BMP_H - 1, C_BG);
    blit_bmp1(LEFT_X0, NET_LBL_Y, net_bmp_w, NET_BMP_H, net_bmp, C_WHITE);
}

/* 未连上主机时, 在网络名位置显示 "主机未连接" */
static void draw_offline_hint(void)
{
    st7789_fill_rect(LEFT_X0, NET_LBL_Y, LEFT_X0 + NET_BMP_W_MAX - 1,
                     NET_LBL_Y + NET_BMP_H - 1, C_BG);
    blit_bmp1(LEFT_X0, NET_LBL_Y + (NET_BMP_H - HOST_OFFLINE_H) / 2,
              HOST_OFFLINE_W, HOST_OFFLINE_H, HOST_OFFLINE_BITS, C_YELLOW);
}

/* ---------------------------------------------------------------- LED 探测 */
#if UI_LED_TEST

static void led_rgb(int r, int g, int b);   /* 定义在下方状态灯一节 */

typedef struct
{
    int       port;
    GPIO_PORT gp;
    GPIO_PIN  pin;
    const char *name;
} ledpin_t;

/* 逐脚高/低电平扫描, 用来确认灯珠引脚。
 * 实测两个版本引脚一致, 且都是低电平点亮:
 *   红 = P0.2   绿 = P2.5   蓝 = P2.9 */
static const ledpin_t led_pins[] = {
    { 0, GPIO_PORT_0, GPIO_PIN_2, "P0.2 R" },
    { 2, GPIO_PORT_2, GPIO_PIN_5, "P2.5 G" },
    { 2, GPIO_PORT_2, GPIO_PIN_9, "P2.9 B" },
};
#define LED_N       ((int)(sizeof(led_pins) / sizeof(led_pins[0])))
#define LED_STEPS   2          /* 每个脚: 推高 10 秒 -> 拉低 10 秒 */

static void led_screen(const char *name, const char *mode, uint16_t color)
{
    st7789_fill(C_BG);
    text_at(&f_tiny, 8, 30, "LED LEVEL TEST", C_WHITE);
    text_at(&f_tiny, 8, 60, name, color);
    text_at(&f_tiny, 8, 80, mode, C_GRAY);
    text_at(&f_tiny, 8, 120, "WATCH LEDS", C_GRAY);
}

static void led_test_tick(void)
{
    static int step = -1;
    static int hold = 0;
    int idx, sub;
    const ledpin_t *p;

    if (hold > 0)
    {
        hold--;
        return;
    }

    step = (step + 1) % (LED_N * LED_STEPS);
    idx = step / LED_STEPS;
    sub = step % LED_STEPS;
    p = &led_pins[idx];

    if (sub == 0)
    {
        led_screen(p->name, "HIGH", C_YELLOW);
        ws2812_pin(p->port, p->pin, 1);
    }
    else
    {
        led_screen(p->name, "LOW", C_CYAN);
        ws2812_pin(p->port, p->pin, 0);
    }
    hold = 9;                                      /* 每档保持 10 秒 */
}
#endif

/* ---------------------------------------------------------------- 状态灯 */
/* 三路都是低电平点亮 */
static void led_rgb(int r, int g, int b)
{
    ws2812_pin(LED_R_PORT, LED_R_PIN, r ? 0 : 1);
    ws2812_pin(LED_G_PORT, LED_G_PIN, g ? 0 : 1);
    ws2812_pin(LED_B_PORT, LED_B_PIN, b ? 0 : 1);
}

/* 事件灯: 平时全灭, 有事件才闪 (常亮太刺眼)
 * 颜色用 LEDC_R/G/B 位组合; 周期由 app 定时器控制 (单位 100ms), 支持自定义灯效 */
static timer_hnd led_timer = EASY_TIMER_INVALID_TIMER;
static uint8_t led_col  = 0;        /* 当前颜色位 */
static uint8_t led_left = 0;        /* 剩余半周期数 */
static uint8_t led_on   = 0;
static uint8_t led_step = 10;       /* 半周期长度, 100ms 单位 */

static void led_show(uint8_t col)
{
    if (!g_cfg.led_en)
        col = 0;
    led_rgb((col & LEDC_R) != 0, (col & LEDC_G) != 0, (col & LEDC_B) != 0);
}

static void led_timer_cb(void)
{
    led_timer = EASY_TIMER_INVALID_TIMER;

    if (!led_left)
    {
        led_show(0);
        return;
    }

    led_on ^= 1;
    led_show(led_on ? led_col : 0);
    if (--led_left == 0)
        return;

    led_timer = app_easy_timer((uint16_t)led_step * 10u, led_timer_cb);
}

static void led_flash(uint8_t col, uint8_t times, uint8_t step)
{
    if (!g_cfg.led_en || times == 0 || col == 0)
        return;
    if (led_left)
        return;                     /* 正在闪就忽略, 避免事件抢占 */

    if (led_timer != EASY_TIMER_INVALID_TIMER)
        app_easy_timer_cancel(led_timer);

    led_col  = col;
    led_left = (uint8_t)(times * 2 - 1);   /* 亮-灭-亮... 最后停在灭 */
    led_on   = 1;
    led_step = step ? step : 10;
    led_show(col);
    led_timer = app_easy_timer((uint16_t)led_step * 10u, led_timer_cb);
}

/* 上位机 0xC2 / 0xC5 用: 立即播放一次灯效 (period 单位 100ms) */
void ui_led_play(uint8_t color, uint8_t times, uint8_t period)
{
    led_flash(color, times, period);
}

/* ---------------------------------------------------------------- 初始化 */
/* 画静态框架 (清屏 + 标签/分隔线/蓝牙图标), 按显示字段掩码决定画哪些 */
static void draw_static(void)
{
    st7789_fill(C_BG);
    st7789_fill(C_BG);          /* 连刷两次, 清掉上电残影 */

    /* ---- 左区静态内容 (顶部一行留给网络信息, 由 ui_update 绘制) ---- */
    if (g_cfg.field_mask & FIELD_NETRATE)
    {
        text_at(&f_tiny, LEFT_X0, DL_LBL_Y, "DL", C_GRAY);
        text_at(&f_tiny, LEFT_X0, UL_LBL_Y, "UL", C_GRAY);
    }
    if (g_cfg.field_mask & FIELD_DISK)
    {
        st7789_fill_rect(LEFT_X0, DIV_L_Y, LEFT_X1, DIV_L_Y, C_LINE);
        text_at(&f_tiny, LEFT_X0, CK_LBL_Y, "C: DISK", C_GRAY);
    }

    /* ---- 右区静态内容 ---- */
    st7789_fill_rect(RIGHT_X0, DIV_R_Y, RIGHT_X1, DIV_R_Y, C_LINE);
    if (g_cfg.field_mask & FIELD_CPU)
        text_at(&f_tiny, RIGHT_X0, CPU_LBL_Y, "CPU", C_GRAY);
    if (g_cfg.field_mask & FIELD_GPU)
        text_at(&f_tiny, RIGHT_X0, GPU_LBL_Y, "GPU", C_GRAY);
    if (g_cfg.field_mask & FIELD_RAM)
        text_at(&f_tiny, RIGHT_X0, RAM_LBL_Y, "RAM", C_GRAY);

    /* ---- 底部: 蓝牙图标 (右侧同行显示主机名) ---- */
    draw_bt_icon(C_LINE);
}

void ui_init(void)
{
    g_mon.cpu_pct = 0;
    g_mon.ram_pct = 0;
    g_mon.ram_used_mb = 0;
    g_mon.ram_total_mb = 0;
    g_mon.net_dl_bps = 0;
    g_mon.net_ul_bps = 0;
    g_mon.disk_pct = 0;
    g_mon.gpu_pct = 0xFF;
    g_mon.cpu_temp = 0xFF;
    g_mon.gpu_temp = 0xFF;
    g_mon.net_type = 0;
    g_mon.net_name[0] = 0;
    g_mon.connected = 0;

#if UI_CALIB
    ui_ruler();
    return;
#endif

#if UI_LED_TEST
    st7789_fill(C_BG);
    text_at(&f_tiny, 8, 100, "LED TEST START", C_WHITE);
    return;
#endif

    led_show(0);

#if STANDALONE_CALENDAR
    /* 独立模式: 无论 Flash 里存过什么卡片, 上电一律显示万年历 */
    g_cfg.card = CARD_CALENDAR;
#endif

    bar_cpu.last_pct = -1;
    bar_gpu.last_pct = -1;
    bar_ram.last_pct = -1;
    bar_ck.last_pct  = -1;

    if (g_cfg.card != CARD_MONITOR)
    {
        ui_repaint_all();       /* 时钟 / 图像卡: 走卡片自己的绘制 */
        return;
    }

    draw_static();
    ui_update();
}

/* ---------------------------------------------------------------- 卡片 */
#define IMG_BAND   80           /* 一条带 = 2 行 x (320/8) 字节; 万年历常驻时让出 RAM 给中文字库 */
#define CARD_ROWS  2

static uint8_t img_open;
static int     img_w, img_h, img_off, img_rows;
static uint8_t img_band[IMG_BAND];

extern int year, month, date, wday, hour, minute, second;

static const char *WD_NAME[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };

static char last_c_hm[6]  = "";
static char last_c_ss[3]  = "";
static char last_c_dat[16] = "";

/* ---- 时钟用的数码管字形 (胶囊笔画, 不占字库) ---- */
#define SEG_W      44
#define SEG_H      100
#define SEG_T      12
#define CLK_Y0     38
#define CLK_X0     46
#define CLK_COL_X  154
#define CLK_SEC_X  282

/* 四个数字相对 CLK_X0 的偏移 (中间 16px 给冒号) */
static const uint8_t CLK_DX[4] = { 0, 56, 128, 184 };

/* 段位: bit0=a 上, 1=b 右上, 2=c 右下, 3=d 下, 4=e 左下, 5=f 左上, 6=g 中 */
static const uint8_t SEG_MASK[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void draw_digit(int x, int y, int d, uint16_t c)
{
    uint8_t m = SEG_MASK[d % 10];
    int t = SEG_T, w = SEG_W, h = SEG_H;
    int mid = h / 2;

    /* 笔画相接, 不留缺口 (经典液晶观感) */
    if (m & 0x01) st7789_fill_rect(x,         y,         x + w,     y + t,        c); /* a */
    if (m & 0x02) st7789_fill_rect(x + w - t, y + t,     x + w,     y + mid - t/2, c); /* b */
    if (m & 0x04) st7789_fill_rect(x + w - t, y + mid + t/2, x + w, y + h - t,    c); /* c */
    if (m & 0x08) st7789_fill_rect(x,         y + h - t, x + w,     y + h,        c); /* d */
    if (m & 0x10) st7789_fill_rect(x,         y + mid + t/2, x + t, y + h - t,    c); /* e */
    if (m & 0x20) st7789_fill_rect(x,         y + t,     x + t,     y + mid - t/2, c); /* f */
    if (m & 0x40) st7789_fill_rect(x,         y + mid - t/2, x + w, y + mid + t/2, c); /* g */
}

/* 纯时钟卡片: full=1 整屏重画, 0=只刷变化的字 */
static void card_clock(int full)
{
    char hm[6], ss[3], dat[16];
    const char *wd;
    int i;

    if (full)
    {
        st7789_fill(C_BG);
        st7789_fill(C_BG);
        st7789_fill_rect(20, 196, 299, 196, C_LINE);
        text_at(&f_tiny, 20, 208, "PC MONITOR", C_GRAY);
        /* 冒号是静态的, 用强调色 */
        st7789_fill_rect(CLK_COL_X + 2, CLK_Y0 + 30, CLK_COL_X + 13, CLK_Y0 + 41, C_CYAN);
        st7789_fill_rect(CLK_COL_X + 2, CLK_Y0 + 58, CLK_COL_X + 13, CLK_Y0 + 69, C_CYAN);
        last_c_hm[0] = 0;
        last_c_ss[0] = 0;
        last_c_dat[0] = 0;
    }

    hm[0] = (char)('0' + hour / 10);
    hm[1] = (char)('0' + hour % 10);
    hm[2] = ':';
    hm[3] = (char)('0' + minute / 10);
    hm[4] = (char)('0' + minute % 10);
    hm[5] = 0;

    for (i = 0; i < 4; i++)
    {
        int idx = (i < 2) ? i : i + 1;          /* 跳过冒号 */
        int x = CLK_X0 + CLK_DX[i];

        if (!full && hm[idx] == last_c_hm[idx])
            continue;

        st7789_fill_rect(x, CLK_Y0, x + SEG_W - 1, CLK_Y0 + SEG_H - 1, C_BG);
        draw_digit(x, CLK_Y0, hm[idx] - '0', C_WHITE);
    }
    for (i = 0; i < 6; i++) last_c_hm[i] = hm[i];

    ss[0] = (char)('0' + second / 10);
    ss[1] = (char)('0' + second % 10);
    ss[2] = 0;
    if (str_diff(ss, last_c_ss))
    {
        text_diff(&f_med, CLK_SEC_X, CLK_Y0 + 44, ss, last_c_ss, C_CYAN);
        last_c_ss[0] = ss[0];
        last_c_ss[1] = ss[1];
        last_c_ss[2] = 0;
    }

    wd = (wday >= 0 && wday < 7) ? WD_NAME[wday] : "---";
    dat[0]  = (char)('0' + (year / 1000) % 10);
    dat[1]  = (char)('0' + (year / 100) % 10);
    dat[2]  = (char)('0' + (year / 10) % 10);
    dat[3]  = (char)('0' + year % 10);
    dat[4]  = '-';
    dat[5]  = (char)('0' + month / 10);
    dat[6]  = (char)('0' + month % 10);
    dat[7]  = '-';
    dat[8]  = (char)('0' + (date + 1) / 10);
    dat[9]  = (char)('0' + (date + 1) % 10);
    dat[10] = ' ';
    dat[11] = wd[0];
    dat[12] = wd[1];
    dat[13] = wd[2];
    dat[14] = 0;

    if (str_diff(dat, last_c_dat))
    {
        text_diff(&f_med, (LCD_W - 14 * f_med.adv) / 2, 152, dat, last_c_dat, C_WHITE);
        for (i = 0; i < 15; i++) last_c_dat[i] = dat[i];
    }
}

/* 图像卡: 一条带攒满就贴上去 (不用整屏缓冲) */
static void img_flush(int rows)
{
    if (rows <= 0) return;
    blit_bmp1(0, img_rows * CARD_ROWS, img_w, rows, img_band, C_WHITE);
    img_rows++;
}

void ui_image_start(int w, int h)
{
    if (w < 8 || w > LCD_W || h <= 0 || h > LCD_H) return;   /* w<8 时行宽为 0, 后面会除零 */

    img_w = w;
    img_h = h;
    img_off = 0;
    img_rows = 0;
    img_open = 1;

    st7789_fill(C_BG);
    st7789_fill(C_BG);
}

void ui_image_data(const uint8_t *buf, int len)
{
    if (!img_open || len <= 0) return;

    while (len > 0 && img_rows * CARD_ROWS < img_h)
    {
        int n = IMG_BAND - img_off;
        if (n > len) n = len;

        memcpy(img_band + img_off, buf, n);
        img_off += n;
        buf += n;
        len -= n;

        if (img_off == IMG_BAND)
        {
            img_flush(CARD_ROWS);
            img_off = 0;
        }
    }
}

void ui_image_end(void)
{
    if (!img_open) return;

    if (img_off > 0)
    {
        int rows = img_off / (img_w / 8);
        int left = img_h - img_rows * CARD_ROWS;
        if (rows > left) rows = left;
        img_flush(rows);
    }
    img_open = 0;
}

void ui_set_card(uint8_t card)
{
#if STANDALONE_CALENDAR
    /* 独立模式不提供卡片切换: 任何 0xC8 命令都不能把万年历切走 */
    card = CARD_CALENDAR;
#endif
    if (card >= CARD_N) card = CARD_MONITOR;
    if (card == g_cfg.card && !img_open) return;   /* 没变就不重绘, 避免重连时闪屏 */

    g_cfg.card = card;
    img_open = 0;
    ui_repaint_all();
}

/* ================================================================== 万年历卡片
 * 320x240 横屏, 中间 x96..116 是漏液带, 必须保持全黑:
 *   顶栏 y4     左: YYYY/MM/DD        右: 星期X
 *   左栏 x6..92 农历 / 干支年 / 生肖 / 农历月日 / 节气 / 节日
 *   右栏 x122..316  大时钟 HH:MM + 秒 / 当月月历 (今天高亮)
 * 时间源是固件本地 RTC (clock_update 逐秒进位)。PC 只在连上时用 0x91 对时,
 * 蓝牙断开后照常走时; 农历/星期/节气/节日全部本地推算, 不依赖电脑常开。
 * ========================================================================== */

extern int l_year, l_month, l_date;
extern void cal_sync_date(void);
extern const char *cal_jieqi_str(void);
extern const char *cal_holiday_str(void);

/* UTF-8 -> UCS2, 同时推进指针; 0 表示串结束 */
static int cal_utf8(const char **ps)
{
    const uint8_t *p = (const uint8_t *)*ps;
    int c;

    if (!p[0]) return 0;
    if (p[0] < 0x80) { *ps += 1; return p[0]; }
    if (p[0] < 0xE0)
    {
        c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *ps += 2;
        return c;
    }
    c = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    *ps += 3;
    return c;
}

static int cal_find(uint16_t code)
{
    int lo = 0, hi = CAL_FONT_N - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        uint16_t v = CAL_FONT_CODES[mid];
        if (v == code) return mid;
        if (v < code) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* 中文串绘制: 每字 16x16, 复用 blit_bmp1 (自动用背景色填满该块) */
static void ctext(int x, int y, const char *s, uint16_t fg)
{
    int code;

    while ((code = cal_utf8(&s)) != 0)
    {
        int idx = cal_find((uint16_t)code);
        if (idx >= 0)
        {
            blit_bmp1(x, y, 16, 16, CAL_FONT_BITS + idx * 32, fg);
            x += 16;
        }
        else if (code >= 0x20 && code < 0x7F)
        {
            char t[2];
            t[0] = (char)code; t[1] = 0;
            text_at(&f_tiny, x, y, t, fg);
            x += 8;
        }
        else
        {
            st7789_fill_rect(x, y, x + 15, y + 15, C_BG);
            x += 16;
        }
    }
}

/* 追加一个 UTF-8 汉字 (3 字节) */
static void cappend(char *buf, const char *ch)
{
    char *p = buf;
    while (*p) p++;
    p[0] = ch[0]; p[1] = ch[1]; p[2] = ch[2]; p[3] = 0;
}

static const char *CN_DIG[10]  = { "一","二","三","四","五","六","七","八","九","十" };
static const char *CN_WD[7]   = { "日","一","二","三","四","五","六" };
static const char *CN_MON[12] = { "正","二","三","四","五","六","七","八","九","十","冬","腊" };
static const char *TG[10]     = { "甲","乙","丙","丁","戊","己","庚","辛","壬","癸" };
static const char *DZ[12]     = { "子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥" };
static const char *SX[12]     = { "鼠","牛","虎","兔","龙","蛇","马","羊","猴","鸡","狗","猪" };

/* 农历日期名: 初一..初十 / 十一..十九 / 二十 / 廿一..廿九 / 三十 */
static void cal_lunar_day(char *out)
{
    int d = l_date;
    out[0] = 0;
    if (d < 10)       { cappend(out, "初"); cappend(out, CN_DIG[d]); }
    else if (d < 19)  { cappend(out, "十"); cappend(out, CN_DIG[d - 10]); }
    else if (d == 19) { cappend(out, "二"); cappend(out, "十"); }
    else if (d < 29)  { cappend(out, "廿"); cappend(out, CN_DIG[d - 20]); }
    else              { cappend(out, "三"); cappend(out, "十"); }
}

/* 最多显示 max_ch 个汉字 (节日名可能较长, 不能画进漏液带) */
static void ctext_clip(int x, int y, const char *s, uint16_t fg, int max_ch)
{
    char t[24];
    const char *p;
    int n = 0, k = 0;

    if (!s) { ctext(x, y, "无", fg); return; }
    p = s;
    while (*p && n < max_ch) { if (!cal_utf8(&p)) break; n++; }
    if (n == 0) { ctext(x, y, "无", fg); return; }
    p = s;
    while (n-- > 0) { t[k++] = *p++; t[k++] = *p++; t[k++] = *p++; }
    t[k] = 0;
    ctext(x, y, t, fg);
}

#define CAL_GX    124       /* 月历网格左上 x */
#define CAL_GY    136       /* 月历网格第一行 y */
#define CAL_CW    27        /* 列宽, 7*27=189 -> x124..313 */
#define CAL_RH    15        /* 行高, 6*15=90 -> y136..226 */

static int cal_ready = 0;
static int cal_ly = -1, cal_lm = -1, cal_ld = -1;
static int cal_lhh = -1, cal_lmm = -1, cal_lss = -1;
static int cal_last_link = -1;

/* 右上角蓝牙指示: 绿色 = 已连接(可写 0x91 对时), 暗灰 = 未连接(本地走时) */
#define CAL_BT_X   300
#define CAL_BT_Y   0

static void cal_draw_link(void)
{
    draw_bt_icon_at(CAL_BT_X, CAL_BT_Y,
                    g_mon.connected ? C_GREEN : C_GRAY);
}

static void cal_draw_time(void)
{
    char hm[6], ss[3];

    hm[0] = (char)('0' + hour / 10);
    hm[1] = (char)('0' + hour % 10);
    hm[2] = ':';
    hm[3] = (char)('0' + minute / 10);
    hm[4] = (char)('0' + minute % 10);
    hm[5] = 0;
    ss[0] = (char)('0' + second / 10);
    ss[1] = (char)('0' + second % 10);
    ss[2] = 0;
    text_at(&f_big, 146, 32, hm, C_CYAN);
    text_at(&f_tiny, 296, 34, ss, C_WHITE);
}

static void cal_draw_static(void)
{
    static const uint8_t mdays_tab[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    char s1[24], s2[24], s3[24], nb[3];
    int i, col, row, first, mdays, v, x, y, ti, di, m1;
    const char *p;

    st7789_fill(C_BG);
    st7789_fill(C_BG);

    /* ---- 顶栏 ---- */
    s1[0] = (char)('0' + (year / 1000) % 10);
    s1[1] = (char)('0' + (year / 100) % 10);
    s1[2] = (char)('0' + (year / 10) % 10);
    s1[3] = (char)('0' + year % 10);
    s1[4] = '/';
    s1[5] = (char)('0' + (month + 1) / 10);
    s1[6] = (char)('0' + (month + 1) % 10);
    s1[7] = '/';
    s1[8] = (char)('0' + (date + 1) / 10);
    s1[9] = (char)('0' + (date + 1) % 10);
    s1[10] = 0;
    text_at(&f_tiny, 6, 4, s1, C_WHITE);

    s2[0] = 0;
    cappend(s2, "星"); cappend(s2, "期");
    cappend(s2, CN_WD[(wday >= 0 && wday < 7) ? wday : 0]);
    ctext(238, 4, s2, C_YELLOW);

    cal_last_link = g_mon.connected;
    cal_draw_link();

    st7789_fill_rect(6, 24, 92, 24, C_LINE);
    st7789_fill_rect(122, 24, 314, 24, C_LINE);

    /* ---- 左栏: 农历信息 ---- */
    ti = (year - 4) % 10; if (ti < 0) ti += 10;
    di = (year - 4) % 12; if (di < 0) di += 12;

    ctext(6, 32, "农历", C_GRAY);

    s1[0] = 0;
    cappend(s1, TG[ti]); cappend(s1, DZ[di]); cappend(s1, "年");
    ctext(6, 52, s1, C_WHITE);

    s1[0] = 0;
    cappend(s1, SX[di]); cappend(s1, "年");
    ctext(6, 72, s1, C_GRAY);

    s1[0] = 0;
    if (l_month & 0x80) cappend(s1, "闰");
    cappend(s1, CN_MON[l_month & 0x7f]);
    cappend(s1, "月");
    cal_lunar_day(s3);
    for (p = s3; *p; p++) { /* 把农历日名接到月份串后面 */
        char one[4];
        one[0] = p[0]; one[1] = p[1]; one[2] = p[2]; one[3] = 0;
        cappend(s1, one);
        p += 2;
    }
    ctext(6, 94, s1, C_YELLOW);

    st7789_fill_rect(6, 118, 92, 118, C_LINE);
    ctext(6, 124, "节气", C_GRAY);
    ctext_clip(6, 142, cal_jieqi_str(), C_CYAN, 5);
    ctext(6, 166, "节日", C_GRAY);
    ctext_clip(6, 184, cal_holiday_str(), C_RED, 5);
    ctext(6, 212, "万年历", C_GRAY);

    /* ---- 右栏: 大时钟 ---- */
    cal_draw_time();
    st7789_fill_rect(122, 90, 314, 90, C_LINE);

    /* 公历月名 (中文) */
    s1[0] = 0;
    m1 = month + 1;
    if (m1 <= 10) cappend(s1, CN_DIG[m1 - 1]);
    else { cappend(s1, "十"); cappend(s1, CN_DIG[m1 - 11]); }
    cappend(s1, "月");
    ctext(126, 96, s1, C_WHITE);

    /* 表头 日..六 */
    for (col = 0; col < 7; col++)
        ctext(CAL_GX + col * CAL_CW + 5, 116, CN_WD[col],
              (col == 0 || col == 6) ? C_RED : C_GRAY);

    /* 当月网格 */
    mdays = mdays_tab[month];
    if (month == 1 && (((year % 4) == 0 && (year % 100) != 0) || (year % 400) == 0))
        mdays += 1;
    first = wday - date;                  /* 本月 1 号是星期几 */
    while (first < 0) first += 7;

    for (row = 0; row < 6; row++)
    {
        for (col = 0; col < 7; col++)
        {
            v = row * 7 + col - first + 1;
            if (v < 1 || v > mdays) continue;
            x = CAL_GX + col * CAL_CW;
            y = CAL_GY + row * CAL_RH;
            nb[0] = (v < 10) ? ' ' : (char)('0' + v / 10);
            nb[1] = (char)('0' + v % 10);
            nb[2] = 0;
            if (v == date + 1)
            {
                st7789_fill_rect(x + 1, y - 2, x + CAL_CW - 2, y - 2, C_YELLOW);
                st7789_fill_rect(x + 1, y + 12, x + CAL_CW - 2, y + 12, C_YELLOW);
                st7789_fill_rect(x + 1, y - 2, x + 1, y + 12, C_YELLOW);
                st7789_fill_rect(x + CAL_CW - 2, y - 2, x + CAL_CW - 2, y + 12, C_YELLOW);
                text_at(&f_tiny, x + 6, y, nb, C_YELLOW);
            }
            else
            {
                text_at(&f_tiny, x + 6, y, nb,
                        (col == 0 || col == 6) ? C_GRAY : C_WHITE);
            }
        }
    }
    (void)s2; (void)i;
}

static void card_calendar(int full)
{
    if (!cal_ready)
    {
        cal_sync_date();       /* 开机兜底: 按当前公历重算农历/星期/节气/节日 */
        cal_ready = 1;
    }

    /* 连接状态变化时只重画右上角图标, 不打断走时/不整屏重绘 */
    if (g_mon.connected != cal_last_link)
    {
        cal_last_link = g_mon.connected;
        cal_draw_link();
    }

    if (full || year != cal_ly || month != cal_lm || date != cal_ld)
    {
        cal_draw_static();
        cal_ly = year; cal_lm = month; cal_ld = date;
        cal_lhh = hour; cal_lmm = minute; cal_lss = second;
        return;
    }

    if (hour != cal_lhh || minute != cal_lmm)
    {
        st7789_fill_rect(146, 32, 146 + 5 * f_big.adv - 1, 32 + f_big.h - 1, C_BG);
        cal_draw_time();
        cal_lhh = hour; cal_lmm = minute; cal_lss = second;
    }
    else if (second != cal_lss)
    {
        char ss[3];
        st7789_fill_rect(296, 34, 296 + 2 * f_tiny.adv - 1, 34 + f_tiny.h - 1, C_BG);
        ss[0] = (char)('0' + second / 10);
        ss[1] = (char)('0' + second % 10);
        ss[2] = 0;
        text_at(&f_tiny, 296, 34, ss, C_WHITE);
        cal_lss = second;
    }
}

/* ---------------------------------------------------------------- 刷新 */
static int  last_cpu_pct = -1;
static int  last_gpu_pct = -1;
static int  last_ram_pct = -1;
static int  last_ck_pct  = -1;
static uint16_t last_cpu_col, last_gpu_col, last_ram_col, last_ck_col;
static int  last_link = -1;
static char last_hm[6]     = ".....";
static char last_ss[3]     = "..";
static char last_ram_s[16] = "";
static char last_dl_s[12]  = "";
static char last_ul_s[12]  = "";
static char last_net_s[22] = "";
/* 温度串最长 "100`C" = 5 字符 + '\0' = 6 字节 (三位数时曾越界, 残留多余的 C) */
static char last_cpu_t[6]  = "";
static char last_gpu_t[6]  = "";

/* 温度: "62`C" —— '`' 是字库里代表摄氏度符号 '°' 的占位键; 未知显示 "--"
 * 最多 5 个字符宽(三位数), 清屏也要清 5 格, 否则会留下残影 */
static void draw_temp(int x, int y, uint8_t t, char *last)
{
    char s[6];
    int i;

    if (t > 120) { s[0] = '-'; s[1] = '-'; s[2] = 0; }
    else
    {
        i = fmt_u(s, t);
        s[i++] = '`';
        s[i++] = 'C';
        s[i] = 0;
    }
    if (str_diff(s, last))
    {
        text_clear(&f_tiny, x, y, 5);
        text_at(&f_tiny, x, y, s, C_GRAY);
        for (i = 0; i < 6; i++) { last[i] = s[i]; if (!s[i]) break; }
    }
}

/* 右对齐百分比: 同色时逐字符差异刷新, 颜色变化时才整串重画
 * pct > 100 视为未知, 显示 "--" */
static void update_pct(int right, int y, int pct, int *lp, uint16_t *lc)
{
    char s[8], o[8];
    uint16_t col;
    int unknown = (pct > 100);

    if (pct == *lp) return;

    if (unknown) { s[0] = '-'; s[1] = '-'; s[2] = 0; col = C_GRAY; }
    else         { fmt_pct(s, pct); col = pct_color(pct); }

    if (*lp < 0)
    {
        text_right(&f_med, right, y, s, col);
    }
    else
    {
        if (*lp > 100) { o[0] = '-'; o[1] = '-'; o[2] = 0; }
        else fmt_pct(o, *lp);

        if (col != *lc)
        {
            int no = slen(o);
            int ns = slen(s);
            text_clear(&f_med, right - no * f_med.adv + 1, y, no);
            text_at(&f_med, right - ns * f_med.adv + 1, y, s, col);
        }
        else
        {
            text_right_diff(&f_med, right, y, s, o, col);
        }
    }

    *lp = pct;
    *lc = col;
}

void ui_update(void)
{
    char buf[24];
    int i, x0;

#if UI_CALIB
    (void)buf;
    (void)i;
    (void)x0;
    return;                 /* 标定模式下保持刻度尺, 不覆盖画面 */
#endif

    /* ---- 卡片: 时钟刷新秒, 万年历走本地 RTC, 图像卡由上位机推图 ---- */
    if (g_cfg.card == CARD_CLOCK)
    {
        card_clock(0);
        return;
    }
    if (g_cfg.card == CARD_CALENDAR)
    {
        card_calendar(0);
        return;
    }
    if (g_cfg.card != CARD_MONITOR)
        return;

    /* ---- 时间: HH:MM 用 52px 大字, 秒用 26px, 同一基线对齐 ---- */
    if (g_cfg.field_mask & FIELD_CLOCK)
    {
        buf[0] = (char)('0' + g_mon.hour / 10);
        buf[1] = (char)('0' + g_mon.hour % 10);
        buf[2] = ':';
        buf[3] = (char)('0' + g_mon.min / 10);
        buf[4] = (char)('0' + g_mon.min % 10);
        buf[5] = 0;
        x0 = RIGHT_CX - (5 * f_big.adv + 2 * f_med.adv) / 2;
        if (str_diff(buf, last_hm))
        {
            text_diff(&f_big, x0, TIME_Y, buf, last_hm, C_CYAN);
            for (i = 0; i < 6; i++) last_hm[i] = buf[i];
        }

        buf[0] = (char)('0' + g_mon.sec / 10);
        buf[1] = (char)('0' + g_mon.sec % 10);
        buf[2] = 0;
        if (str_diff(buf, last_ss))
        {
            text_diff(&f_med, x0 + 5 * f_big.adv,
                      TIME_Y + f_big.asc - f_med.asc, buf, last_ss, C_CYAN);
            last_ss[0] = buf[0];
            last_ss[1] = buf[1];
        }
    }


    /* ---- CPU ---- */
    if (g_cfg.field_mask & FIELD_CPU)
    {
        update_pct(RIGHT_X1, CPU_VAL_Y, g_mon.cpu_pct, &last_cpu_pct, &last_cpu_col);
        bar_update(&bar_cpu, g_mon.cpu_pct, pct_color(g_mon.cpu_pct));
    }

    if (g_cfg.field_mask & FIELD_CPUTEMP)
        draw_temp(TEMP_X, CPU_LBL_Y, g_mon.cpu_temp, last_cpu_t);

    if (g_cfg.field_mask & FIELD_GPU)
    {
        /* ---- GPU (0xFF = 采集不到, 显示 --) ---- */
        {
            int g = (g_mon.gpu_pct > 100) ? 0xFF : g_mon.gpu_pct;
            update_pct(RIGHT_X1, GPU_VAL_Y, g, &last_gpu_pct, &last_gpu_col);
            if (g > 100) bar_update(&bar_gpu, 0, C_BAR_BG);
            else         bar_update(&bar_gpu, g, pct_color(g));
        }
    }

    if (g_cfg.field_mask & FIELD_GPUTEMP)
        draw_temp(TEMP_X, GPU_LBL_Y, g_mon.gpu_temp, last_gpu_t);

    /* ---- RAM ---- */
    if (g_cfg.field_mask & FIELD_RAM)
    {
        update_pct(RIGHT_X1, RAM_VAL_Y, g_mon.ram_pct, &last_ram_pct, &last_ram_col);
        bar_update(&bar_ram, g_mon.ram_pct, pct_color(g_mon.ram_pct));

        fmt_ram(buf, g_mon.ram_used_mb, g_mon.ram_total_mb);
        if (str_diff(buf, last_ram_s))
        {
            text_diff(&f_tiny, RIGHT_X0, RAM_USED_Y, buf, last_ram_s, C_GRAY);
            for (i = 0; i < 15; i++) last_ram_s[i] = buf[i];
        }
    }


    /* ---- 左区: 下载 / 上传速率 ---- */
    if (g_cfg.field_mask & FIELD_NETRATE)
    {
        fmt_rate_short(buf, g_mon.net_dl_bps);
        if (str_diff(buf, last_dl_s))
        {
            text_right_diff(&f_med, LEFT_X1, DL_VAL_Y, buf, last_dl_s, C_WHITE);
            for (i = 0; i < 11; i++) last_dl_s[i] = buf[i];
        }
    }


    if (g_cfg.field_mask & FIELD_NETRATE)
    {
        fmt_rate_short(buf, g_mon.net_ul_bps);
        if (str_diff(buf, last_ul_s))
        {
            text_right_diff(&f_med, LEFT_X1, UL_VAL_Y, buf, last_ul_s, C_WHITE);
            for (i = 0; i < 11; i++) last_ul_s[i] = buf[i];
        }
    }


    /* ---- 左区: C 盘占用 ---- */
    if (g_cfg.field_mask & FIELD_DISK)
    {
        update_pct(LEFT_X1, CK_VAL_Y, g_mon.disk_pct, &last_ck_pct, &last_ck_col);
        bar_update(&bar_ck, g_mon.disk_pct, pct_color(g_mon.disk_pct));
    }


    /* ---- 连接状态: 蓝牙图标 + 左上角提示 ---- */
    if (g_mon.connected != last_link)
    {
        last_link = g_mon.connected;
        if (g_mon.connected)
        {
            draw_bt_icon(C_GREEN);
            if (net_bmp_ready)
                net_bmp_dirty = 1;      /* 用网络名覆盖 "主机未连接" */
            else
                st7789_fill_rect(LEFT_X0, NET_LBL_Y,
                                 LEFT_X0 + NET_BMP_W_MAX - 1,
                                 NET_LBL_Y + NET_BMP_H - 1, C_BG);
        }
        else
        {
            draw_bt_icon(C_RED);
            /* 独立万年历不需要电脑: 不显示"主机未连接", 只清掉旧网络名 */
            st7789_fill_rect(LEFT_X0, NET_LBL_Y,
                             LEFT_X0 + NET_BMP_W_MAX - 1,
                             NET_LBL_Y + NET_BMP_H - 1, C_BG);
        }
    }

    /* ---- 左区顶部: 网络名称位图 (含中文 SSID, 由 PC 渲染) ---- */
    if (net_bmp_dirty && (g_cfg.field_mask & FIELD_NETNAME))
    {
        draw_net_bmp();
        net_bmp_dirty = 0;
    }

    /* ---- 主机名: 与蓝牙图标同一行, 替代原来的 LINK 字样 ---- */
    if (host_dirty && (g_cfg.field_mask & FIELD_HOST))
    {
        text_clear(&f_tiny, HOST_X, HOST_Y, HOST_MAX_CHARS);
        text_at(&f_tiny, HOST_X, HOST_Y, host_name, C_WHITE);
        host_dirty = 0;
    }
}

/* 配置变化后整屏重绘 (重置差异缓存, 让所有内容重新画一遍) */
void ui_repaint_all(void)
{
    if (g_cfg.card == CARD_CLOCK)
    {
        card_clock(1);
        return;
    }

    if (g_cfg.card == CARD_CALENDAR)
    {
        card_calendar(1);
        return;
    }

    if (g_cfg.card != CARD_MONITOR)
    {
        /* 图像卡: 清屏 + 提示, 等上位机推图 */
        st7789_fill(C_BG);
        st7789_fill(C_BG);
        text_center(&f_med, LCD_W / 2, 104, "WAITING FOR PC", C_GRAY);
        return;
    }

    draw_static();

    bar_cpu.last_pct = -1;
    bar_gpu.last_pct = -1;
    bar_ram.last_pct = -1;
    bar_ck.last_pct  = -1;

    last_cpu_pct = last_gpu_pct = last_ram_pct = last_ck_pct = -1;
    last_link = -1;
    last_hm[0] = 0;
    last_ss[0] = 0;
    last_ram_s[0] = 0;
    last_dl_s[0] = 0;
    last_ul_s[0] = 0;
    last_cpu_t[0] = 0;
    last_gpu_t[0] = 0;
    host_dirty = 1;
    net_bmp_dirty = 1;

    ui_update();
}

/* ---------------------------------------------------------------- 每秒驱动 */
void ui_tick(void)
{
    static uint16_t overheat_cool = 0;   /* 过热闪烁冷却剩余秒数 */
    static uint8_t  overheat_hold = 0;   /* 温度连续超标秒数 */
    static uint8_t  prev_sec = 0xFF;

#if UI_LED_TEST
    led_test_tick();
    return;
#endif

#if !STANDALONE_CALENDAR
    if (!g_mon.connected)
    {
        /* 仅监控面板调试用: 尚无 PC 数据时跑一段模拟曲线 */
        static unsigned phase = 0;
        phase = (phase + 7) & 255;
        g_mon.cpu_pct = (uint8_t)(20 + (phase * 5) % 70);
        g_mon.ram_pct = (uint8_t)(50 + (phase * 3) % 40);
        g_mon.ram_used_mb = (uint16_t)(7000 + (phase * 37) % 3000);
        g_mon.ram_total_mb = 16384;
        g_mon.net_dl_bps = 300000UL + (uint32_t)phase * 52000UL;
        g_mon.net_ul_bps = 60000UL + (uint32_t)phase * 9000UL;
        g_mon.disk_pct = (uint8_t)(45 + (phase * 2) % 30);
        g_mon.gpu_pct = (uint8_t)(10 + (phase * 3) % 60);
        g_mon.cpu_temp = (uint8_t)(45 + (phase % 20));
        g_mon.gpu_temp = (uint8_t)(40 + (phase % 25));
    }
#endif

    /* 独立万年历: 时间始终由本地 RTC 提供, 与是否连着蓝牙无关 */
    ui_update();

    /* ---- 状态灯: 平时全灭, 只有事件才闪 (颜色/次数/阈值可由上位机配置) ---- */
    /* CPU 过热: 连续 5 秒达到阈值才闪, 冷却时间可配 */
    if (g_mon.cpu_temp != 0xFF && g_mon.cpu_temp >= g_cfg.overheat_temp)
    {
        if (overheat_hold < 5)
            overheat_hold++;
    }
    else
    {
        overheat_hold = 0;
    }

    if (overheat_cool)
    {
        overheat_cool--;
    }
    else if (overheat_hold >= 5 && g_cfg.overheat_times)
    {
        led_flash(g_cfg.overheat_color, g_cfg.overheat_times, 10);
        overheat_cool = g_cfg.overheat_cool ? g_cfg.overheat_cool : 600;
        overheat_hold = 0;
    }

    /* 整点报时: 59 -> 00 秒那一刻, 每小时一次 */
    if (g_cfg.hourly_en && g_cfg.hourly_times &&
        g_mon.min == 0 && g_mon.sec == 0 && prev_sec == 59)
        led_flash(g_cfg.hourly_color, g_cfg.hourly_times, 10);
    prev_sec = g_mon.sec;
}

/* ---------------------------------------------------------------- 方向自检 */
/*
 * 4 种 MADCTL 覆盖 320x240 下的全部方向/镜像组合:
 *   mode 0: 0x28  MX=0 MY=0
 *   mode 1: 0x68  MX=1 MY=0   <- 版本一正向
 *   mode 2: 0xA8  MX=0 MY=1
 *   mode 3: 0xE8  MX=1 MY=1
 * 正向画面应满足: 红块在左上、绿色在右上、黄色横条在上、青色竖条在左、数字正着。
 */
void ui_dir_test(int mode)
{
    static const uint8_t mads[4] = { 0x28, 0x68, 0xA8, 0xE8 };
    static const uint16_t tags[4] = { 0x10A2, 0x8000, 0x0200, 0x4010 };
    char s[2];

    mode &= 3;
    st7789_set_madctl(mads[mode]);
    st7789_fill(C_BG);

    /* 四角标记 */
    st7789_fill_rect(0, 0, 49, 49, LCD_RED);                            /* 左上 红 */
    st7789_fill_rect(LCD_W - 50, 0, LCD_W - 1, 49, LCD_GREEN);          /* 右上 绿 */
    st7789_fill_rect(0, LCD_H - 50, 49, LCD_H - 1, LCD_BLUE);           /* 左下 蓝 */
    st7789_fill_rect(LCD_W - 50, LCD_H - 50, LCD_W - 1, LCD_H - 1, LCD_WHITE);

    /* L 形参考: 顶部黄色横条 + 左侧青色竖条 */
    st7789_fill_rect(60, 0, LCD_W - 61, 11, LCD_YELLOW);
    st7789_fill_rect(0, 60, 11, LCD_H - 61, LCD_CYAN);

    /* 模式编号 (1..4), 同时用顶部色条标识 */
    st7789_fill_rect(0, LCD_H - 12, LCD_W - 1, LCD_H - 1, tags[mode]);
    s[0] = (char)('1' + mode);
    s[1] = 0;
    text_center(&f_med, LCD_W / 2, LCD_H / 2 - 14, s, C_WHITE);
}
