# 07 常见问题

## 屏幕

**全黑，什么都没显示**
1. 确认烧录真的成功（`flash.py` 输出 `完成`，且 gdb 没有报错）
2. 手电筒斜照屏幕看有没有微弱图像 → 有图像就是背光没开：检查 `st7789.c` 里 `PWR_MASK/PWR0_MASK` 那 4 个脚的极性
3. 完全没有图像 → SPI 引脚不对，用万用表对一遍 `P0_1 / P2_0 / P2_1 / P0_7`

**颜色是反的（白底、颜色像底片）**
`st7789.c` 里 `0x21`（INVON）和 `0x20`（INVOFF）换一个。IPS 屏和非 IPS 屏极性相反。

**画面镜像 / 上下颠倒**
`st7789.c` 的 `st7789_set_madctl(0x68)` 换值：
`0x28` / `0x68` / `0xA8` / `0xE8` 四种组合，或烧一个 `ui_dir_test(0..3)` 的自检画面来试。
（侧灯版 = `0x60` + `INVON`；中间灯版 = `0x28` + `INVOFF`，由 `PANEL_MIDDLE` 切换）

**马上就有奇怪的花纹、雪花**
软件 SPI 太快。把 `st7789_init()` 里的 `lcd_delay_ms()` 数值加大，或确认 `0x3A=0x55`（RGB565）。

**每秒闪一下**
说明在整屏重绘。检查是不是自己改成了 `st7789_fill()`；本固件默认只做逐字符差异刷新。

**屏幕中间有一条竖线/色带**
那是面板本身的液晶坏点，不是固件问题。用 `UI_CALIB 1` 量出位置，把 `LEFT_X1`/`RIGHT_X0` 调开。

## 烧录

**gdb 报 `Connection timed out`**
- J-Link GDB Server 没起来：看它的日志有没有 `Listening on TCP/IP port 2331`
- 端口被上一次的进程占用：任务管理器结束 `JLinkGDBServerCL.exe` 后重试
- 兼容版 J-Link 偶发：重插 USB、降低 `--speed 1000`、重启 Server

**`Target voltage: 0V`**
SWD 线没接好，或价签没上电/电池没电。

**烧完过一会儿又变回原厂界面**
`selflash()` 没写成功（Flash 写保护/分区只读）。检查 `src/epd/spi_flash.c` 的写入结果打印。

**⚠️ 能把原厂固件刷回来吗**
本仓库不提供原厂镜像。改造前先用 J-Link 备份整片 SPI Flash。

## 电脑端

**`未找到设备`**
- 价签屏幕要有画面（说明固件在跑），只接电不开机是扫不到的
- 电脑蓝牙打开、价签靠近
- 用 nRF Connect 确认能看到 `DLG-CLOCK-`

**`连接失败: TimeoutError` 反复出现**
Windows 上很常见，脚本会自动重试；同时关掉其他占用 BLE 的程序（手机 App、串口助手等）。
USB 蓝牙适配器还可能被系统省电关掉：设备管理器 → 蓝牙 → 属性 → 电源管理 → 取消勾选。

**GPU 一直显示 `--`**
只支持 NVIDIA（走 `nvidia-smi`）。AMD/Intel 集显需要自己换数据源，
在 `pc_agent.py` 的 `gpu_stats()` 里改成读性能计数器。

**CPU 温度看起来偏低（比如一直 25℃）**
Windows 桌面版不公开 CPU 核心（DIE）温度。没有监控软件时，脚本读的是 ACPI 热区
（`ThermalZoneInformation`），很多机器上返回的是主板/进风温度，只能当参考。
想要准确读数：装 **LibreHardwareMonitor**（或 OpenHardwareMonitor）并以管理员身份运行，
脚本会在 60 秒内自动切换数据源，见 [04-pc-agent.md](04-pc-agent.md#6-cpu-温度为什么会偏低以及怎么变准)。
GPU 温度来自 NVIDIA 核心温度，读数是准的。

**网络名显示 `LAN ETH` 而不是 Wi-Fi 名字**
- 说明默认路由走的是有线网卡，这是正常的
- Wi-Fi 名字靠 WLAN API（`WlanQueryInterface`）读取，某些系统策略会拦截，报错时会退回配置文件里的名字

**中文 SSID 变成方块/空白**
固件用位图显示网络名（1bpp）。如果名字特别长会被裁到 92px 宽，
可改 `pc_agent.py` 的 `NET_BMP_W` + `render_label()` 的截断，同时改固件 `NET_BMP_W_MAX`。

**主机名显示不全**
最多 15 字符（`HOST_MAX_CHARS`）。改大要同时确认右栏宽度够。

## 编译

**`region RAM overflowed with stack`**
镜像太大。DA14585 只有 96KB RAM，当前镜像约 55.9KB。
优化方向：减字库、删未使用的数据、或像本项目一样把无用模块改空实现交给 `--gc-sections` 回收。

**`undefined reference to ...`**
`firmware/pcmonitor/sources.txt` 里少了对应的 `.c`（加入后重新编译）。

**改了 `monitor_ui.c` 但屏幕没变**
1. `build.py` 是不是真的重编了（看输出里没有 `cached` 字样）
2. 烧录的是不是新 bin（`flash.py --bin` 路径）
3. 烧完要重新上电一次

## 其它

**耗电 / 会不会把电池耗光**
本固件为了 1Hz 刷新关闭了深度睡眠，耗电明显高于原厂。
建议直接 USB 供电，别用纽扣电池。

**能加别的显示项吗**
能，见 [06-customize.md](06-customize.md) 最后一节（端到端加一个数据项）。
