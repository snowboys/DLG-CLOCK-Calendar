# 04 电脑端脚本（pc_agent.py）

电脑端负责：采集本机状态 → 通过蓝牙每秒推给价签。

## 1. 安装

```bat
python -m pip install -r requirements.txt
```

`requirements.txt` 只有三项：

```
psutil>=5.9
bleak>=0.21
pillow>=9.0
```

> Pillow 用来把网络名称（可能含中文）渲染成 1bpp 位图发给屏幕，固件里没有中文字库。

## 2. 直接运行

```bat
python tools\pc_agent.py
```

默认先扫描名字以 `DLG-CLOCK-` 开头的设备，按地址连接，断开后每 2 秒重连。
想跳过扫描，把脚本里的 `DEFAULT_ADDR` 填成自己的 MAC：

```python
DEFAULT_ADDR = "18:BC:5A:5A:87:81"   # 换成你的设备
```

也可以直接命令行指定地址（优先级最高）：

```bat
python tools\pc_agent.py 18:BC:5A:5A:87:81
```

正常输出（每秒一行，覆盖同一行）：

```
CPU  65%  GPU  36%  RAM  79.3%  C:  94%  DL       3.5 KB/s  UL       4.9 KB/s  WIFI 梦创_5G
```

## 3. 采集了哪些数据

| 数据 | 来源 |
|---|---|
| CPU % | `psutil.cpu_percent()` |
| 内存 % / 已用 / 总量 | `psutil.virtual_memory()` |
| 上下行速率 | `psutil.net_io_counters()` 两次采样差 / 时间差 |
| C 盘占用 | `psutil.disk_usage("C:\\")` |
| 时间 | 本机时间 |
| GPU % | `nvidia-smi --query-gpu=utilization.gpu`（仅 NVIDIA；没有则显示 `--`） |
| CPU 温度 | 优先 LibreHardwareMonitor / OpenHardwareMonitor 的 WMI；没有则退回 ACPI 热区（见下） |
| GPU 温度 | `nvidia-smi temperature.gpu`（NVIDIA 核心温度，准确） |
| 网络类型/名称 | 默认路由网卡 + `Get-NetConnectionProfile`（有线/无线）+ WLAN API 读真实 SSID |
| 主机名 | `%COMPUTERNAME%` |

想改成 D 盘或加别的指标，见 [05-protocol.md](05-protocol.md) 和 `build_packet()`。

## 4. 开机自启

**方法 A：启动文件夹（最简单）**

1. `Win + R` 输入 `shell:startup`
2. 在该目录新建 `pcmonitor.bat`：

```bat
@echo off
cd /d C:\path\to\bms-ble-monitor
start "" /min pythonw tools\pc_agent.py
```

用 `pythonw`（无控制台窗口）而不是 `python`。

**方法 B：任务计划程序（更稳）**

1. 打开「任务计划程序」→ 创建任务
2. 常规：勾选「不管用户是否登录都要运行」可省去登录依赖（也可只勾「使用最高权限运行」）
3. 触发器：登录时
4. 操作：程序 `pythonw.exe`，参数 `tools\pc_agent.py`，起始于 `C:\path\to\bms-ble-monitor`
5. 设置：勾选「如果任务失败，按以下频率重新启动：1 分钟」

## 6. CPU 温度为什么会偏低（以及怎么变准）

Windows 桌面版**不对外提供 CPU 核心（DIE）温度**。脚本按优先级取数：

| 优先级 | 数据源 | 说明 |
|---|---|---|
| 1 | 无界面守护写出的 `%ProgramData%\pcmonitor_cputemp.txt` | 准确的核心温度，后台静默运行（推荐） |
| 2 | LibreHardwareMonitor 的本地 Web 服务（`127.0.0.1:8085/data.json`） | 准确，需 GUI 常驻并开启 Web Server |
| 3 | LibreHardwareMonitor / OpenHardwareMonitor 的 WMI | 准确，需软件常驻 |
| 4 | ACPI 热区 `ThermalZoneInformation` | 免安装，多数机器是主板/进风温度（25~35℃，不随负载变化） |

脚本每 5 秒取一次，检测到更优数据源会自动切换，无需改配置。
取到的值会做 **3 点中位数滤波**：传感器偶尔会有一帧 ±20℃ 的毛刺（例如空闲时
60℃ 突然跳一帧 88℃），中位数会把它丢掉，屏幕上就不会出现「一秒涨 40℃」的假象。

**让 CPU 温度变准（推荐：无界面守护，无窗口无托盘）**

1. 下载 [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases) 的 zip，
   解压到 `F:\AI_PROJECTS\DA14585\LibreHardwareMonitor`（不用运行它）
2. 右键「Windows PowerShell」→ **以管理员身份运行**，执行：

```bat
cd /d C:\path\to\bms-ble-monitor
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_temp_daemon.ps1
```

3. 完成。脚本会注册计划任务 `PcMonitorTemp`（登录时以最高权限、隐藏窗口运行），
   每 4 秒把 CPU 核心最高温度写入 `C:\ProgramData\pcmonitor_cputemp.txt`，
   `pc_agent.py` 直接读取。

卸载：

```bat
schtasks /Delete /TN PcMonitorTemp /F
```

> 为什么不用 GUI：LibreHardwareMonitor 的窗口一关，Web 服务和传感器就停了。
> 守护脚本只用它的 `LibreHardwareMonitorLib.dll` 引擎，没有界面，也不会出现在托盘里。
> 如果你更愿意开着 GUI，也可以在菜单里 `Options → Remote Web Server → Run`，脚本同样能读。

GPU 温度走 `nvidia-smi`，不依赖上述组件，读数本来就准。

## 7. 参数调整

`pc_agent.py` 顶部：

| 常量 | 默认 | 说明 |
|---|---|---|
| `DEFAULT_ADDR` | `None` | 填设备 MAC 可跳过扫描，每次连接更快 |
| `INTERVAL` | `1.0` | 上报周期（秒）。屏幕 1Hz 刷新，改小没意义 |
| `HOST_MAX_CHARS` | `15` | 主机名最多 15 字符 |
| `NET_BMP_W/H` | `92 / 15` | 网络名位图尺寸，需和固件一致 |

## 6. 连不上怎么办

| 现象 | 处理 |
|---|---|
| `未找到设备` | 确认价签已上电、屏幕有界面（说明固件在跑）；确认电脑蓝牙打开；把价签靠近电脑 |
| `连接失败/断开: TimeoutError` | 常见于刚开机，脚本会自动重试；多个程序抢连接时先关掉其他 BLE 工具 |
| 反复断开 | USB 蓝牙适配器省电导致，设备管理器 → 蓝牙 → 电源管理 → 取消「允许计算机关闭此设备以节约电源」 |
| 屏幕上数据不动 | 看脚本输出是否还在刷新；如果脚本在跑但屏幕不动，重启价签（拔电再上电） |
