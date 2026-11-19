# -*- coding: utf-8 -*-
"""pc_monitor_gui.py — 副屏配置工具（Tkinter 上位机）

功能:
  - 通过 BLE 连接价签副屏，并持续推送电脑状态数据（等同于 pc_agent）
  - 选择副屏显示哪些信息 / 调整屏幕配色
  - 开关 RGB LED、测试颜色与闪烁、保存自定义灯效
  - 配置过热 / 整点规则，并可保存到副屏 Flash（断电不丢）

用法:
    python tools\\pc_monitor_gui.py
    python tools\\pc_monitor_gui.py 18:BC:5A:38:28:81
"""
import atexit
import json
import os
import queue
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import colorchooser, filedialog, messagebox, ttk

import psutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pc_agent as agent
import ota
import cards
import weather

from bleak import BleakClient

CONFIG_PATH = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")),
                           "pcmonitor", "gui_config.json")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_FLAG = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")),
                        "pcmonitor", "gui_active.flag")

CFG_FMT = "<IHH9HBBBBHBBBB12sH"      # 50 字节: 比 v1 多了 card 字段
CFG_SIZE = struct.calcsize(CFG_FMT)
CFG_MAGIC = 0x314D4350

TASK = "PcMonitorAgent"     # 后台推送的计划任务名


def _run_schtasks(*args):
    """调用 schtasks; 不解析输出 (中文系统是 GBK, 解码会抛异常)"""
    try:
        return subprocess.run(["schtasks"] + list(args), stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL,
                              creationflags=0x08000000).returncode
    except Exception:
        return 1


def task_running():
    """判断后台推送进程是否真的在跑 (按进程命令行, 比 schtasks 返回码可靠)"""
    try:
        for p in psutil.process_iter(["name", "cmdline"]):
            name = (p.info.get("name") or "").lower()
            if not name.startswith("python"):
                continue
            cmd = p.info.get("cmdline") or []
            if any("pc_agent.py" in str(a) for a in cmd):
                return True
    except Exception:
        pass
    return False


def task_stop():
    """结束后台推送任务 + 清掉残留的 pythonw 推送进程
    (计划任务通过 cmd 启动时, /End 只结束外层, pythonw 仍占着 BLE)"""
    _run_schtasks("/End", "/TN", TASK)
    _run_schtasks("/Change", "/TN", TASK, "/DISABLE")   # 防止失败自动重启
    try:
        for p in psutil.process_iter(["name", "cmdline"]):
            name = (p.info.get("name") or "").lower()
            if not name.startswith("python"):
                continue
            cmd = p.info.get("cmdline") or []
            if any("pc_agent.py" in str(a) for a in cmd):
                p.kill()
    except Exception:
        pass


def task_start():
    _run_schtasks("/Change", "/TN", TASK, "/ENABLE")
    _run_schtasks("/Run", "/TN", TASK)

# ---------------- 视觉 ----------------
BG        = "#f4f6fb"
CARD      = "#ffffff"
INK       = "#1f2937"
MUTED     = "#6b7280"
ACCENT    = "#2563eb"
ACCENT_D  = "#1d4ed8"
OK        = "#16a34a"
WARN      = "#d97706"
BAD       = "#dc2626"
HEADER_BG = "#1f2937"
HEADER_FG = "#ffffff"
FONT      = ("Microsoft YaHei UI", 10)
FONT_B    = ("Microsoft YaHei UI", 10, "bold")
FONT_T    = ("Microsoft YaHei UI", 15, "bold")
FONT_S    = ("Microsoft YaHei UI", 9)

FIELDS = [
    ("时钟", 1 << 0), ("CPU 占用", 1 << 1), ("GPU 占用", 1 << 2),
    ("内存", 1 << 3), ("C 盘", 1 << 4), ("网速", 1 << 5),
    ("网络名", 1 << 6), ("主机名", 1 << 7), ("CPU 温度", 1 << 8), ("GPU 温度", 1 << 9),
]

PALETTES = [
    ("背景", 0), ("主文字", 1), ("次文字", 2), ("分隔线", 3),
    ("进度条-低", 4), ("进度条-中", 5), ("进度条-高", 6), ("时间", 7), ("强调色", 8),
]

LED_COLORS = [("红", 1), ("绿", 2), ("蓝", 4), ("黄", 3), ("青", 6), ("紫", 5), ("白", 7)]

DEFAULTS = {
    "field_mask": 0x03FF,
    "pal": [0x0000, 0xFFFF, 0x8410, 0x2124, 0x07E0, 0xFFE0, 0xF800, 0xFFFF, 0x07FF],
    "led_en": 1,
    "overheat_temp": 95, "overheat_color": 1, "overheat_times": 3, "overheat_cool": 600,
    "hourly_en": 1, "hourly_color": 4, "hourly_times": 1,
    "preset": [[7, 2, 5], [0, 0, 0], [0, 0, 0], [0, 0, 0]],
    # ---- 卡片 ----
    "card": 5,                 # 5 = 万年历 (固件离线绘制, 默认)
    "city": "北京",
    "todo": [{"text": "", "done": False} for _ in range(6)],
    "photo": "",
}


def rgb565_to_hex(v):
    r = ((v >> 11) & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x3F) * 255 // 63
    b = (v & 0x1F) * 255 // 31
    return "#%02x%02x%02x" % (r, g, b)


