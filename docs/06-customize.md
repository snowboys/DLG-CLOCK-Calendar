# 06 自定义界面 / 换字体

界面全部在 `firmware/pcmonitor/src/ui/monitor_ui.c`，改完 `python build.py` 再烧录。

## 1. 坐标系

屏幕横屏 **320 × 240**，原点左上，x 向右、y 向下。左上角是 `(0,0)`。

中间 `x 98..112` 是坏点保护区，**不要在那里画东西**（见 01-hardware.md 校准）。

## 2. 布局常量

左栏（网速 + C 盘）宽 4..96，右栏（时钟 + CPU/GPU/RAM）宽 114..316。

| 常量 | 值 | 含义 |
|---|---|---|
| `LEFT_X0 / LEFT_X1` | 4 / 96 | 左栏左右边界 |
| `RIGHT_X0 / RIGHT_X1` | 114 / 316 | 右栏左右边界 |
| `NET_LBL_Y` | 4 | 网络名位图顶边 |
| `DL_LBL_Y / DL_VAL_Y` | 24 / 40 | 下载标签 / 数值 |
| `UL_LBL_Y / UL_VAL_Y` | 72 / 88 | 上传标签 / 数值 |
| `DIV_L_Y` | 120 | 左栏分隔线 |
| `CK_LBL_Y / CK_VAL_Y / CK_BAR_Y` | 126 / 148 / 178 | C 盘标签 / 百分比 / 进度条 |
| `BT_ICON_X / BT_ICON_Y` | 4 / 206 | 蓝牙图标（13×24） |
| `HOST_X / HOST_Y` | 22 / 210 | 主机名（与图标同行，最多 9 字符） |
| `TIME_Y` | 2 | 大字时钟顶边（52px，高 53） |
| `DIV_R_Y` | 60 | 右栏分隔线 |
| `CPU_VAL_Y / CPU_LBL_Y / CPU_BAR_Y` | 68 / 77 / 98 | CPU 三件套 |
| `GPU_VAL_Y / GPU_LBL_Y / GPU_BAR_Y` | 116 / 125 / 146 | GPU 三件套 |
| `RAM_VAL_Y / RAM_LBL_Y / RAM_BAR_Y` | 164 / 173 / 194 | RAM 三件套 |
| `RAM_USED_Y` | 206 | `8.0/16.0GB` 那一行 |
| `TEMP_X` | 146 | 温度文字（在 CPU/GPU 标签右侧） |

标签 y 一般是 `数值 y + 9`，这样两种字号的基线看起来是对齐的：

```
标签(TINY 15px, asc=11)  ┐
数值(MED 27px, asc=20)   ┘← 两条基线都在 数值y+20
```

## 3. 配色

```c
#define C_BG        0x0000   /* 背景黑 */
#define C_LINE      0x2124   /* 分隔线 / 条槽 */
#define C_WHITE     0xFFFF
#define C_CYAN      0x07FF   /* 时钟 */
#define C_GREEN     0x07E0   /* <60% */
#define C_YELLOW    0xFFE0   /* 60~84% */
#define C_RED       0xF800   /* >=85% */
#define C_GRAY      0x8410   /* 标签 / 次要信息 */
```

颜色都是 RGB565。改阈值在 `pct_color()`：

```c
static uint16_t pct_color(int pct)
{
    if (pct < 60) return C_GREEN;
    if (pct < 85) return C_YELLOW;
    return C_RED;
}
```

## 4. 进度条

- 高 **8px**，两端做圆角：逐行内缩 `{2,1,0,0,0,0,1,2}`（`bar_inset[]`）
- 只为差量像素发数据：变长只补新增的一段，变短只擦掉减少的一段
- 条的位置在 `bar_cpu / bar_gpu / bar_ram / bar_ck` 四个 `bar_t` 结构体里，
  想加第四根条就照着抄一个：

```c
static bar_t bar_xxx = { RIGHT_X0, XXX_BAR_Y, RIGHT_X1, -1, 0 };
...
bar_update(&bar_xxx, value, pct_color(value));
```

## 5. 字体

三套字号，全部由 `tools/gen_font.py` 从系统字体 Consolas 离线生成：
**4bit alpha 抗锯齿**，绘制时按 alpha 和黑底混合，所以边缘是平滑灰阶，不是点阵硬边。

