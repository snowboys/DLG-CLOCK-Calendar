# -*- coding: utf-8 -*-
"""
pc_agent.py — 采集本机 CPU / 内存 / 网络速度, 通过 BLE 推送到盒马彩色电子价签(DA14585)

用法:
    python tools\\pc_agent.py                 # 自动扫描 DLG-CLOCK-* 设备
    python tools\\pc_agent.py 18:BC:5A:5A:87:81   # 指定设备地址

协议: 写入 0xFF01 长值特征 (write request), 20 字节:
    [0]      = 0xB0              命令字
    [1]      = CPU 使用率 %      0..100
    [2]      = RAM 使用率 %      0..100
    [3]      = flags (bit0=在线) 固定 0x01
    [4..5]   = RAM 已用 MB       uint16 LE
    [6..7]   = RAM 总量 MB       uint16 LE
    [8..11]  = 下载速度 B/s      uint32 LE
    [12..15] = 上传速度 B/s      uint32 LE
    [16..18] = 时 / 分 / 秒
    [19]     = C 盘占用 %        0..100

连接后会额外写入一包 0xB1 主机名包 (可变长):
    [0]      = 0xB1
    [1]      = 字符数
    [2..]    = 主机名 ASCII (最多 15 字符)

每秒还会写入一包 0xB2 扩展包 (GPU + 网络):
    [0]      = 0xB2
    [1]      = GPU 使用率 %     (0xFF = 采集不到)
    [2]      = 网络类型         0=未知 1=有线 2=WiFi
    [3]      = 名称长度
    [4..]    = WiFi SSID 或网卡名 ASCII (最多 15 字符)
"""
import sys
import os

# pythonw / 计划任务运行时没有控制台: 在任何 import 之前就把 stdout/stderr
# 落到日志, 否则 import 失败或异常会随空 stdout 一起丢失, 任务只剩退出码 1。
if (sys.stdout is None or sys.stderr is None
        or os.path.basename(sys.executable).lower().startswith("pythonw")):
    _logdir = os.path.join(os.environ.get("LOCALAPPDATA") or os.getcwd(), "pcmonitor")
    try:
        os.makedirs(_logdir, exist_ok=True)
        _lf = open(os.path.join(_logdir, "pc_agent.log"), "a", buffering=1, encoding="utf-8")
        sys.stdout = _lf
        sys.stderr = _lf
    except OSError:
        pass

import asyncio
import ctypes
import ctypes.wintypes as wt
import datetime
import json
import re
import shutil
import socket
import subprocess
import threading
import time
import urllib.request

import psutil
from bleak import BleakClient, BleakScanner
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cards import CardPusher          # 屏幕卡片 (天气/TODO/画册)

# 子进程不要弹控制台窗口 (后台/计划任务运行时尤其重要)
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0

CHAR_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"   # SVC1_IDX_LONG_VALUE_VAL
NAME_PREFIX = "DLG-CLOCK-"
DEFAULT_ADDR = None                                   # 填自己的设备 MAC 可省去扫描

INTERVAL = 1.0                                        # 上报周期（秒）
HOST_MAX_CHARS = 15                                   # 屏幕右栏最多 15 字符


async def find_device(preferred: str | None) -> str | None:
    if preferred:
        return preferred
    print("扫描 BLE 设备（8 秒）...")
    devices = await BleakScanner.discover(timeout=8.0)
    for d in devices:
        name = d.name or ""
        if name.startswith(NAME_PREFIX):
            print(f"找到设备: {name}  {d.address}")
            return d.address
    return None


