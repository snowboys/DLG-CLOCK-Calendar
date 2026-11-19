# pcmonitor 固件工程

这个目录要放到 DA145xx SDK 里才能编译：

```
<SDK>/projects/target_apps/ble_examples/pcmonitor/
```

然后：

```bat
python build.py
```

产物在 `build/pcmonitor.bin`，用 `python tools\flash.py` 烧录。

## 目录说明

| 路径 | 内容 |
|---|---|
| `src/ui/monitor_ui.c` | 监控界面：布局、绘制、逐字符差异刷新 |
| `src/ui/monitor_ui.h` | 界面数据结构 `mon_stats_t` 与对外接口 |
| `src/ui/font_aa.h` | 自动生成的 4bit alpha 抗锯齿字库（勿手改） |
| `src/lcd/st7789.c/.h` | ST7789 软件 SPI 驱动、引脚定义、方向 |
| `src/user_custs1_impl.c` | BLE 长值特征回调：解析 `0xB0/0xB1/0xB2/0xB3` |
| `src/user_peripheral.c` | 启动流程、1Hz 定时器、广播 |
| `src/custom_profile/` | GATT 服务/特征定义（`0xFF00` / `0xFF01`） |
| `src/epd/` | 原厂墨水屏代码，仅保留 `selflash()`、时钟变量等必要部分 |
| `src/config/` | SDK 配置（睡眠、模块裁剪、引脚） |
| `startup/crt0_pcmonitor.c` | 最小 crt0 |
| `sources.txt` / `includes.txt` | 参与编译的源文件 / 头文件搜索路径清单 |
| `build.py` | 一键编译脚本 |

## 关键改动点

1. **屏幕**：`epd_hw_disabled = 1`，改用 `st7789_init()`（两者共用同一组引脚，所以要先调 `epd_hw_init()` 保持变量有效）
2. **墨水屏绘图层**：`clock_draw()` / `QR_draw()` / `LB_draw()` 已改为空实现，
   让 `--gc-sections` 回收 9.5KB 双帧缓冲，腾给 TFT 字库
3. **刷新**：全部在 1Hz 定时器里做，BLE 回调只写变量

详细说明见仓库的 `docs/`。