| 名字 | 生成尺寸 | 字形 | 用途 | 数据量 |
|---|---|---|---|---|
| `BIG` | 52px | `0123456789:` | 时钟 HH:MM | 8.7 KB |
| `MED` | 26px | 数字 + `:%.MK/BSs-` | 百分比 / 网速 / 秒 | 4.0 KB |
| `TINY` | 14px | 数字 + 大写字母 + 符号 | 标签 / 主机名 | 2.7 KB |

重新生成：

```bat
python tools\gen_font.py
```

脚本会覆盖 `firmware/pcmonitor/src/ui/font_aa.h` 并打印每套的 `adv / h / 字节数`。
**改字号后要同步更新 monitor_ui.c 里的布局常量**（宽度 = 字符数 × adv）。

字体数据格式：

```
每个字形 = adv × h 像素，每像素 4bit alpha，逐行打包
每字节 = 2 像素（高半字节左像素，低半字节右像素）
字库总字节 = 字形数 × h × ceil(adv/2)
```

### 非 ASCII 符号怎么加（以摄氏度 `°` 为例）

字库按**单字节**索引，所以中文/符号不能直接写进 C 字符串。做法是给符号分配一个 ASCII 占位键：

```python
# tools/gen_font.py
SPECS = [..., ("TINY", 14, " ...`")]        # 末尾加占位键 '`'
RENDER_MAP = {"`": "\u00b0"}                # 渲染 '`' 时实际画 '°'
```

固件里写 `"62`C"`，屏幕就会显示 `62°C`。要加别的符号（`±`、`Ω`、`℃`）照这个套路再加一个占位键即可。

### 想换成别的字体

`gen_font.py` 顶部：

```python
CONSOLA = r"C:\Windows\Fonts\consola.ttf"      # 换成任意 ttf/ttc
SPECS = [("BIG", 52, "0123456789:"), ...]      # 字号、字符集
```

> 换字体后 `adv` 会变，务必重新核对左/右栏宽度够不够，避免文字压到中间坏点区。

### 中文字怎么办

固件里没有中文字库（太大）。中文网络名由电脑端渲染成 1bpp 位图发送，见协议 `0xB3`。
如果还想显示其他中文，可以走同一套位图机制，把 `NET_BMP_W/H` 和绘制位置改成自己的区域。

## 6. 刷新策略

`ui_update()` 每秒执行，原则是**只画变化的部分**：

- `text_diff()`：逐字符比较，只重画变了的字符；字符串变短时用空格补掉
- `text_right_diff()`：右对齐版本，长度不变时逐字符比较，长度变了才擦旧区重画
- `bar_update()`：只补/擦差量像素
- 时钟：`HH:MM` 和 `SS` 分开 diff，通常每秒只重画 1~2 个字符
- 网速：`fmt_rate_short()` 输出**固定 6 字符**（不足用前导空格补齐），
  这样单位在 `K/s`↔`M/s` 之间切换时长度不变，只会触发逐字符重绘，不会整行擦除
- 字形块缓冲 `BLIT_ROWS = 53`，一个大字只需要一次 SPI 窗口写入（而不是分 7 块）

不要在 `ui_update()` 里整屏 `st7789_fill()`，软件 SPI 下会明显闪烁。

## 7. 刻度尺自检模式

`monitor_ui.c` 顶部：

```c
#define UI_CALIB  0   /* 改 1：只显示坐标刻度尺 */
```

用途：

1. 量坏点位置（见 01-hardware.md）
2. 确认屏幕方向/偏移是否正确
3. 排线/刷屏是否正常

## 8. 加一个自己的数据项（端到端）

以「CPU 温度」为例：

1. **协议**：`0xB0` 已满 20 字节，另开 `0xB4`：`[1]=温度℃，[2]=保留`
2. **固件接收**：`src/user_custs1_impl.c` 里在 `0xb3` 分支后加 `0xb4` 分支，存到 `g_mon.cpu_temp`
3. **结构体**：`src/ui/monitor_ui.h` 的 `mon_stats_t` 加 `uint8_t cpu_temp;`
4. **绘制**：`monitor_ui.c` 里找个空位（比如主机名右侧）用 `text_diff()` 画出来
5. **电脑端**：`pc_agent.py` 的 `build_ext_packet()` 里多写一包 `0xB4`
