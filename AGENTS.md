# AGENTS.md — 给 AI Agent 的复刻执行说明

> **本文档是写给 AI Agent 看的。** 人类用户只需要对 Agent 说一句：
>
> ```
> 请根据 https://gitee.com/luo-yihuan/DA14585-ble-monitor 仓库里的 AGENTS.md，
> 帮我把这个项目复刻出来：把固件烧进价签，并让电脑端自动连接推送数据。
> ```
>
> **默认走「路径 A：直接烧录」——不需要下载 SDK、不需要交叉编译器、不需要编译。**
> 固件已经编译好放在 `firmware/release/pcmonitor.bin`，Agent 只需要装 J-Link 和 Python。
> 只有用户想改代码时才走路径 B。

---

## 0. 项目是什么

把 **盒马彩色电子价签**（Dialog/Renesas **DA14585** + **ST7789 240×320 IPS**）
改造成电脑状态副屏：价签离线显示时间/CPU/GPU/内存/C盘/网速/网络名/主机名，
电脑端一个 Python 脚本通过 **BLE** 每秒推送数据。

---

## 1. ⚠️ 先确认面板版本（决定烧哪个固件）

同款价签有两种**灯珠位置**不同的面板，**两种都已适配**，差别只在面板方向 / 颜色顺序：

| 版本 | 特征 | 用哪个固件 |
|---|---|---|
| **侧灯版** | RGB 灯珠在面板**左上角**边缘 | `firmware/release/pcmonitor.bin` |
| **中间灯版** | RGB 灯珠在面板**中部** | `firmware/release/pcmonitor_middle.bin` |

- 两种版本 **LED 引脚完全相同**（红 P0.2 / 绿 P2.5 / 蓝 P2.9，低电平点亮），不用改代码
- 区别只在屏幕初始化：侧灯版 `MADCTL=0x60` + `INVON`；中间灯版 `MADCTL=0x28`（左右镜像 + BGR）+ `INVOFF`
- 烧错版本不会损坏，只是画面左右反 / 颜色偏；换对应 bin 重烧即可

其它前提：

- 屏幕为 **ST7789 240×320 IPS**（不是墨水屏版）
- 面板中部有物理漏液带（逻辑坐标 x100..110），UI 特意留了黑带保护区，**不要删这段黑带**
- 需要 **SEGGER J-Link**（SWD 接线：SWDIO / SWCLK / GND / 3V3）
- 电脑需要 **蓝牙 4.0+**

---

## 2. 两条复刻路径

| | 路径 A：直接烧录（默认，推荐） | 路径 B：自己编译 |
|---|---|---|
| 适用 | 只想复刻现成效果 | 想改界面/协议/引脚 |
| 需要装 | **J-Link + Python** | 另需 DA145xx SDK + Arm GNU Toolchain |
| 固件来源 | 仓库里的 `pcmonitor.bin`（侧灯版）/ `pcmonitor_middle.bin`（中间灯版） | 自己编译产物 |
| 耗时 | 几分钟 | 首次装环境较久 |

> 本仓库**不打包 SDK 和交叉编译器**：体积大（合计约 1 GB），且版权归 Renesas / Arm，
> 不适合二次分发。需要时由 Agent 从官方渠道下载。

---

## 3. 引脚映射（接线核对用）

屏幕与灯珠全部用 GPIO 模拟，定义见
`firmware/pcmonitor/src/lcd/st7789.c` 与 `src/ui/monitor_ui.c`：

### 3.1 ST7789 屏幕（软件 SPI）

| 信号 | DA14585 引脚 | 说明 |
|---|---|---|
| LCD_SCK | **P0_1** | 时钟 |
| LCD_SDA | **P2_0** | 数据（MOSI） |
| LCD_CS | **P2_1** | 片选，低有效 |
| LCD_DC | **P0_7** | 1=数据，0=命令 |
| LCD_RST | **P1_0** | 复位，低有效 |
| PWR_EN | **P2_3** | 屏幕电源使能 |

### 3.2 RGB 状态灯（共阳，低电平点亮）

| 颜色 | 引脚 |
|---|---|
| 红 | **P0_2** |
| 绿 | **P2_5** |
| 蓝 | **P2_9** |

> 灯的默认行为：**平时全灭**；CPU ≥95℃ 连续 5 秒红闪 3 次（10 分钟冷却）；
> 整点（59→00 秒）蓝闪 1 次。不是故障，**不要改成常亮**。

### 3.3 烧录接口

J-Link SWD 四根线：**SWDIO / SWCLK / GND / 3V3**（价签需供电）。

原始 pinout 表见 `docs/pinout.xlsx`（从原厂固件反汇编整理）。

---

## 4. 路径 A：执行清单（Agent 按顺序做）

### A1. 安装 SEGGER J-Link

1. 打开 <https://www.segger.com/downloads/jlink/>，下载
   **J-Link Software and Documentation Pack for Windows（64-bit）**（页面需勾选同意协议）