def hex_to_rgb565(h):
    h = h.lstrip("#")
    r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    return ((r * 31 // 255) << 11) | ((g * 63 // 255) << 5) | (b * 31 // 255)


def load_config():
    cfg = json.loads(json.dumps(DEFAULTS))
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg.update(json.load(f))
    except Exception:
        pass
    return cfg


def save_config(cfg):
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)


def _log(*args):
    print(time.strftime("[%H:%M:%S]"), *args, flush=True)


class BleWorker(threading.Thread):
    """后台线程: 独占 BLE 连接, 同时推送数据与下发配置"""

    def __init__(self, q, addr=None, prepare=None):
        super().__init__(daemon=True)
        self.q = q
        self.addr = addr
        self.prepare = prepare     # 首次连接前执行 (暂停后台推送), 在线程里跑
        self.prepared = False
        self.ota_req = None        # (blob, size, crc, ver) 待发送的升级包
        self.ota_ok = False        # 已提交, 等重连后报成功
        self.cmds = queue.Queue()
        self.result_q = queue.Queue()
        self.stop = threading.Event()

    def send(self, data):
        self.cmds.put(bytes(data))

    def read_cfg(self):
        self.cmds.put("READ")

    def stop_conn(self):
        self.stop.set()

    def start_ota(self, blob, size, crc, ver):
        self.ota_req = (blob, size, crc, ver)

    def run(self):
        import asyncio
        try:
            asyncio.run(self._main())
        except Exception as e:
            self.q.put(("msg", "线程退出: %s" % e, self))

    async def _main(self):
        import asyncio
        while not self.stop.is_set():
            if self.prepare is not None and not self.prepared:
                self.prepared = True
                self.q.put(("state", "prep", None, self))
                try:
                    await asyncio.to_thread(self.prepare, self.q, self)
                except Exception as e:
                    self.q.put(("msg", "接管失败: %s" % e, self))
                if self.stop.is_set():
                    break
            self.q.put(("state", "scan" if not self.addr else "connect", None, self))
            addr = self.addr or await agent.find_device(None)
            if self.stop.is_set():
                break                       # 扫描期间被要求停止: 不要再抢连接
            if not addr:
                self.q.put(("state", "idle", None, self))
                await asyncio.sleep(2)
                continue
            self.addr = addr
            _log("[gui] connecting", addr)
            try:
                async with BleakClient(addr, timeout=20.0) as client:
                    _log("[gui] connected", addr)
                    self.q.put(("state", "connected", addr, self))
                    if self.ota_ok:          # 升级后重连成功
                        self.ota_ok = False
                        self.q.put(("ota_done", "升级完成，设备已重启", self))
                    await self._session(client)
            except Exception as e:
                _log("[gui] connect error:", repr(e))
                self.q.put(("msg", "连接失败/断开: %s" % e, self))
            finally:
                self.q.put(("state", "idle", None, self))
            if not self.stop.is_set():
                await asyncio.sleep(2)

    async def _send_ota(self, client, blob, size, crc, ver):
        """把升级包推给副屏 (协议见 docs/09-ota.md)"""
        import asyncio
        pages = len(blob) // ota.PAGE
        _log("[gui] ota start, %d pages" % pages)
        try:
            await client.write_gatt_char(agent.CHAR_UUID,
                                         ota.pkt_start(size, ver, crc), response=True)
            for i in range(pages):
                page = blob[i * ota.PAGE:(i + 1) * ota.PAGE]
                await client.write_gatt_char(agent.CHAR_UUID,
                                             ota.pkt_lo(page[:ota.CHUNK]), response=True)
                await client.write_gatt_char(agent.CHAR_UUID,
                                             ota.pkt_hi(page[ota.CHUNK:]), response=True)
                if i % 4 == 0 or i == pages - 1:
                    self.q.put(("ota", (i + 1) * 100 // pages,
                                "正在发送 %d/%d 页" % (i + 1, pages), self))
            self.q.put(("ota", 100, "校验并提交…", self))
            try:
                await asyncio.wait_for(
                    client.write_gatt_char(agent.CHAR_UUID, ota.pkt_end(), response=True), 10)
            except Exception:
                pass                    # 校验通过会立刻复位, 断连是预期的
            self.q.put(("ota", 100, "设备重启中，等待重新连接…", self))
            for _ in range(8):          # 等设备掉线
                if not client.is_connected:
                    break
                await asyncio.sleep(0.25)
            # 主动断开: 副屏复位后 Windows 侧链路要很久才超时, 不主动断会一直卡在会话里
            try:
                await asyncio.wait_for(client.disconnect(), timeout=5)
            except Exception as e:
                _log("[gui] ota disconnect:", repr(e))
            self.ota_ok = True
        except Exception as e:
            _log("[gui] ota error:", repr(e))
            self.q.put(("ota", -1, "升级失败: %s" % e, self))

    async def _session(self, client):
        import asyncio
        prev_io = psutil.net_io_counters()
        prev_t = time.time()
        psutil.cpu_percent(interval=None)
        agent.start_cpu_temp()
        label_prev = None
        pause = 0.0
        _log("[gui] session start")

        while client.is_connected and not self.stop.is_set():
            if self.ota_req is not None:
                blob, size, crc, ver = self.ota_req
                self.ota_req = None
                await self._send_ota(client, blob, size, crc, ver)
                continue
            while not self.cmds.empty():
                item = self.cmds.get()
                if item == "READ":
                    try:
                        pause = time.time() + 0.6
                        await client.write_gatt_char(agent.CHAR_UUID,
                                                     bytes([0xC7]), response=True)
                        await asyncio.sleep(0.05)
                        raw = await client.read_gatt_char(agent.CHAR_UUID)
                        self.result_q.put(bytes(raw))
                    except Exception as e:
                        self.q.put(("msg", "读取配置失败: %s" % e, self))
                else:
                    try:
                        await client.write_gatt_char(agent.CHAR_UUID, item, response=True)
                    except Exception as e:
                        self.q.put(("msg", "配置写入失败: %s" % e, self))

            if time.time() >= pause:
                p, prev_io, prev_t, cpu, ram, dl, ul, disk = agent.build_packet(prev_io, prev_t)
                ext = agent.build_ext_packet()
                label = agent._cached_label()
                try:
                    await client.write_gatt_char(agent.CHAR_UUID, p, response=True)
                    await client.write_gatt_char(agent.CHAR_UUID, ext, response=True)
                    if label != label_prev:
                        await agent.send_net_bitmap(client, label)
                        label_prev = label
                    self.q.put(("beat", time.time(), self))
                except Exception as e:
                    _log("[gui] push error:", repr(e))
                    self.q.put(("msg", "推送失败: %s" % e, self))
                    break

            await asyncio.sleep(agent.INTERVAL)

        _log("[gui] session end, connected=", client.is_connected,
             "stop=", self.stop.is_set())
        # 主动断开, 让副屏尽快回到广播状态 (它同一时刻只允许一个连接)
        try:
            if client.is_connected:
                await client.disconnect()
        except Exception as e:
            _log("[gui] disconnect error:", repr(e))


class App:
    def __init__(self, root, addr=None):
        self.root = root
        self.cfg = load_config()
        self.q = queue.Queue()
        self.worker = None
        self.last_beat = 0.0
        self.state = "idle"
        self.addr = addr
        self.field_vars = {}
        self.pal_buttons = []
        self.preset_rows = []
        self.took_over = False          # 是否已暂停后台任务并接管连接
        self.restored = False
        self.ota_active = False         # 升级中: 不要触发自动接管
        self.started = time.time()
        self.auto_takeover_done = False
        self.last_task_check = 0.0

        # 上次异常退出 (被强杀/崩溃) 会留下标记: 后台推送可能仍处于禁用状态, 先恢复
        if os.path.exists(RUN_FLAG):
            _log("[gui] found stale flag, restoring agent task")
            task_start()
        try:
            os.makedirs(os.path.dirname(RUN_FLAG), exist_ok=True)
            with open(RUN_FLAG, "w") as f:
                f.write(str(os.getpid()))
        except OSError:
            pass

        self._style()
        self._build_header()
        self._build_body()
        self._build_footer()
        self._start_worker(addr)
        self._tick()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        atexit.register(self._restore_agent)

    # ---------------- 样式 ----------------
    def _style(self):
        self.root.configure(bg=BG)
        s = ttk.Style()
        try:
            s.theme_use("clam")
        except Exception:
            pass
        s.configure("TFrame", background=BG)
        s.configure("Card.TFrame", background=CARD, relief="flat")
        s.configure("TLabel", background=BG, foreground=INK, font=FONT)
        s.configure("Card.TLabel", background=CARD, foreground=INK, font=FONT)
        s.configure("Muted.TLabel", background=CARD, foreground=MUTED, font=FONT_S)
        s.configure("TCheckbutton", background=CARD, foreground=INK, font=FONT)
        s.configure("TRadiobutton", background=CARD, foreground=INK, font=FONT)
        s.configure("TNotebook", background=BG, borderwidth=0, tabmargins=(8, 8, 8, 0))
        s.configure("TNotebook.Tab", padding=(20, 9), font=FONT, borderwidth=0)
        s.map("TNotebook.Tab",
              background=[("selected", CARD), ("!selected", "#e6eaf2")],
              foreground=[("selected", ACCENT), ("!selected", MUTED)])
        s.configure("TButton", font=FONT, padding=(12, 6))
        s.configure("Accent.TButton", font=FONT_B, padding=(16, 9),
                    background=ACCENT, foreground="white", borderwidth=0)
        s.map("Accent.TButton", background=[("active", ACCENT_D)])
        s.configure("Ghost.TButton", font=FONT, padding=(12, 8),
                    background="#e6eaf2", foreground=INK, borderwidth=0)
        s.map("Ghost.TButton", background=[("active", "#d7ddea")])
        s.configure("TEntry", fieldbackground="white")
        s.configure("TSpinbox", fieldbackground="white", arrowsize=12)
        s.configure("TCombobox", fieldbackground="white")
        s.configure("TSeparator", background="#e5e7eb")

    # ---------------- 顶部状态栏 ----------------
    def _build_header(self):
        bar = tk.Frame(self.root, bg=HEADER_BG, height=76)
        bar.pack(fill="x")
        bar.pack_propagate(False)

        left = tk.Frame(bar, bg=HEADER_BG)
        left.pack(side="left", padx=18, pady=12)
        tk.Label(left, text="副屏配置工具", bg=HEADER_BG, fg=HEADER_FG,
                 font=FONT_T).pack(anchor="w")
        tk.Label(left, text="DLG-CLOCK · 显示内容 / 配色 / RGB LED",
                 bg=HEADER_BG, fg="#9ca3af", font=FONT_S).pack(anchor="w")

        right = tk.Frame(bar, bg=HEADER_BG)
        right.pack(side="right", padx=18, pady=12)

        self.pill = tk.Label(right, text="●  未连接", bg=BAD, fg="white",
                             font=FONT_B, padx=12, pady=4)
        self.pill.pack(side="right", padx=(10, 0))

        info = tk.Frame(right, bg=HEADER_BG)
        info.pack(side="right")
        self.dev_label = tk.Label(info, text="正在查找设备…", bg=HEADER_BG,
                                  fg="#e5e7eb", font=FONT, anchor="e")
        self.dev_label.pack(anchor="e")
        self.beat_label = tk.Label(info, text="—", bg=HEADER_BG,
                                   fg="#9ca3af", font=FONT_S, anchor="e")
        self.beat_label.pack(anchor="e")

        self.btn_reconnect = tk.Button(right, text="重新连接", font=FONT_S,
                                       bg="#374151", fg="white", relief="flat",
                                       activebackground="#4b5563",
                                       activeforeground="white", padx=10, pady=3,
                                       cursor="hand2", command=self._reconnect)
        self.btn_reconnect.pack(side="right", padx=8)

        self.btn_takeover = tk.Button(right, text="接管连接", font=FONT_S,
                                      bg="#2563eb", fg="white", relief="flat",
                                      activebackground="#1d4ed8",
                                      activeforeground="white", padx=10, pady=3,
                                      cursor="hand2", command=self._takeover)
        self.btn_takeover.pack(side="right", padx=4)

    # ---------------- 主体 ----------------
    def _build_body(self):
        wrap = tk.Frame(self.root, bg=BG)
        wrap.pack(fill="both", expand=True, padx=14, pady=10)

        nb = ttk.Notebook(wrap)
        nb.pack(fill="both", expand=True)

        self._tab_fields(nb)
        self._tab_colors(nb)
        self._tab_led(nb)
        self._tab_rules(nb)
        self._tab_cards(nb)
        self._tab_firmware(nb)

    def _card(self, parent):
        f = tk.Frame(parent, bg=CARD, highlightthickness=1,
                     highlightbackground="#e5e7eb")
        f.pack(fill="both", expand=True, padx=12, pady=12)
        return f

    def _tab_fields(self, nb):
        page = tk.Frame(nb, bg=BG)
        nb.add(page, text="显示内容")
        card = self._card(page)

        tk.Label(card, text="勾选要在副屏上显示的项目（取消后该区域会清空）",
                 bg=CARD, fg=MUTED, font=FONT_S).grid(row=0, column=0, columnspan=2,
                                                      sticky="w", padx=14, pady=(12, 4))
        mask = self.cfg["field_mask"]
        for i, (name, bit) in enumerate(FIELDS):
            var = tk.BooleanVar(value=bool(mask & bit))
            self.field_vars[bit] = var
            cb = tk.Checkbutton(card, text=name, variable=var, bg=CARD, fg=INK,
                                font=FONT, activebackground=CARD, selectcolor="white",
                                anchor="w", command=self._on_fields, cursor="hand2")
            cb.grid(row=1 + i // 2, column=i % 2, sticky="w", padx=16, pady=9)

    def _tab_colors(self, nb):
        page = tk.Frame(nb, bg=BG)
        nb.add(page, text="屏幕配色")
        card = self._card(page)

        tk.Label(card, text="点击色块选择颜色，点「应用到副屏」后立即生效",
                 bg=CARD, fg=MUTED, font=FONT_S).grid(row=0, column=0, columnspan=3,
                                                      sticky="w", padx=14, pady=(12, 6))
        for i, (name, idx) in enumerate(PALETTES):
            tk.Label(card, text=name, bg=CARD, fg=INK, font=FONT, width=10,
                     anchor="w").grid(row=1 + i, column=0, sticky="w", padx=16, pady=6)
            v = self.cfg["pal"][idx]
            btn = tk.Button(card, width=12, height=1, bg=rgb565_to_hex(v), relief="flat",
                            highlightthickness=1, highlightbackground="#d1d5db",
                            cursor="hand2", command=lambda x=idx: self._pick_color(x))
            btn.grid(row=1 + i, column=1, sticky="w", pady=6)
            tk.Label(card, text=rgb565_to_hex(v), bg=CARD, fg=MUTED,
                     font=FONT_S).grid(row=1 + i, column=2, sticky="w", padx=10)
            self.pal_buttons.append(btn)

    def _tab_led(self, nb):
        page = tk.Frame(nb, bg=BG)
        nb.add(page, text="LED 控制")
        card = self._card(page)

        top = tk.Frame(card, bg=CARD)
        top.pack(fill="x", padx=14, pady=(12, 6))
        self.led_en = tk.BooleanVar(value=bool(self.cfg["led_en"]))
        tk.Checkbutton(top, text="启用 RGB LED", variable=self.led_en, bg=CARD, fg=INK,
                       font=FONT_B, activebackground=CARD, selectcolor="white",
                       command=self._on_led_en, cursor="hand2").pack(side="left")

        row = tk.Frame(card, bg=CARD)
        row.pack(fill="x", padx=14, pady=8)
        tk.Label(row, text="点击测试颜色：", bg=CARD, fg=MUTED,
                 font=FONT_S).pack(side="left", padx=(0, 8))
        for name, bits in LED_COLORS:
            tk.Button(row, text=name, width=4, font=FONT, bg="#eef2ff", fg=INK,
                      relief="flat", activebackground="#dbeafe", cursor="hand2",
                      command=lambda b=bits: self._play(b)).pack(side="left", padx=3)

        row2 = tk.Frame(card, bg=CARD)
        row2.pack(fill="x", padx=14, pady=8)
        tk.Label(row2, text="次数", bg=CARD, fg=INK, font=FONT).pack(side="left")
        self.led_times = tk.IntVar(value=1)
        ttk.Spinbox(row2, from_=1, to=20, width=4,
                    textvariable=self.led_times).pack(side="left", padx=6)
        tk.Label(row2, text="周期(100ms)", bg=CARD, fg=INK,
                 font=FONT).pack(side="left", padx=(16, 4))
        self.led_period = tk.IntVar(value=10)
        ttk.Spinbox(row2, from_=1, to=50, width=4,
                    textvariable=self.led_period).pack(side="left", padx=4)
        ttk.Button(row2, text="测试白光", style="Ghost.TButton",
                   command=lambda: self._play(None)).pack(side="left", padx=14)

        ttk.Separator(card).pack(fill="x", padx=14, pady=12)
        tk.Label(card, text="自定义灯效预设（颜色 / 次数 / 周期）", bg=CARD, fg=MUTED,
                 font=FONT_S).pack(anchor="w", padx=14)

        for s in range(4):
            r = tk.Frame(card, bg=CARD)
            r.pack(fill="x", padx=14, pady=5)
            tk.Label(r, text="预设 %d" % (s + 1), bg=CARD, fg=INK, font=FONT,
                     width=7, anchor="w").pack(side="left")
            col = tk.StringVar(value=str(self.cfg["preset"][s][0]))
            times = tk.IntVar(value=max(1, self.cfg["preset"][s][1]))
            per = tk.IntVar(value=max(1, self.cfg["preset"][s][2]))
            ttk.Combobox(r, width=4, state="readonly", values=[c[1] for c in LED_COLORS],
                         textvariable=col).pack(side="left", padx=4)
            ttk.Spinbox(r, from_=1, to=20, width=4,
                        textvariable=times).pack(side="left", padx=3)
            ttk.Spinbox(r, from_=1, to=50, width=4,
                        textvariable=per).pack(side="left", padx=3)
            ttk.Button(r, text="保存", style="Ghost.TButton",
                       command=lambda i=s: self._preset_save(i)).pack(side="left", padx=(12, 4))
            ttk.Button(r, text="播放", style="Ghost.TButton",
                       command=lambda i=s: self._preset_play(i)).pack(side="left")
            self.preset_rows.append((col, times, per))

    def _tab_rules(self, nb):
        page = tk.Frame(nb, bg=BG)
        nb.add(page, text="LED 规则")
        card = self._card(page)

        g1 = tk.LabelFrame(card, text=" CPU 过热 ", bg=CARD, fg=INK, font=FONT_B,
                           bd=1, relief="groove", padx=12, pady=10)
        g1.pack(fill="x", padx=14, pady=(14, 8))
        tk.Label(g1, text="阈值 ℃", bg=CARD, fg=INK, font=FONT).grid(row=0, column=0, padx=4, pady=6)
        self.oh_temp = tk.IntVar(value=self.cfg["overheat_temp"])
        ttk.Spinbox(g1, from_=40, to=110, width=5, textvariable=self.oh_temp).grid(row=0, column=1)
        tk.Label(g1, text="颜色", bg=CARD, fg=INK, font=FONT).grid(row=0, column=2, padx=(18, 4))
        self.oh_color = tk.StringVar(value=str(self.cfg["overheat_color"]))
        ttk.Combobox(g1, width=4, state="readonly", textvariable=self.oh_color,
                     values=[c[1] for c in LED_COLORS]).grid(row=0, column=3)
        tk.Label(g1, text="次数", bg=CARD, fg=INK, font=FONT).grid(row=0, column=4, padx=(18, 4))
        self.oh_times = tk.IntVar(value=self.cfg["overheat_times"])
        ttk.Spinbox(g1, from_=0, to=20, width=4, textvariable=self.oh_times).grid(row=0, column=5)
        tk.Label(g1, text="冷却秒", bg=CARD, fg=INK, font=FONT).grid(row=1, column=0, padx=4, pady=6)
        self.oh_cool = tk.IntVar(value=self.cfg["overheat_cool"])
        ttk.Spinbox(g1, from_=10, to=3600, width=7, textvariable=self.oh_cool).grid(row=1, column=1)

        g2 = tk.LabelFrame(card, text=" 整点报时 ", bg=CARD, fg=INK, font=FONT_B,
                           bd=1, relief="groove", padx=12, pady=10)
        g2.pack(fill="x", padx=14, pady=8)
        self.hr_en = tk.BooleanVar(value=bool(self.cfg["hourly_en"]))
        tk.Checkbutton(g2, text="启用", variable=self.hr_en, bg=CARD, fg=INK, font=FONT,
                       activebackground=CARD, selectcolor="white").grid(row=0, column=0, padx=4, pady=6)
        tk.Label(g2, text="颜色", bg=CARD, fg=INK, font=FONT).grid(row=0, column=1, padx=(10, 4))
        self.hr_color = tk.StringVar(value=str(self.cfg["hourly_color"]))
        ttk.Combobox(g2, width=4, state="readonly", textvariable=self.hr_color,
                     values=[c[1] for c in LED_COLORS]).grid(row=0, column=2)
        tk.Label(g2, text="次数", bg=CARD, fg=INK, font=FONT).grid(row=0, column=3, padx=(18, 4))
        self.hr_times = tk.IntVar(value=self.cfg["hourly_times"])
        ttk.Spinbox(g2, from_=0, to=20, width=4, textvariable=self.hr_times).grid(row=0, column=4)

        tk.Label(card, text="提示：RGB 灯为三路开关，颜色只能取 7 种组合，无亮度调节。",
                 bg=CARD, fg=MUTED, font=FONT_S).pack(anchor="w", padx=16, pady=(6, 12))

    def _tab_cards(self, nb):
        page = tk.Frame(nb, bg=BG)
        nb.add(page, text="显示卡片")
        box = self._card(page)

        # ---- 卡片选择 ----
        row = tk.Frame(box, bg=CARD)
        row.pack(fill="x", padx=14, pady=(14, 4))
        tk.Label(row, text="屏幕显示", bg=CARD, fg=INK, font=FONT_B).pack(side="left", padx=(0, 12))
        self.card_var = tk.IntVar(value=int(self.cfg.get("card", 5)))
        for i, name in enumerate(["监控面板", "纯时钟", "天气", "TODO", "画册", "万年历"]):
            tk.Radiobutton(row, text=name, variable=self.card_var, value=i,
                           command=self._on_card, bg=CARD, fg=INK, font=FONT,
                           selectcolor="white", activebackground=CARD,
                           cursor="hand2").pack(side="left", padx=(0, 10))

        tk.Label(box, text="万年历/监控面板/纯时钟由固件绘制；天气/TODO/画册由电脑渲染成黑白图后推送",
                 bg=CARD, fg=MUTED, font=FONT_S).pack(anchor="w", padx=16, pady=(0, 8))

        # ---- 天气 ----
        g1 = tk.LabelFrame(box, text=" 天气 ", bg=CARD, fg=INK, font=FONT_B,
                           bd=1, relief="groove", padx=12, pady=8)
        g1.pack(fill="x", padx=14, pady=6)
        tk.Label(g1, text="城市", bg=CARD, fg=INK, font=FONT).pack(side="left")
        self.city_var = tk.StringVar(value=self.cfg.get("city", "北京"))
        tk.Entry(g1, textvariable=self.city_var, font=FONT, width=12,
                 relief="solid", bd=1).pack(side="left", padx=8, ipady=3)
        tk.Label(g1, text="数据来自 wttr.in，约 20 分钟更新一次", bg=CARD, fg=MUTED,
                 font=FONT_S).pack(side="left", padx=6)

        # ---- TODO ----
        g2 = tk.LabelFrame(box, text=" TODO（最多 6 条，每条 16 字内最清楚） ", bg=CARD, fg=INK,
                           font=FONT_B, bd=1, relief="groove", padx=12, pady=8)
        g2.pack(fill="x", padx=14, pady=6)
        todo = self.cfg.get("todo", [])
        self.todo_rows = []
        for i in range(6):
            item = todo[i] if i < len(todo) else {"text": "", "done": False}
            done = tk.BooleanVar(value=bool(item.get("done")))
            txt = tk.StringVar(value=item.get("text", ""))
            r = tk.Frame(g2, bg=CARD)
            r.pack(fill="x", pady=2)
            tk.Checkbutton(r, text="完成", variable=done, bg=CARD, fg=INK, font=FONT_S,
                           selectcolor="white", activebackground=CARD).pack(side="left")
            tk.Entry(r, textvariable=txt, font=FONT, relief="solid", bd=1).pack(
                side="left", fill="x", expand=True, padx=6, ipady=2)
            self.todo_rows.append((done, txt))

        # ---- 画册 ----
        g3 = tk.LabelFrame(box, text=" 画册（自动裁成 320x240 黑白抖动图） ", bg=CARD, fg=INK,
                           font=FONT_B, bd=1, relief="groove", padx=12, pady=8)
        g3.pack(fill="x", padx=14, pady=6)
        r = tk.Frame(g3, bg=CARD)
        r.pack(fill="x")
        self.photo_var = tk.StringVar(value=self.cfg.get("photo", ""))
        tk.Entry(r, textvariable=self.photo_var, font=FONT, relief="solid",
                 bd=1).pack(side="left", fill="x", expand=True, ipady=3)
        ttk.Button(r, text="浏览…", style="Ghost.TButton",
                   command=self._pick_photo).pack(side="left", padx=(8, 0))

        # ---- 底部 ----
        bottom = tk.Frame(box, bg=CARD)
        bottom.pack(fill="x", padx=14, pady=(10, 14))
        ttk.Button(bottom, text="应用卡片", style="Accent.TButton",
                   command=self._apply_card).pack(side="left")
        self.card_hint = tk.Label(bottom, text="", bg=CARD, fg=MUTED, font=FONT_S)
        self.card_hint.pack(side="left", padx=12)

    def _pick_photo(self):
        path = filedialog.askopenfilename(
            title="选择图片", filetypes=[("图片", "*.jpg *.jpeg *.png *.bmp *.webp"),
                                        ("所有文件", "*.*")])
        if path:
            self.photo_var.set(path)

    def _collect_card(self):
        self.cfg["card"] = int(self.card_var.get())
        self.cfg["city"] = self.city_var.get().strip() or "北京"
        self.cfg["todo"] = [{"text": t.get().strip(), "done": bool(d.get())}
                            for d, t in self.todo_rows]
        self.cfg["photo"] = self.photo_var.get().strip()
        save_config(self.cfg)

    def _on_card(self):
        self._collect_card()
        self._apply_card()

    def _apply_card(self):
        self._collect_card()
        card = int(self.cfg["card"])
        if self.state != "connected":
            self.card_hint.configure(text="已保存，连接副屏后自动下发", fg=MUTED)
            return
        if card == 2:
            self.card_hint.configure(text="正在获取天气并下发…", fg=MUTED)
        elif card == 4 and not os.path.exists(self.cfg.get("photo", "")):
            self.card_hint.configure(text="请先选择画册图片", fg=BAD)
            return
        else:
            self.card_hint.configure(text="正在下发…", fg=MUTED)
        self._push_card_async()

    def _push_card_async(self):
        """渲染 + 入队放到后台线程, 避免抓天气时卡住界面"""
        cfg = dict(self.cfg)

        def work():
            try:
                pkts = cards.build_card_packets(cfg)
            except Exception as e:
                self.q.put(("card", "下发失败: %s" % e, self.worker))
                return
            for p in pkts:
                self.worker.send(p)
            self.q.put(("card", "卡片已下发", self.worker))

        threading.Thread(target=work, daemon=True).start()

    def _tab_firmware(self, nb):
        page = tk.Frame(nb, bg=BG)
        nb.add(page, text="固件升级")
        card = self._card(page)

        tk.Label(card, text="选择要写入副屏的固件（.bin），升级约 30 秒，期间请保持供电",
                 bg=CARD, fg=MUTED, font=FONT_S).pack(anchor="w", padx=16, pady=(14, 6))

        row = tk.Frame(card, bg=CARD)
        row.pack(fill="x", padx=16)
        default = os.path.join(ROOT, "firmware", "release", "pcmonitor_middle.bin")
        if not os.path.exists(default):
            default = os.path.join(ROOT, "firmware", "pcmonitor", "build", "pcmonitor.bin")
        self.fw_var = tk.StringVar(value=default)
        tk.Entry(row, textvariable=self.fw_var, font=FONT,
                 relief="solid", bd=1).pack(side="left", fill="x", expand=True, ipady=5)
        ttk.Button(row, text="浏览…", style="Ghost.TButton",
                   command=self._pick_fw).pack(side="left", padx=(8, 0))

        self.fw_info = tk.Label(card, text="", bg=CARD, fg=MUTED, font=FONT_S, anchor="w")
        self.fw_info.pack(fill="x", padx=16, pady=(8, 0))

        self.progress = ttk.Progressbar(card, maximum=100, value=0)
        self.progress.pack(fill="x", padx=16, pady=(16, 6))

        self.ota_label = tk.Label(card, text="等待开始", bg=CARD, fg=INK, font=FONT, anchor="w")
        self.ota_label.pack(fill="x", padx=16)

        self.btn_ota = ttk.Button(card, text="开始升级", style="Accent.TButton",
                                  command=self._start_ota)
        self.btn_ota.pack(anchor="w", padx=16, pady=(14, 4))

        tk.Label(card, text="说明：升级只写副屏的另一个分区，中途失败/断电不会变砖；"
                            "完成后设备自动重启，本工具会自动重连。",
                 bg=CARD, fg=MUTED, font=FONT_S, justify="left", wraplength=640
                 ).pack(anchor="w", padx=16, pady=(6, 14))

        self._fw_info()

    def _pick_fw(self):
        path = filedialog.askopenfilename(
            title="选择固件", filetypes=[("固件", "*.bin"), ("所有文件", "*.*")])
        if path:
            self.fw_var.set(path)
            self._fw_info()

    def _fw_info(self):
        path = self.fw_var.get().strip()
        if not os.path.isfile(path):
            self.fw_info.configure(text="找不到文件: %s" % path, fg=BAD)
            return
        try:
            blob, size, crc, ver = ota.build_packet(path)
        except Exception as e:
            self.fw_info.configure(text="读取失败: %s" % e, fg=BAD)
            return
        self.fw_info.configure(
            text="%d 字节 · %d 页 · version 0x%08X · crc 0x%08X"
                 % (size, len(blob) // ota.PAGE, ver, crc), fg=MUTED)

    def _start_ota(self):
        if self.state != "connected":
            self.ota_label.configure(text="请先连接副屏（连不上就点顶部「接管连接」）", fg=BAD)
            return
        path = self.fw_var.get().strip()
        if not os.path.isfile(path):
            self.ota_label.configure(text="找不到固件文件", fg=BAD)
            return
        try:
            blob, size, crc, ver = ota.build_packet(path)
        except Exception as e:
            self.ota_label.configure(text="读取固件失败: %s" % e, fg=BAD)
            return
        if not messagebox.askokcancel(
                "确认升级",
                "固件 %d 字节，version 0x%08X\n\n"
                "升级期间不要关闭本工具，也不要断开副屏电源。\n"
                "完成后副屏会自动重启并重新连接。" % (size, ver)):
            return
        self.btn_ota.configure(state="disabled")
        self.progress.configure(value=0)
        self.ota_label.configure(text="正在发送…", fg=INK)
        self.ota_active = True
        self.worker.start_ota(blob, size, crc, ver)

    # ---------------- 底部按钮 ----------------
    def _build_footer(self):
        footer = tk.Frame(self.root, bg=BG)
        footer.pack(fill="x", padx=14, pady=(0, 10))

        ttk.Button(footer, text="应用到副屏", style="Accent.TButton",
                   command=self.apply).pack(side="left")
        ttk.Button(footer, text="保存到 Flash", style="Ghost.TButton",
                   command=self.save_flash).pack(side="left", padx=8)
        ttk.Button(footer, text="读取配置", style="Ghost.TButton",
                   command=self.read_cfg).pack(side="left")
        ttk.Button(footer, text="恢复默认", style="Ghost.TButton",
                   command=self.reset).pack(side="left", padx=8)

        self.hint = tk.Label(footer, text="", bg=BG, fg=MUTED, font=FONT_S, anchor="e")
        self.hint.pack(side="right")

    # ---------------- 连接状态 ----------------
    def _start_worker(self, addr, prepare=None):
        if self.worker:
            self.worker.stop_conn()
            self.worker.join(timeout=0.6)   # 旧线程会在带 stop 检查处自行退出
        self.worker = BleWorker(self.q, addr, prepare=prepare)
        self.worker.start()

    def _reconnect(self):
        self.addr = None
        self.last_beat = 0.0
        self._set_state("scan")
        self._start_worker(None)
        self.hint.configure(text="正在重新扫描设备…")

    def _takeover(self):
        """暂停后台推送任务, 让本工具独占 BLE 连接 (耗时操作放在 worker 线程里)"""
        if self.took_over:
            self.hint.configure(text="已在接管模式：后台推送已暂停")
            return
        self.hint.configure(text="正在暂停后台推送并接管连接…")
        self.addr = None
        self.last_beat = 0.0
        self._set_state("prep")
        self._start_worker(None, prepare=self._prepare_takeover)

    def _prepare_takeover(self, q, worker):
        """在 worker 线程内执行: 结束后台推送并确认它真的退出了"""
        task_stop()
        deadline = time.time() + 6.0
        while task_running() and time.time() < deadline:
            time.sleep(0.2)
        left = task_running()
        _log("[gui] takeover, agent_running=", left)
        q.put(("took", not left, worker))
        if left:
            q.put(("msg", "后台推送未能停止，请手动结束 PcMonitorAgent 任务", worker))
        else:
            q.put(("msg", "后台推送已暂停，正在连接副屏…", worker))
        time.sleep(1.5)                 # 等副屏断开上一路连接并重新广播

    def _on_close(self):
        if self.worker:
            self.worker.stop_conn()
        self._close_deadline = time.time() + 3.0
        self._wait_close()

    def _wait_close(self):
        """等 BLE 真正断开 -> 恢复后台推送 -> 退出 (不做 Tk/WinRT 析构, 避免关闭时崩溃)"""
        alive = self.worker and self.worker.is_alive()
        if alive and time.time() < self._close_deadline:
            self.root.after(150, self._wait_close)
            return
        self._restore_agent()
        try:
            self.root.destroy()
        except Exception:
            pass
        _log("[gui] bye")
        os._exit(0)

    def _restore_agent(self):
        """退出时恢复后台推送任务 (atexit 兜底; 无论是否接管过都保证任务可用)"""
        if self.restored:
            return
        self.restored = True
        try:
            os.remove(RUN_FLAG)
        except OSError:
            pass
        task_start()

    def _set_state(self, state, addr=None):
        self.state = state
        if state == "connected":
            self.pill.configure(text="●  已连接", bg=OK)
            self.dev_label.configure(text=addr or "")
            self.beat_label.configure(text="等待数据…")
            self.btn_reconnect.configure(text="重新连接")
            # 连上后自动同步一次卡片 (天气/TODO/画册的图像需要重推)
            if hasattr(self, "card_var") and not self.ota_active:
                self.root.after(400, self._apply_card)
        elif state == "prep":
            self.pill.configure(text="●  接管中", bg=WARN)
            self.dev_label.configure(text="正在暂停后台推送…")
            self.beat_label.configure(text="—")
        elif state == "connect":
            self.pill.configure(text="●  连接中", bg=WARN)
            self.dev_label.configure(text="正在连接设备…")
            self.beat_label.configure(text="—")
        elif state == "scan":
            self.pill.configure(text="●  搜索中", bg=WARN)
            self.dev_label.configure(text="正在扫描 DLG-CLOCK-*…")
            self.beat_label.configure(text="—")
        else:
            self.pill.configure(text="●  未连接", bg=BAD)
            self.dev_label.configure(text="没有连接到副屏")
            self.beat_label.configure(text="副屏若已有数据，说明被后台推送占用")
            if not self.took_over:
                self.hint.configure(text="点顶部「接管连接」暂停后台推送，即可在本工具里配置")

    def _tick(self):
        while True:
            try:
                item = self.q.get_nowait()
            except queue.Empty:
                break
            kind = item[0]
            if item[-1] is not self.worker:
                continue                    # 旧线程的残留消息直接丢掉
            if kind == "state":
                self._set_state(item[1], item[2] if len(item) > 2 else None)
            elif kind == "beat":
                self.last_beat = item[1]
            elif kind == "msg":
                self.hint.configure(text=item[1])
            elif kind == "took":
                self.took_over = bool(item[1])
            elif kind == "ota":
                pct, msg = item[1], item[2]
                if pct < 0:
                    self.ota_active = False
                    self.btn_ota.configure(state="normal")
                    self.progress.configure(value=0)
                else:
                    self.progress.configure(value=max(0, pct))
                if msg:
                    self.ota_label.configure(text=msg, fg=BAD if pct < 0 else INK)
            elif kind == "ota_done":
                self.ota_active = False
                self.btn_ota.configure(state="normal")
                self.progress.configure(value=100)
                self.ota_label.configure(text=item[1], fg=OK)
            elif kind == "card":
                if hasattr(self, "card_hint"):
                    self.card_hint.configure(text=item[1], fg=OK)

        if self.state == "connected":
            if self.last_beat:
                dt = time.time() - self.last_beat
                self.beat_label.configure(
                    text="数据实时 · %.1f 秒前" % dt if dt < 60 else "数据中断",
                    fg=OK if dt < 3 else WARN)
            else:
                self.beat_label.configure(text="等待数据…", fg="#9ca3af")

        try:
            raw = self.worker.result_q.get_nowait()
            self._apply_readback(raw)
        except queue.Empty:
            pass

        # 启动 6 秒仍连不上, 且后台推送在跑 -> 自动接管一次 (每 2 秒检查)
        if (not self.auto_takeover_done and not self.took_over and not self.ota_active
                and self.state != "connected"
                and time.time() - self.started > 6.0
                and time.time() - self.last_task_check > 2.0):
            self.last_task_check = time.time()
            if task_running():
                self.auto_takeover_done = True
                self._takeover()

        # 接管模式下若又检测到后台推送, 重新压下去 (防止任务被自动重启)
        if (self.took_over and self.state != "connected"
                and time.time() - self.last_task_check > 5.0):
            self.last_task_check = time.time()
            if task_running():
                task_stop()
                self.hint.configure(text="后台推送又启动了，已再次暂停并重连…")

        self.root.after(200, self._tick)

    # ---------------- 交互 ----------------
    def _on_fields(self):
        mask = 0
        for bit, var in self.field_vars.items():
            if var.get():
                mask |= bit
        self.cfg["field_mask"] = mask

    def _pick_color(self, idx):
        cur = rgb565_to_hex(self.cfg["pal"][idx])
        rgb, hx = colorchooser.askcolor(color=cur, title="选择颜色")
        if not hx:
            return
        self.cfg["pal"][idx] = hex_to_rgb565(hx)
        self.pal_buttons[idx].configure(bg=hx)

    def _on_led_en(self):
        self.cfg["led_en"] = 1 if self.led_en.get() else 0
        self.worker.send([0xC2, self.cfg["led_en"], 0, 0, 10])

    def _play(self, bits):
        if bits is None:
            bits = 7
        self.worker.send([0xC2, 1 if self.led_en.get() else 0, bits,
                          max(1, int(self.led_times.get())),
                          max(1, int(self.led_period.get()))])

    def _preset_save(self, slot):
        col, times, per = self.preset_rows[slot]
        c, t, p = int(col.get() or 0), max(0, int(times.get())), max(1, int(per.get()))
        self.cfg["preset"][slot] = [c, t, p]
        self.worker.send([0xC5, slot, c, t, p, 0])
        self.hint.configure(text="预设 %d 已下发（记得「保存到 Flash」）" % (slot + 1))

    def _preset_play(self, slot):
        col, times, per = self.preset_rows[slot]
        self.worker.send([0xC5, slot, int(col.get() or 0),
                          max(1, int(times.get())), max(1, int(per.get())), 1])

    def _packets(self):
        c = self.cfg
        out = [bytes([0xC0, c["field_mask"] & 0xFF, (c["field_mask"] >> 8) & 0xFF])]
        for i, v in enumerate(c["pal"]):
            out.append(bytes([0xC1, i, v & 0xFF, (v >> 8) & 0xFF]))
        out.append(bytes([0xC2, 1 if c["led_en"] else 0, 0, 0, 10]))
        out.append(bytes([0xC3, c["overheat_temp"] & 0xFF, c["overheat_color"] & 7,
                          c["overheat_times"] & 0xFF,
                          c["overheat_cool"] & 0xFF, (c["overheat_cool"] >> 8) & 0xFF]))
        out.append(bytes([0xC4, 1 if c["hourly_en"] else 0, c["hourly_color"] & 7,
                          c["hourly_times"] & 0xFF]))
        for s, (col, t, p) in enumerate(c["preset"]):
            out.append(bytes([0xC5, s, col & 7, t & 0xFF, p & 0xFF, 0]))
        out.append(bytes([0xC8, int(c.get("card", 0)) & 0xFF]))
        return out

    def apply(self):
        if self.state != "connected":
            messagebox.showwarning("未连接", "请先连接副屏（查看顶部状态条）")
            return
        self._on_fields()
        self.cfg["led_en"] = 1 if self.led_en.get() else 0
        self.cfg["overheat_temp"] = int(self.oh_temp.get())
        self.cfg["overheat_color"] = int(self.oh_color.get() or 0)
        self.cfg["overheat_times"] = max(0, int(self.oh_times.get()))
        self.cfg["overheat_cool"] = max(10, int(self.oh_cool.get()))
        self.cfg["hourly_en"] = 1 if self.hr_en.get() else 0
        self.cfg["hourly_color"] = int(self.hr_color.get() or 0)
        self.cfg["hourly_times"] = max(0, int(self.hr_times.get()))
        save_config(self.cfg)
        for p in self._packets():
            self.worker.send(p)
        if int(self.cfg.get("card", 0)) >= 2:
            self._push_card_async()          # 图像卡还要把图贴上去
        self.hint.configure(text="配置已下发 · 点「保存到 Flash」可断电保留")

    def save_flash(self):
        self.apply()
        self.worker.send([0xC6, 0])
        self.hint.configure(text="已请求保存到副屏 Flash")

    def read_cfg(self):
        self.worker.read_cfg()
        self.hint.configure(text="正在读取配置…")

    def reset(self):
        self.cfg = json.loads(json.dumps(DEFAULTS))
        save_config(self.cfg)
        for bit, var in self.field_vars.items():
            var.set(bool(self.cfg["field_mask"] & bit))
        for idx, btn in enumerate(self.pal_buttons):
            btn.configure(bg=rgb565_to_hex(self.cfg["pal"][idx]))
        self.card_var.set(0)
        self.city_var.set(self.cfg["city"])
        for i, (done, txt) in enumerate(self.todo_rows):
            done.set(False)
            txt.set("")
        self.photo_var.set("")
        self.worker.send([0xC6, 1])
        self.hint.configure(text="已恢复默认")

    def _apply_readback(self, raw):
        if len(raw) < CFG_SIZE:
            self.hint.configure(text="配置回读长度异常: %d" % len(raw))
            return
        vals = struct.unpack(CFG_FMT, raw[:CFG_SIZE])
        if vals[0] != CFG_MAGIC:
            self.hint.configure(text="回读校验失败（可能被数据包覆盖，请重试）")
            return
        c = {
            "field_mask": vals[2],
            "pal": list(vals[3:12]),
            "led_en": vals[12],
            "overheat_temp": vals[13], "overheat_color": vals[14],
            "overheat_times": vals[15], "overheat_cool": vals[16],
            "hourly_en": vals[17], "hourly_color": vals[18], "hourly_times": vals[19],
            "card": vals[20],
            "preset": [[vals[21 + i * 3], vals[22 + i * 3], vals[23 + i * 3]] for i in range(4)],
        }
        c["city"] = self.cfg.get("city", "北京")
        c["todo"] = self.cfg.get("todo", [])
        c["photo"] = self.cfg.get("photo", "")
        self.cfg = c
        save_config(c)
        for bit, var in self.field_vars.items():
            var.set(bool(c["field_mask"] & bit))
        for idx, btn in enumerate(self.pal_buttons):
            btn.configure(bg=rgb565_to_hex(c["pal"][idx]))
        self.card_var.set(int(c["card"]))
        self.city_var.set(c["city"])
        for i, (done, txt) in enumerate(self.todo_rows):
            item = c["todo"][i] if i < len(c["todo"]) else {"text": "", "done": False}
            done.set(bool(item.get("done")))
            txt.set(item.get("text", ""))
        self.photo_var.set(c["photo"])
        self.hint.configure(text="已读取副屏配置")


_CRASH_FILE = None


def _install_crash_log():
    """把原生崩溃 / 未捕获异常写到日志文件, 便于排查进程静默退出"""
    global _CRASH_FILE
    path = os.path.join(os.path.dirname(CONFIG_PATH), "gui_crash.log")

    def hook(exc_type, exc, tb):
        import traceback
        with open(path, "a", encoding="utf-8") as f:
            f.write(time.strftime("[%Y-%m-%d %H:%M:%S] unhandled\n"))
            traceback.print_exception(exc_type, exc, tb, file=f)

    def thook(args):
        if args.exc_value is not None:
            hook(args.exc_type, args.exc_value, args.exc_traceback)

    sys.excepthook = hook
    threading.excepthook = thook
    try:
        import faulthandler
        _CRASH_FILE = open(path, "a", buffering=1, encoding="utf-8")
        faulthandler.enable(file=_CRASH_FILE, all_threads=True)
    except Exception:
        pass


def main():
    _install_crash_log()
    addr = sys.argv[1] if len(sys.argv) > 1 else None
    root = tk.Tk()
    root.title("副屏配置工具")
    root.geometry("760x720")
    root.minsize(680, 640)
    try:
        root.iconbitmap(default="")
    except Exception:
        pass
    App(root, addr)
    root.mainloop()


if __name__ == "__main__":
    main()