def build_packet(prev_io, prev_t):
    now = time.time()
    io = psutil.net_io_counters()
    dt = max(now - prev_t, 0.001)
    dl = int(max(0, io.bytes_recv - prev_io.bytes_recv) / dt)
    ul = int(max(0, io.bytes_sent - prev_io.bytes_sent) / dt)
    cpu = int(round(psutil.cpu_percent(interval=None)))
    vm = psutil.virtual_memory()

    total_mb = min(int(vm.total // (1024 * 1024)), 0xFFFF)
    used_mb = min(int(vm.used // (1024 * 1024)), 0xFFFF)
    dl = min(dl, 0xFFFFFFFF)
    ul = min(ul, 0xFFFFFFFF)
    lt = time.localtime(now)
    try:
        disk_pct = int(round(psutil.disk_usage("C:\\").percent))
    except Exception:
        disk_pct = 0

    p = bytearray(20)
    p[0] = 0xB0
    p[1] = max(0, min(100, cpu))
    p[2] = max(0, min(100, int(round(vm.percent))))
    p[3] = 0x01
    p[4] = used_mb & 0xFF
    p[5] = (used_mb >> 8) & 0xFF
    p[6] = total_mb & 0xFF
    p[7] = (total_mb >> 8) & 0xFF
    for i in range(4):
        p[8 + i] = (dl >> (8 * i)) & 0xFF
        p[12 + i] = (ul >> (8 * i)) & 0xFF
    p[16] = lt.tm_hour
    p[17] = lt.tm_min
    p[18] = lt.tm_sec
    p[19] = max(0, min(100, disk_pct))

    return p, io, now, cpu, vm.percent, dl, ul, disk_pct


async def send_host_name(client: BleakClient):
    name = (os.environ.get("COMPUTERNAME") or "PC").upper().replace("_", "-")
    b = name.encode("ascii", "ignore")[:HOST_MAX_CHARS]
    p = bytearray(2 + len(b))
    p[0] = 0xB1
    p[1] = len(b)
    p[2:] = b
    await client.write_gatt_char(CHAR_UUID, p, response=True)


def build_clock_packet():
    """0x91 对时包: 公历 年/月(0-based)/日/时/分/秒。

    万年历固件收到后自己反算农历、星期、节气、节日, 后 4 字节只是占位,
    蓝牙断开后设备凭本地 RTC 独立走时, 不依赖电脑常开。
    """
    n = datetime.datetime.now()
    return bytes([
        0x91,
        n.year & 0xFF, (n.year >> 8) & 0xFF,
        n.month - 1, n.day,
        n.hour, n.minute, n.second,
        (n.weekday() + 1) % 7,       # 占位: 固件自行按公历计算
        0, 0, 0,                     # 占位: 农历由固件自行反算
    ])


_last_clock_sync = 0.0     # 跨连接保持: 万年历靠本地 RTC 走时, 不必每次重连都对时


async def send_clock_sync(client: BleakClient):
    global _last_clock_sync
    if time.time() - _last_clock_sync < 600:   # 10 分钟内的重连不再对时
        return
    await client.write_gatt_char(CHAR_UUID, build_clock_packet(), response=True)
    _last_clock_sync = time.time()
    print(f"已对时: {datetime.datetime.now():%Y-%m-%d %H:%M:%S}")


NVIDIA_SMI = shutil.which("nvidia-smi")
NET_FONT = r"C:\Windows\Fonts\msyh.ttc"      # 微软雅黑, 用于渲染含中文的网络名
NET_BMP_W, NET_BMP_H = 92, 15                # 与左栏宽度/行高一致


def gpu_stats():
    """返回 (GPU 利用率 0..100|255 未知, GPU 温度 ℃|255 未知)"""
    if not NVIDIA_SMI:
        return 255, 255
    try:
        r = subprocess.run(
            [NVIDIA_SMI, "--query-gpu=utilization.gpu,temperature.gpu",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=2,
            creationflags=CREATE_NO_WINDOW)
        line = r.stdout.strip().splitlines()
        if not line:
            return 255, 255
        parts = [p.strip() for p in line[0].split(",")]
        u = int(parts[0]) if parts and parts[0].isdigit() else 255
        t = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 255
        if u != 255:
            u = min(100, u)
        if not (0 < t < 120):
            t = 255
        return u, t
    except Exception:
        return 255, 255


# ---- CPU 温度 ----
# Windows 桌面版不公开 CPU 核心温度, 优先级:
#   1) LibreHardwareMonitor / OpenHardwareMonitor 的 WMI (需常驻运行, 读数准确)
#   2) ACPI 热区 (ThermalZoneInformation) —— 免安装, 但多数机器是主板/进风温度
_cpu_temp = [255]
_cpu_src = ["?"]          # lhm / ohm / acpi
_probe = [0]
_warned = [False]

LHM_NS = (("root/LibreHardwareMonitor", "lhm"), ("root/OpenHardwareMonitor", "ohm"))
LHM_URL = "http://127.0.0.1:8085/data.json"   # LibreHardwareMonitor 的 Remote Web Server
TEMP_FILE = os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"),
                         "pcmonitor_cputemp.txt")   # 无界面守护写出的温度


def _file_temp():
    """读取无界面守护 (temp_daemon.ps1) 写出的 CPU 核心温度, 30 秒内有效"""
    try:
        if time.time() - os.path.getmtime(TEMP_FILE) > 30:
            return None, 255
        with open(TEMP_FILE) as f:
            v = int(f.read().strip())
        if 0 < v < 120:
            return "lhm-file", v
    except Exception:
        pass
    return None, 255


def _lhm_http_temp():
    """从 LibreHardwareMonitor 的本地 Web 服务抓 CPU 核心温度最大值"""
    try:
        with urllib.request.urlopen(LHM_URL, timeout=1.5) as r:
            data = json.loads(r.read().decode("utf-8", "ignore"))
    except Exception:
        return None, 255

    vals = []

    def walk(node):
        if isinstance(node, dict):
            if node.get("Type") == "Temperature" and "Core" in str(node.get("Text", "")):
                m = re.match(r"[-+]?\d+(\.\d+)?", str(node.get("Value", "")))
                if m:
                    vals.append(float(m.group()))
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    vals = [v for v in vals if 0 < v < 120]
    if vals:
        return "lhm-http", int(round(max(vals)))
    return None, 255


def _ps_out(script):
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "[Console]::OutputEncoding=[Text.Encoding]::UTF8; " + script],
                       capture_output=True, text=True, timeout=12,
                       encoding="utf-8", errors="ignore",
                       creationflags=CREATE_NO_WINDOW)
    return r.stdout.strip()


def _num(text):
    try:
        return float(text.strip().splitlines()[0])
    except Exception:
        return None


def _core_temp():
    """从硬件监控软件的 WMI 里取核心温度最大值"""
    for ns, tag in LHM_NS:
        out = _ps_out(
            f"Get-CimInstance -Namespace {ns} -ClassName Sensor -ErrorAction SilentlyContinue | "
            "Where-Object { $_.SensorType -eq 'Temperature' -and $_.Name -match 'Core' } | "
            "Measure-Object -Property Value -Maximum | Select-Object -ExpandProperty Maximum")
        v = _num(out)
        if v is not None and 0 < v < 120:
            return tag, int(round(v))
    return None, 255


def _acpi_temp():
    out = _ps_out(
        "Get-CimInstance -ClassName Win32_PerfFormattedData_Counters_ThermalZoneInformation "
        "| Select-Object -First 1 -ExpandProperty Temperature")
    v = _num(out)
    if v is not None:
        c = v - 273.15
        if 0 < c < 120:
            return "acpi", int(round(c))
    return None, 255


_temp_hist = []        # 最近几次温度采样, 用于中位数滤波
_temp_hist_tag = [None]


def _smooth_temp(tag, val):
    """3 点中位数滤波: 抑制传感器偶发的 ±20℃ 单帧毛刺 (换数据源时重新开始)"""
    if tag != _temp_hist_tag[0]:
        _temp_hist.clear()
        _temp_hist_tag[0] = tag
    _temp_hist.append(val)
    if len(_temp_hist) > 3:
        _temp_hist.pop(0)
    return sorted(_temp_hist)[len(_temp_hist) // 2]


def _cpu_temp_loop():
    while True:
        tag, val = _file_temp()                 # 无界面守护 (最省事)
        if tag is None:
            tag, val = _lhm_http_temp()         # LHM 开了 Web Server
        # 已锁定 LHM/OHM 就只查它; 否则每 60 秒重新探测一次 (用户可能刚打开监控软件)
        if tag is None and (_cpu_src[0] in ("lhm", "ohm") or _probe[0] % 12 == 0):
            tag, val = _core_temp()
        if tag is None:
            tag, val = _acpi_temp()
        if tag:
            _cpu_src[0] = tag
            _cpu_temp[0] = _smooth_temp(tag, val)
            if tag == "acpi" and not _warned[0]:
                _warned[0] = True
                print("\n[提示] 未检测到 LibreHardwareMonitor 数据,"
                      " CPU 温度显示的是主板热区温度, 不是核心温度 (见 docs/04-pc-agent.md)",
                      flush=True)
        _probe[0] += 1
        time.sleep(5)


_temp_thread = None


def start_cpu_temp():
    """后台线程采集 CPU 温度 (PowerShell 调用较慢, 每 5 秒一次, 不阻塞推送)"""
    global _temp_thread
    if _temp_thread is None:
        _temp_thread = threading.Thread(target=_cpu_temp_loop, daemon=True)
        _temp_thread.start()


def _active_iface():
    """默认路由出口 IP 对应的网卡名"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
        finally:
            s.close()
    except Exception:
        return None
    try:
        for n, addrs in psutil.net_if_addrs().items():
            for a in addrs:
                if a.family == socket.AF_INET and a.address == ip:
                    return n
    except Exception:
        pass
    return None


def _profiles():
    """接口别名 -> 网络名(SSID)。netsh wlan 需要定位权限, 这里改用连接配置文件"""
    try:
        ps = ("[Console]::OutputEncoding=[Text.Encoding]::UTF8; "
              "Get-NetConnectionProfile | ForEach-Object { $_.InterfaceAlias + [char]9 + $_.Name }")
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                           capture_output=True, text=True, timeout=8,
                           encoding="utf-8", errors="ignore",
                           creationflags=CREATE_NO_WINDOW)
        out = {}
        for line in r.stdout.splitlines():
            if "\t" in line:
                alias, name = line.split("\t", 1)
                out[alias.strip()] = name.strip()
        return out
    except Exception:
        return {}


# ---- WLAN API: 读取当前连接的真实 SSID (连接配置文件名字可能带 Windows 去重后缀 " 7") ----
class _GUID(ctypes.Structure):
    _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD),
                ("Data3", wt.WORD), ("Data4", ctypes.c_ubyte * 8)]


class _SSID(ctypes.Structure):
    _fields_ = [("uSSIDLength", wt.ULONG), ("ucSSID", ctypes.c_ubyte * 32)]


class _ASSOC(ctypes.Structure):
    _fields_ = [("dot11Ssid", _SSID), ("dot11BssType", ctypes.c_uint),
                ("dot11Bssid", ctypes.c_ubyte * 6), ("dot11PhyType", ctypes.c_uint),
                ("uDot11PhyIndex", wt.ULONG),
                ("wlanSignalQuality", wt.ULONG),
                ("ulRxRate", wt.ULONG), ("ulTxRate", wt.ULONG)]


class _SEC(ctypes.Structure):
    _fields_ = [("bSecurityEnabled", wt.BOOL),
                ("bOneXEnabled", wt.BOOL),
                ("dot11AuthAlgorithm", ctypes.c_uint),
                ("dot11CipherAlgorithm", ctypes.c_uint)]


class _CONN(ctypes.Structure):
    _fields_ = [("isState", ctypes.c_uint), ("wlanConnectionMode", ctypes.c_uint),
                ("strProfileName", ctypes.c_wchar * 256),
                ("assoc", _ASSOC), ("sec", _SEC)]


class _IFACE(ctypes.Structure):
    _fields_ = [("InterfaceGuid", _GUID), ("desc", ctypes.c_wchar * 256),
                ("state", wt.DWORD)]


def wifi_ssid() -> str:
    """当前连接的真实 SSID (不含 Windows 的去重序号); 未连接返回空串"""
    try:
        wlan = ctypes.windll.wlanapi
        handle = wt.HANDLE()
        ver = wt.DWORD()
        if wlan.WlanOpenHandle(2, None, ctypes.byref(ver), ctypes.byref(handle)) != 0:
            return ""
        try:
            p_list = ctypes.c_void_p()
            if wlan.WlanEnumInterfaces(handle, None, ctypes.byref(p_list)) != 0:
                return ""
            count = ctypes.cast(p_list, ctypes.POINTER(wt.DWORD))[0]
            base = p_list.value + 8          # 跳过 dwNumberOfItems / dwIndex
            for i in range(count):
                info = ctypes.cast(base + i * ctypes.sizeof(_IFACE),
                                   ctypes.POINTER(_IFACE)).contents
                data = ctypes.c_void_p()
                size = wt.DWORD()
                opcode = wt.DWORD()
                rc = wlan.WlanQueryInterface(handle, ctypes.byref(info.InterfaceGuid), 7,
                                             None, ctypes.byref(size),
                                             ctypes.byref(data), ctypes.byref(opcode))
                if rc == 0 and data:
                    conn = ctypes.cast(data, ctypes.POINTER(_CONN)).contents
                    if conn.isState == 1 and conn.assoc.dot11Ssid.uSSIDLength:
                        n = conn.assoc.dot11Ssid.uSSIDLength
                        raw = bytes(conn.assoc.dot11Ssid.ucSSID[:n])
                        wlan.WlanFreeMemory(data)
                        return raw.decode("utf-8", "ignore").strip()
                    wlan.WlanFreeMemory(data)
            return ""
        finally:
            wlan.WlanCloseHandle(handle, None)
    except Exception:
        return ""


def net_label() -> str:
    """网络标签文本 (可能含中文, 由 PC 渲染成位图下发)"""
    active = _active_iface()
    prof = {}
    if active is None:
        prof = _profiles()
        if len(prof) == 1:
            active = next(iter(prof))
    if not active:
        return "LAN ETH"
    wireless = any(k in active.upper() for k in ("WLAN", "WI-FI", "WIFI", "无线"))
    if wireless:
        return "WIFI " + (wifi_ssid() or prof.get(active, "") or "4G")
    name = prof.get(active) or (active if active.isascii() else "ETH")
    return "LAN " + name


def render_label(text: str) -> bytes:
    """把文本渲染成 1bpp 位图: 每行 (W+7)/8 字节, 逐行, 高位在左"""
    f = ImageFont.truetype(NET_FONT, 12)
    img = Image.new("L", (NET_BMP_W, NET_BMP_H), 0)
    ImageDraw.Draw(img).text((0, -1), text, font=f, fill=255)
    px = img.load()
    data = bytearray()
    for y in range(NET_BMP_H):
        for xb in range(0, NET_BMP_W, 8):
            b = 0
            for k in range(8):
                x = xb + k
                if x < NET_BMP_W and px[x, y] >= 100:
                    b |= 0x80 >> k
            data.append(b)
    return bytes(data)


async def send_net_bitmap(client: BleakClient, text: str):
    data = render_label(text)
    total = len(data)
    off = 0
    while off < total:
        chunk = data[off:off + 16]
        p = bytearray(4 + len(chunk))
        p[0] = 0xB3
        p[1] = NET_BMP_W
        p[2] = total
        p[3] = off
        p[4:] = chunk
        await client.write_gatt_char(CHAR_UUID, p, response=True)
        off += len(chunk)


_label_cache = [0.0, "LAN ETH"]


def _cached_label(ttl=15.0) -> str:
    """网络标签缓存: Get-NetConnectionProfile 启动 PowerShell 较慢, 15 秒刷新一次"""
    now = time.time()
    if now - _label_cache[0] > ttl:
        try:
            _label_cache[1] = net_label()
        except Exception:
            pass
        _label_cache[0] = now
    return _label_cache[1]


def build_ext_packet() -> bytearray:
    util, gtemp = gpu_stats()
    ntype = 2 if _cached_label().startswith("WIFI") else 1
    p = bytearray(6)
    p[0] = 0xB2
    p[1] = util
    p[2] = ntype
    p[3] = 0
    p[4] = _cpu_temp[0]
    p[5] = gtemp
    return p


async def session(client: BleakClient):
    global _last_clock_sync
    prev_io = psutil.net_io_counters()
    prev_t = time.time()
    psutil.cpu_percent(interval=None)          # 预热
    start_cpu_temp()
    # Windows/WinRT 在连接建立后仍需短暂收尾, 立刻写首包容易触发瞬时断链
    await asyncio.sleep(0.5)
    for _attempt in range(3):
        try:
            await send_host_name(client)
            break
        except Exception:
            if not client.is_connected:
                raise
            await asyncio.sleep(0.5)
    else:
        raise RuntimeError("hostname write failed")

    # 万年历: 连上后对一次时, 之后每 30 分钟再校准一次 (补偿晶振漂移)
    for _attempt in range(3):
        try:
            await send_clock_sync(client)
            break
        except Exception:
            if not client.is_connected:
                raise
            await asyncio.sleep(0.5)
    await asyncio.sleep(INTERVAL)

    label_prev = None
    cards = CardPusher()                       # 卡片 (监控/时钟/天气/TODO/画册/万年历)
    while client.is_connected:
        p, prev_io, prev_t, cpu, ram_pct, dl, ul, disk_pct = build_packet(prev_io, prev_t)
        ext = build_ext_packet()
        label = _cached_label()
        try:
            await client.write_gatt_char(CHAR_UUID, p, response=True)
            await client.write_gatt_char(CHAR_UUID, ext, response=True)
            if label != label_prev:
                await send_net_bitmap(client, label)
                label_prev = label
            await cards.tick(lambda b: client.write_gatt_char(CHAR_UUID, b, response=True))
            if time.time() - _last_clock_sync >= 1800:
                await client.write_gatt_char(CHAR_UUID, build_clock_packet(), response=True)
                _last_clock_sync = time.time()
                print("\n已校准时钟")
        except Exception as e:
            print(f"\n写入失败: {e}")
            break
        gpu_s = f"{ext[1]:3d}%" if ext[1] <= 100 else "  -- "
        t_s = (f"CPU {ext[4]:2d}C  GPU {ext[5]:2d}C"
               if ext[4] <= 120 else "--")
        print(
            f"CPU {cpu:3d}%  GPU {gpu_s}  RAM {ram_pct:5.1f}%  C: {disk_pct:3d}%  "
            f"DL {dl/1024:9.1f} KB/s  UL {ul/1024:9.1f} KB/s  {t_s}  {label}"
            .ljust(130),
            end="\r",
            flush=True,
        )
        await asyncio.sleep(INTERVAL)


async def main():
    preferred = sys.argv[1] if len(sys.argv) > 1 else None
    if preferred is None and DEFAULT_ADDR:
        preferred = DEFAULT_ADDR          # 默认先试已知地址，失败再扫描

    while True:
        addr = await find_device(preferred)
        if not addr:
            print("未找到设备，15 秒后重试...")
            preferred = None
            await asyncio.sleep(15)
            continue

        print(f"连接 {addr} ...")
        try:
            async with BleakClient(addr, timeout=60.0) as client:   # 服务发现可能需 20~30s(固件刷屏挤占主循环), 20s 会超时
                print("已连接，开始推送（Ctrl+C 退出）")
                await session(client)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            print(f"\n连接失败/断开: {e}")

        preferred = None                  # 断开后改为扫描
        print("15 秒后重连...")           # 间隔放宽: Windows 适配器在急速重连风暴下会进入坏状态
        await asyncio.sleep(15)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n已退出")
    except Exception:
        import traceback
        traceback.print_exc()
        raise