2. 安装包按默认选项安装即可（会同时装 `JLink.exe`、`JLinkGDBServerCL.exe` 和 USB 驱动）
3. 安装时保持 J-Link 已用 USB 接上电脑，让驱动自动装好

**成功标志**：以下任一路径存在 `JLink.exe`

```
C:\Program Files\SEGGER\JLink\JLink.exe
C:\Program Files (x86)\SEGGER\JLink\JLink.exe
C:\Program Files (x86)\SEGGER\JLink_V630d\JLink.exe
```

> 其它版本目录名也兼容；`tools\flash.py` 会自动搜索，找不到时可用 `--jlink <目录>` 指定。

### A2. 安装 Python 依赖

```bat
git clone https://gitee.com/luo-yihuan/DA14585-ble-monitor.git
cd DA14585-ble-monitor
python -m pip install -r requirements.txt
```

`requirements.txt`：`psutil` / `bleak` / `pillow`。
（只烧录不需要这些依赖；它们在「电脑端推送」时才用。）

### A3. 烧录固件（不编译）

接好 J-Link。**侧灯版**（默认）：

```bat
python tools\flash.py
```

**中间灯版**：

```bat
python tools\flash.py --bin firmware\release\pcmonitor_middle.bin
```

J-Link 装在非标准路径时：

```bat
python tools\flash.py --jlink "C:\Program Files (x86)\SEGGER\JLink_V630d"
```

**成功标志**：

```
固件: ...\firmware\release\pcmonitor.bin (54256 字节)  SP=0x7fd21bc  Reset=0x7fc0511
写入 RAM 并启动...
完成。设备应在 1~2 秒内点亮屏幕并开始广播 DLG-CLOCK-*
```

烧录后价签屏幕立即出现监控界面：左上角黄色「主机未连接」、红色蓝牙图标，
其余是模拟数据 —— 说明固件已跑起来，正在等电脑连接。

> 原理：DA14585 没有片内 Flash，固件平时在价签的 SPI Flash 里。
> 本脚本把 bin 写进 RAM 后启动；固件启动时会自己把镜像写入 SPI Flash 备用分区，
> 所以**断电重启后仍是本固件**，只需烧一次。
> 恢复原厂固件需要原厂镜像，本仓库不提供 —— 烧录前向用户确认。

### A4. 启动电脑端推送

```bat
python tools\pc_agent.py
```

自动扫描名字以 `DLG-CLOCK-` 开头的 BLE 设备并连接，断开后每 2 秒自动重连。
已知 MAC 时可跳过扫描：

```bat
python tools\pc_agent.py 18:BC:5A:5A:87:81
```

**成功标志**：控制台每秒刷一行

```
CPU  63%  GPU  36%  RAM  79.3%  C:  89%  DL 12.3 KB/s  UL 4.5 KB/s  CPU 66C  ... WIFI 你的SSID
```

同时价签左上角出现网络名、底部出现主机名、数值开始跳动。

