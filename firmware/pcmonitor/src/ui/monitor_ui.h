/**
 ****************************************************************************************
 * @file monitor_ui.h
 * @brief PC 资源监控界面 (ST7789 320x240)
 *
 * 无帧缓冲, 直接流式写屏。静态框架只在 ui_init() 画一次,
 * 之后每秒只重绘变化的小区域, 保证软件 SPI 下也能流畅刷新。
 ****************************************************************************************
 */
#ifndef _MONITOR_UI_H_
#define _MONITOR_UI_H_

#include <stdint.h>

/** 监控数据 (由 BLE 或本地模拟填充) */
typedef struct
{
    uint8_t  cpu_pct;      /* 0..100 */
    uint8_t  ram_pct;      /* 0..100 */
    uint16_t ram_used_mb;
    uint16_t ram_total_mb;
    uint32_t net_dl_bps;   /* 下载 字节/秒 */
    uint32_t net_ul_bps;   /* 上传 字节/秒 */
    uint8_t  disk_pct;     /* C 盘占用 % 0..100 */
    uint8_t  gpu_pct;      /* 0..100, 0xFF = 未知 */
    uint8_t  cpu_temp;     /* 摄氏度, 0xFF = 未知 */
    uint8_t  gpu_temp;     /* 摄氏度, 0xFF = 未知 */
    uint8_t  connected;    /* 1 = PC agent 已连接 */
    uint8_t  net_type;     /* 0=未知 1=有线 2=WiFi */
    char     net_name[20]; /* WiFi SSID 或网卡名 */
    uint8_t  hour, min, sec;
} mon_stats_t;

extern mon_stats_t g_mon;

/** 绘制静态框架 (标题栏/标签/进度条外框) 并清屏 */
void ui_init(void);

/** 刷新动态内容: 只重绘数值/进度条/时间 */
void ui_update(void);

/** 每秒调用一次: 推进模拟数据并刷新 (BLE 接入后改成用真实数据) */
void ui_tick(void);

/** 绘制文本 (抗锯齿中号字) */
void ui_draw_text(int x, int y, const char *s, uint16_t fg);
void ui_draw_text_center(int cx, int y, const char *s, uint16_t fg);

/** 设置底部显示的电脑名称 (最多 15 字符, 超出截断) */
void ui_set_host_name(const char *s);

/** 网络名称位图分块下发: 宽 w 像素, 每行 (w+7)/8 字节, 逐行从上到下, 高位在左
 *  offset 为字节偏移, n 为本块字节数; 收满后自动重绘 */
void ui_net_bmp_chunk(uint8_t w, uint8_t total, uint8_t offset, const uint8_t *d, uint8_t n);

/** 方向/镜像自检: mode 0..3 对应 MADCTL = 0x28/0x68/0xA8/0xE8 (均带 MV, 320x240) */
void ui_dir_test(int mode);

/** 配置变化后整屏重绘 (会重置差异缓存) */
void ui_repaint_all(void);

/** 立即播放一次灯效: color = LEDC_R|G|B 位组合, times = 闪烁次数, period 单位 100ms */
void ui_led_play(uint8_t color, uint8_t times, uint8_t period);

/* ---------------- 卡片 (监控 / 纯时钟 / 天气 / TODO / 画册) ---------------- */

/** 切换卡片 (CARD_*) 并立即重绘 */
void ui_set_card(uint8_t card);

/** 图像卡开始: 清屏, 准备接收 w*h 的 1bpp 位图 (高位在左, 每行 (w+7)/8 字节) */
void ui_image_start(int w, int h);

/** 图像卡数据: 按顺序追加 len 字节; 每攒满一条带 (1600 字节) 自动上屏 */
void ui_image_data(const uint8_t *buf, int len);

/** 图像卡结束: 把不足一条带的数据补齐后上屏 */
void ui_image_end(void);

#endif /* _MONITOR_UI_H_ */
