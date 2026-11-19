# 02 编译环境与构建

> **这一步是可选的。** 只想复刻效果的话，直接用仓库里的
> `firmware/release/pcmonitor.bin` + `python tools\flash.py` 烧录即可，不需要本节内容。
> 只有想改界面、协议、引脚时才需要自己编译。

## 1. 需要装什么

| 软件 | 版本 | 用途 | 下载 |
|---|---|---|---|
| DA145xx SDK | **6.0.22.1401** | Dialog/Renesas 官方 SDK | Renesas 官网搜索 "DA14585 SDK 6.0.22" |
| Arm GNU Toolchain | 12.2（`arm-none-eabi`） | 交叉编译器 | developer.arm.com → Downloads → GNU Toolchain |
| Python | 3.10+ | 跑构建脚本 / PC 端脚本 | python.org |

安装完把 `arm-none-eabi-gcc` 的 `bin` 目录加入 PATH（或用 `build.py --gcc` 指定）。

## 2. 摆放工程

SDK 解压后目录结构是 `DA145xx_SDK/6.0.22.1401/sdk/...`。
把本仓库的固件目录复制成：

```
<SDK>/projects/target_apps/ble_examples/pcmonitor/
```

最终应该长这样：

```
DA145xx_SDK/6.0.22.1401/
├─ sdk/
└─ projects/target_apps/ble_examples/pcmonitor/
   ├─ build.py
   ├─ sources.txt
   ├─ includes.txt
   ├─ startup/crt0_pcmonitor.c
   └─ src/
```

> 目录名必须是 `pcmonitor`，因为 `sources.txt` / `includes.txt` 里的路径是按这个名字写的。

## 3. 编译

```bat
cd /d <SDK>\projects\target_apps\ble_examples\pcmonitor
python build.py
```

脚本会自动：

1. 在脚本所在目录向上 4 级找到 SDK（也可以 `--sdk D:\...\6.0.22.1401` 指定）
2. 在 PATH / 常见安装目录里找 `arm-none-eabi-gcc`
3. 按 `sources.txt`（82 个源文件）并行编译，按 `includes.txt`（149 个头文件路径）搜索
4. 预处理链接脚本 `ldscript_DA14585_586.lds.S`
5. 首次运行会把 SDK 自带的 Keil 静态库 `da14585_586.lib` 解包成 `.o`（GCC 不能直接链 Keil 库）
6. 链接并生成 `build/pcmonitor.bin` 与 `.hex`

**中间灯版**（面板方向/颜色顺序不同）：先设置环境变量再编译：

```bat
set PANEL_MIDDLE=1
python build.py
```

侧灯版不需要设置（默认 `PANEL_MIDDLE=0`）。两版的差异只在 `src/lcd/st7789.c` 的
`MADCTL` 与 `INVON/INVOFF`，LED 引脚完全相同。

成功输出示例：

```
SDK : D:\SDK_6.0.22.1401\DA145xx_SDK\6.0.22.1401
GCC : C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin
编译 85 个文件...
已解包 SDK 预编译库 -> build\lib_extract

完成: ...\build\pcmonitor.bin (55936 字节)
```

## 4. RAM 预算（改东西前必看）

DA14585 只有 **96KB RAM**，而且代码就是跑在 RAM 里的，所以「镜像大小」直接就是内存占用：

| 项目 | 大小 |
|---|---|
| 当前镜像 | **≈55.9 KB** |
| 留给堆 + 栈 | ≈13 KB（栈 1.5KB） |

所以：

- 加字库、加图片前先估算，**不要超过 ~66KB**，否则链接会报
  `region RAM overflowed with stack` 或 `section .bss will not fit in region LR_IROM3`。
- 大字号不要存整套字库，可以用 2 倍插值放大（更费 CPU）或删掉无用的大数组。
- 本项目为了腾空间，已经把停用的墨水屏绘图层（`clock_draw`/`QR_draw`/`LB_draw`）改成空实现，
  让链接器 `--gc-sections` 回收 `epd_gui` 的双帧缓冲（9.5KB）和整套 EPD 字库表。

## 5. 改了界面后先预览再烧录

不想每次都烧录试错的话，可以先在电脑上渲染 1:1 预览：

```bat
python tools\preview_ui.py
```

输出 `tools/ui_preview.png`（2 倍放大）。脚本里的布局常量和 `monitor_ui.c` 一一对应，
改完记得两边同步。

## 5.1 资源占用与优化开关

- **固件版本号自动生成**：`build.py` 对「全部源码 + `src` 下所有头文件」取 md5，
  生成 `0xA5xxxxxx` 并通过 `-DEPD_VERSION` 传给编译器。`selflash()` 靠这个版本号
  判断要不要把新固件写进 Flash，所以改头文件（引脚/开关）也会触发更新。
- **串口日志开关**：`src/config/user_config.h` 里的 `PC_MONITOR_DEBUG`，
  默认 `0`（关闭）。关闭后 `printk(...)` 被预处理成空语句，格式字符串、
  arch_console 调用一并消失；调试时改成 `1` 重编即可。
- 关日志后镜像从 56.7KB 降到 53.8KB（约省 2.9KB RAM/Flash）。

## 6. 常见编译错误

| 报错 | 原因 / 解决 |
|---|---|
| `找不到 SDK` | 工程没放到 `<SDK>/projects/target_apps/ble_examples/pcmonitor`，或用 `--sdk` 指定 |
| `找不到 arm-none-eabi-gcc` | 工具链没进 PATH，或用 `--gcc "C:\...\bin"` |
| `undefined reference to ui_init` | `sources.txt` 里缺 `src/ui/monitor_ui.c` |
| `region RAM overflowed with stack` | 镜像太大，删点数据（见上节预算） |
| `section RET_DATA overlaps .bss` | 同上，RAM 溢出 |
| `缺少 SDK 预编译库` | SDK 路径不对，或 SDK 版本不是 6.0.22.1401 |
