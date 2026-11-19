# 03 烧录固件

## 1. 原理：为什么不是「烧 hex」

DA14585 **没有片内 Flash**。固件平时存在价签的 SPI Flash 里，上电由 OTP bootloader 加载。
用 J-Flash 写 `.hex` 只会写进 RAM，复位就没了。

正确做法是：

1. 用 J-Link 把 `pcmonitor.bin` **直接写进 RAM** 的 `0x07FC0000`
2. 把向量表里的初始 SP / Reset 地址装进 R13 / PC
3. `monitor go` 让 CPU 跑起来

固件跑起来后，启动流程里的 `selflash()` 会把当前 RAM 镜像写进 SPI Flash 的备用分区，
这样**断电重启后依然是本固件**，不用每次重新烧。

> `tools/flash.py` 会在跳转前把 `0x464C5348` 写到 RAM 的 `0x07FD1000`（堆与栈之间的空闲区），
> 固件的 `selflash()` 看到这个标记就强制写入一次 SPI Flash，写完自动清零。
> 不能依赖 `otp_boot`：`read_otp_value()` 里的 `hw_otpc_init()` 会重刷 OTP 影子内存把它覆盖掉。

## 2. 一键烧录（推荐，不需要编译）

**只依赖 SEGGER J-Link**，不需要 SDK、不需要 `arm-none-eabi-gdb`。

接好 J-Link，然后：

```bat
python tools\flash.py
```

默认烧 `firmware\release\pcmonitor.bin`（侧灯版，仓库里已编译好的固件）；
中间灯版烧 `pcmonitor_middle.bin`：

```bat
python tools\flash.py --bin firmware\release\pcmonitor_middle.bin
```

要烧自己编译的版本就加参数：

```bat
python tools\flash.py --bin "<SDK>\projects\target_apps\ble_examples\pcmonitor\build\pcmonitor.bin"
```

它会自动完成：

- 读 bin 前 8 字节，取出初始 SP 和 Reset 地址（每次编译都会变，脚本自动读，不写死）
- 生成 J-Link Commander 脚本并调用 `JLink.exe`：
  `connect` → `r`（复位，清 lockup）→ `loadbin ... 0x07FC0000`
  → 用 CoreSight `DCRDR/DCRSR` 写 SP → `SetPC` 写 PC → `g` 启动
- 检查输出里的 `Downloading file ... O.K.` 和 `Script processing completed`

参数：

```bat
python tools\flash.py --jlink "C:\Program Files (x86)\SEGGER\JLink_V630d"
python tools\flash.py --speed 2000
```

成功的标志：

```
固件: ...\pcmonitor.bin (54256 字节)  SP=0x7fd21bc  Reset=0x7fc0511
写入 RAM 并启动...
完成。设备应在 1~2 秒内点亮屏幕并开始广播 DLG-CLOCK-*
```

> J-Link 的 `wreg` 命令在部分旧版本（如 V6.30d）不可用，所以脚本用 CoreSight 寄存器写 SP，
> PC 用 Commander 自带的 `SetPC` —— 新旧版本 J-Link 都能用。

## 3. 手动烧录（进阶）

在一张纸上记下这两个值，然后直接用 J-Link Commander：

```bat
"JLink.exe" -CommanderScript flash.jlink
```

`flash.jlink` 内容（SP/PC 从 bin 前 8 字节读，换版本要改）：

```
device Cortex-M0
si SWD
speed 1000
connect
r
h
loadbin "D:/xxx/pcmonitor.bin", 0x07FC0000
w4 0xE000EDF8, 0x07FD21BC
w4 0xE000EDF4, 0x0001000D
sleep 100
SetPC 0x07FC0511
g
sleep 2000
qc
```

读这两个值：

```python
import struct
sp, pc = struct.unpack("<II", open("pcmonitor.bin", "rb").read(8))
print(hex(sp), hex(pc))
```

## 4. 烧完怎么验证

1. **屏幕**：1~2 秒内出现界面（左上 `WIFI ...`，右上是 52px 大字时间）
2. **蓝牙**：手机 App（nRF Connect 之类）能扫到 `DLG-CLOCK-xxxx`，服务 `0xFF00`
3. **数据**：电脑上跑 `python tools\pc_agent.py`，屏幕数字开始跳

## 5. 常见失败

| 现象 | 排查 |
|---|---|
| gdb 报 `Connection timed out` / `refused` | GDB Server 没起来或被别的进程占用。先确认日志里出现 `Listening on TCP/IP port 2331`；端口被占就关掉旧的 Server |
| `Target voltage: 0V` | SWD 接线/VCC/GND 问题，或价签没上电 |
| 烧完黑屏 | 检查 `src/lcd/st7789.c` 的 `0x36` 值（方向）与 `0x21/0x20`（反相）；或 SPI 引脚与实际板子不符 |
| 花屏/雪花 | 软件 SPI 时序太快，把 `lcd_delay_ms` 后的延时加大；确认 `0x3A=0x55`（RGB565） |
| 烧完能亮，断电又变回原厂 | `selflash()` 写入 Flash 失败，检查 SPI Flash 是否被写保护；本固件按原厂 A/B 分区写 |
| 反复 `Connection timed out` | 某些 J-Link 兼容版需要先 `-singlerun`，或换 USB 口；也可以重启 GDB Server 后再试 |

## 6. 想恢复原厂？

本仓库**不提供**原厂镜像。改造前请先用 J-Link + 原厂工具备份 SPI Flash，
否则刷回原厂界面会很麻烦（原厂 OTA 走蓝牙 SUOTA，需要官方 App）。