### A5. 注册开机自启（推荐）

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_pc_agent.ps1 -Addr 18:BC:5A:5A:87:81
```

- 注册计划任务 `PcMonitorAgent`（**不需要管理员权限**），登录后后台运行、无窗口
- `-Addr` 填 A4 里看到的设备 MAC
- 日志：`%LOCALAPPDATA%\pcmonitor\pc_agent.log`
- 卸载：`Unregister-ScheduledTask -TaskName PcMonitorAgent -Confirm:$false`

---

### A6. 上位机配置工具（可选）

```bat
python tools\pc_monitor_gui.py
```

图形界面里可以勾选显示内容、调整屏幕配色、开关 RGB LED、编辑过热/整点规则，
并点「保存到 Flash」写入副屏（断电不丢）。详见 [docs/08-gui.md](docs/08-gui.md)。
注意：上位机与后台 `PcMonitorAgent` 抢占同一条 BLE 连接，关闭上位机后会自动恢复。

---

## 5. 路径 B：自己编译（可选，改代码时才需要）

### B1. 安装 SDK 与工具链

| 软件 | 版本 | 来源 |
|---|---|---|
| DA145xx SDK | **6.0.22.1401** | Renesas 官网搜索 "DA14585 SDK 6.0.22" |
| Arm GNU Toolchain | 12.2 `arm-none-eabi` | <https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads> |

装完把 `arm-none-eabi-gcc` 的 `bin` 目录加入 PATH（或构建时用 `--gcc` 指定）。

### B2. 把工程复制进 SDK

```
xcopy firmware\pcmonitor "<SDK>\projects\target_apps\ble_examples\pcmonitor\" /E /I /Y
```

目录名必须是 `pcmonitor`（`sources.txt` / `includes.txt` 按这个名字写路径）。

### B3. 编译

侧灯版（默认）：

```bat
python "<SDK>\projects\target_apps\ble_examples\pcmonitor\build.py" --sdk "<SDK>"
```

中间灯版（多一个环境变量）：

```bat
set PANEL_MIDDLE=1
python "<SDK>\projects\target_apps\ble_examples\pcmonitor\build.py" --sdk "<SDK>"
```

GCC 不在 PATH 时加 `--gcc "<工具链>\bin"`；也可用环境变量 `DA14585_SDK` 代替 `--sdk`。

**成功标志**：输出 `firmware version: 0xA5XXXXXX` 和
`完成: ...\build\pcmonitor.bin (约 54 KB)`。

### B4. 烧录自己编的固件

```bat
python tools\flash.py --bin "<SDK>\projects\target_apps\ble_examples\pcmonitor\build\pcmonitor.bin"
```

其余步骤同 A4 / A5。

---

## 6. 验收清单（Agent 必须逐条确认）

| # | 检查项 | 判定方法 |
|---|---|---|
| 1 | 屏幕亮、有 UI | 上电后出现时间/CPU/GPU/RAM/C 盘界面 |
| 2 | 未连接提示 | PC 端没连上时，左上角黄字「主机未连接」、蓝牙图标红色 |
| 3 | 已连接 | PC 端连上后，左上角变为网络名（`WIFI XXX` / `LAN ETH`），图标变绿 |
| 4 | 数据实时 | 打开大文件/下载，屏幕 CPU%、网速 1 秒内跟随变化 |
| 5 | 主机名 | 屏幕底部、蓝牙图标右侧显示电脑名（大写，最多 9 字符） |
| 6 | 灯行为 | 平时全灭；≥95℃ 红闪 3 次；整点蓝闪 1 次（不是故障） |

---

## 7. 可选：准确的 CPU 温度

Windows 不公开 CPU 核心温度，默认读 ACPI 热区（多数机器是主板温度）。
需要准确核心温度时：

1. 下载 [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases)，
   解压到 `C:\Tools\LibreHardwareMonitor`（不用运行它）
2. 管理员 PowerShell 执行：
   ```bat
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_temp_daemon.ps1
   ```
3. 脚本注册计划任务 `PcMonitorTemp`（最高权限、隐藏窗口），每 4 秒把核心最高温度写入
   `%ProgramData%\pcmonitor_cputemp.txt`，`pc_agent.py` 自动读取。

---

## 8. 协议速查（调试用）

BLE 长值特征 UUID `0000ff01-0000-1000-8000-00805f9b34fb`，电脑端每秒写入，首字节为命令字：

| 命令 | 含义 | 长度 |
|---|---|---|
| `0xB0` | CPU/RAM/网速/时间/C盘 | 20 |
| `0xB1` | 主机名（连接后一次） | 2+n |
| `0xB2` | GPU% / 网络类型 / CPU、GPU 温度 | 6 |
| `0xB3` | 网络名 1bpp 位图（分块，名称变化时发） | 4+n |

完整字段见 [docs/05-protocol.md](docs/05-protocol.md)。

---

## 9. 常见失败与处理

| 现象 | 原因 / 处理 |
|---|---|
| `找不到 JLink.exe` | 装 SEGGER J-Link，或 `--jlink <安装目录>` |
| 烧录报 `Could not connect` | J-Link USB 未接好 / 驱动未装 / 价签没供电 / SWD 线序错 |
| 烧录成功但屏幕不亮 | 确认烧的是对应面板版本的 bin（侧灯 / 中间）；断电重上电（固件已写 SPI Flash）；确认 J-Link 接线 |
| `未找到设备`（PC 端） | 价签在广播吗（屏幕有界面说明在广播）；电脑蓝牙是否打开；关掉其它 BLE 调试工具 |
| 连接后很快断开 | USB 蓝牙适配器省电：设备管理器 → 蓝牙 → 属性 → 电源管理 → 取消「允许计算机关闭此设备以节约电源」 |
| 屏幕数据不动 | 看 `%LOCALAPPDATA%\pcmonitor\pc_agent.log` 是否在刷新；在跑就重启价签 |
| CPU 温度像主板温度 | 未装 LibreHardwareMonitor，见第 7 节 |
| 编译报错后残留旧 `.o` | 删除 `firmware\pcmonitor\build\*.o` 重试（版本号变化时脚本会自动清） |
| `缺少 SDK 预编译库` | SDK 不完整，确认 `<SDK>\sdk\platform\system_library\output\Keil_5\da14585_586.lib` 存在 |

---

## 10. 约束（Agent 请遵守）

1. **先确认面板版本**，烧对应的 bin（侧灯 / 中间）；两版 LED 引脚相同，不要随意改引脚定义。
2. **不要**删除 UI 中间 x100..110 的黑带（面板坏点保护区）。
3. 改了 `src/` 下任何文件必须重新编译并烧录，否则屏幕行为不会变。
4. 烧录会永久替换价签固件（写入 SPI Flash），执行前向用户确认。
5. 不要提交 `build/` 下的中间产物；仓库只保留 `firmware/release/` 的发布固件。
6. 电脑端脚本只在本机运行，不联网、不上传任何数据。
7. 路径 A 不需要 SDK / 交叉编译器，**不要**为此额外下载 1 GB 的工具链。
