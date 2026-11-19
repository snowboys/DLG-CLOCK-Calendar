"""cards.py - 卡片图像渲染与推送 (320x240, 1bpp)

设备端只做"贴图", 排版/中文字体全在电脑上完成, 固件不需要字库。
图像协议 (docs/10-cards.md):
    0xCB  w(u16) h(u16)        开始, 清屏
    0xCC  7 字节保留 + 128 字节  顺序追加, 每 1600 字节(40 行)自动上屏
    0xCD                        结束
"""
import json
import os
import time

import asyncio

from PIL import Image, ImageDraw, ImageFont

W, H = 320, 240
PAGE = 1600                     # 设备端一条带 = 40 行 x 40 字节
CARD_MONITOR, CARD_CLOCK, CARD_WEATHER, CARD_TODO, CARD_GALLERY, CARD_CALENDAR = range(6)

CONFIG_PATH = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")),
                           "pcmonitor", "gui_config.json")

FONT_REG = [r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simhei.ttf",
            r"C:\Windows\Fonts\simsun.ttc"]
FONT_BOLD = [r"C:\Windows\Fonts\msyhbd.ttc", r"C:\Windows\Fonts\msyh.ttc",
             r"C:\Windows\Fonts\simhei.ttf"]


def font(size, bold=False):
    for p in (FONT_BOLD if bold else FONT_REG) + FONT_REG:
        try:
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
        except Exception:
            pass
    return ImageFont.load_default()


def blank():
    return Image.new("1", (W, H), 0)


def pack(img):
    """转成设备要的 1bpp 字节流: 每行 40 字节, 高位在左, 1 = 亮"""
    if img.size != (W, H):
        img = img.resize((W, H))
    if img.mode != "1":
        img = img.convert("1")
    return img.tobytes()


# ---------------------------------------------------------------- 天气
def _wx_icon(d, kind, x, y, s=64):
    """用基本图形画天气图标 (1 = 亮); 填充比描边在 1bit 屏上更清楚"""
    import math

    cx, cy = x + s // 2, y + s // 2
    b = s // 3

    def sun(r, ox=0, oy=0):
        d.ellipse([cx - r + ox, cy - r + oy, cx + r + ox, cy + r + oy], fill=1)
        for i in range(8):
            a = i * math.pi / 4
            d.line([cx + ox + math.cos(a) * (r + 4), cy + oy + math.sin(a) * (r + 4),
                    cx + ox + math.cos(a) * (r + 11), cy + oy + math.sin(a) * (r + 11)],
                   fill=1, width=3)

    def cloud(dy=0):
        d.ellipse([cx - b, cy - b * 0.45 + dy, cx + b * 0.15, cy + b * 0.75 + dy], fill=1)
        d.ellipse([cx - b * 0.35, cy - b * 0.95 + dy, cx + b * 0.45, cy + b * 0.05 + dy], fill=1)
        d.ellipse([cx + b * 0.05, cy - b * 0.35 + dy, cx + b, cy + b * 0.75 + dy], fill=1)
        d.rectangle([cx - b * 0.85, cy + b * 0.35 + dy, cx + b * 0.85, cy + b * 0.78 + dy], fill=1)

    if kind == 0:                              # 晴
        sun(s // 4)
    elif kind == 1:                            # 多云
        sun(s // 6, -s // 5, -s // 5)
        cloud(s // 10)
    elif kind == 2:                            # 阴
        cloud()
    elif kind == 3:                            # 雨
        cloud(-s // 6)
        for i in range(4):
            px = cx - b * 0.7 + i * (b * 0.45)
            d.line([px, cy + b * 0.85, px - 4, cy + b * 1.25], fill=1, width=3)
    elif kind == 4:                            # 雪
        cloud(-s // 6)
        for i in range(3):
            px = cx - b * 0.6 + i * (b * 0.6)
            d.ellipse([px - 3, cy + b * 0.9, px + 3, cy + b * 0.9 + 6], fill=1)
    elif kind == 5:                            # 雷
        cloud(-s // 6)
        d.line([cx + 2, cy + b * 0.7, cx - b * 0.35, cy + b * 1.2,
                cx + b * 0.15, cy + b * 1.2, cx - b * 0.25, cy + b * 1.7], fill=1, width=4)
    else:                                      # 雾
        for i in range(4):
            yy = cy - b * 0.5 + i * (b * 0.38)
            d.line([cx - b * 0.9, yy, cx + b * 0.9, yy], fill=1, width=4)


def wmo_kind(code):
    """WWO 天气码 -> 图标类别"""
    try:
        c = int(code)
    except Exception:
        return 2
    if c == 113:
        return 0
    if c in (116, 119, 122):
        return 1
    if c in (143, 248, 260):
        return 6
    if c in (176, 185, 263, 266, 281, 284, 293, 296, 299, 302, 305, 308, 311, 314, 353, 356, 359):
        return 3
    if c in (179, 182, 227, 230, 317, 320, 323, 326, 329, 332, 335, 338, 350, 368, 371, 374, 377):
        return 4
    if c in (200, 386, 389, 392, 395):
        return 5
    return 2


def render_weather(wx):
    """wx: dict(city, desc, temp, hi, lo, hum, kind)"""
    img = blank()
    d = ImageDraw.Draw(img)

    d.text((14, 6), str(wx.get("city", ""))[:12], font=font(24, True), fill=1)

    t = wx.get("temp", "--")
    fbig = font(96, True)
    d.text((12, 34), "%s" % t, font=fbig, fill=1)
    tw = d.textlength("%s" % t, font=fbig)
    d.text((14 + tw + 4, 44), "°C", font=font(28), fill=1)

    _wx_icon(d, wx.get("kind", 2), 240, 40, 68)

    d.text((14, 154), str(wx.get("desc", ""))[:14], font=font(22), fill=1)
    d.line([14, 190, 306, 190], fill=1, width=1)
    d.text((14, 197), "H %s   L %s    RH %s%%" % (wx.get("hi", "--"), wx.get("lo", "--"),
                                                  wx.get("hum", "--")),
           font=font(19), fill=1)
    return img


# ---------------------------------------------------------------- TODO
def render_todo(items, title="TODO"):
    """items: [(text, done), ...] 最多 6 条"""
    img = blank()
    d = ImageDraw.Draw(img)
    items = list(items)[:6]
    done_n = sum(1 for _, dk in items if dk)

    d.text((14, 8), title, font=font(24, True), fill=1)
    d.text((W - 14 - d.textlength("%d/%d" % (done_n, len(items)), font=font(22)),
            10), "%d/%d" % (done_n, len(items)), font=font(22), fill=1)
    d.line([14, 40, W - 14, 40], fill=1, width=1)

    y = 52
    for text, done in items:
        box = [16, y + 4, 30, y + 18]
        if done:
            d.rectangle(box, fill=1)
            d.line([19, y + 11, 22, y + 15], fill=0, width=2)
            d.line([22, y + 15, 28, y + 7], fill=0, width=2)
        else:
            d.rectangle(box, outline=1, width=2)
        d.text((40, y), str(text)[:16], font=font(20), fill=1)
        if done:
            d.line([40, y + 12, 40 + min(len(str(text)), 16) * 20, y + 12], fill=1, width=1)
        y += 29
    return img


# ---------------------------------------------------------------- 画册
def _bayer(n):
    if n == 1:
        return [[0]]
    m = _bayer(n // 2)
    return ([[4 * v for v in r] + [4 * v + 2 for v in r] for r in m] +
            [[4 * v + 3 for v in r] + [4 * v + 1 for v in r] for r in m])


_BAYER8 = _bayer(8)


def _ordered_dither(im):
    """8x8 有序抖动: 小屏上比误差扩散更像印刷网点, 噪点少"""
    im = im.convert("L")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            t = (_BAYER8[y & 7][x & 7] + 0.5) * 255.0 / 64.0
            px[x, y] = 255 if px[x, y] > t else 0
    return im.convert("1", dither=Image.Dither.NONE)


def render_photo(path):
    from PIL import ImageOps
    im = Image.open(path)
    try:
        im = ImageOps.exif_transpose(im)
    except Exception:
        pass
    im = im.convert("L")
    im = ImageOps.autocontrast(im)
    r = max(W / im.width, H / im.height)
    im = im.resize((max(1, int(im.width * r + 0.5)), max(1, int(im.height * r + 0.5))),
                   Image.LANCZOS)
    left = (im.width - W) // 2
    top = (im.height - H) // 2
    im = im.crop((left, top, left + W, top + H))
    return _ordered_dither(im)


# ---------------------------------------------------------------- 推送
async def send_card(write, card):
    await write(bytes([0xC8, card]))


async def send_image(write, img):
    data = pack(img)
    await write(bytes([0xCB, W & 0xFF, W >> 8, H & 0xFF, H >> 8]))
    for i in range(0, len(data), 128):
        chunk = data[i:i + 128]
        await write(bytes([0xCC]) + b"\x00" * 7 + chunk)
    await write(bytes([0xCD]))


def build_card_packets(cfg):
    """按配置生成完整的一串 BLE 包: 切卡片 + (可选)整屏图像"""
    card = int(cfg.get("card", CARD_CALENDAR))
    pkts = [bytes([0xC8, card])]

    img = None
    if card == CARD_WEATHER:
        from weather import get_weather
        img = render_weather(get_weather(cfg.get("city", "Beijing")))
    elif card == CARD_TODO:
        items = [(i.get("text", ""), bool(i.get("done")))
                 for i in cfg.get("todo", [])][:6]
        img = render_todo(items)
    elif card == CARD_GALLERY:
        path = cfg.get("photo", "")
        if path and os.path.exists(path):
            img = render_photo(path)

    if img is None:
        return pkts

    data = pack(img)
    pkts.append(bytes([0xCB, W & 0xFF, W >> 8, H & 0xFF, H >> 8]))
    for i in range(0, len(data), 128):
        pkts.append(bytes([0xCC]) + b"\x00" * 7 + data[i:i + 128])
    pkts.append(bytes([0xCD]))
    return pkts


def load_config():
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _signature(cfg):
    """内容签名: 变了才重推"""
    card = int(cfg.get("card", CARD_CALENDAR))
    if card == CARD_WEATHER:
        from weather import get_weather
        wx = get_weather(cfg.get("city", "Beijing"))
        return (card, wx.get("city"), wx.get("desc"), wx.get("temp"),
                wx.get("hi"), wx.get("lo"), wx.get("hum"), wx.get("kind"))
    if card == CARD_TODO:
        return (card, tuple((i.get("text", ""), bool(i.get("done")))
                            for i in cfg.get("todo", [])))
    if card == CARD_GALLERY:
        path = cfg.get("photo", "")
        try:
            mt = os.path.getmtime(path) if path else 0
        except OSError:
            mt = 0
        return (card, path, mt)
    return (card,)


class CardPusher:
    """按 gui_config.json 维护屏幕卡片, 挂在每秒的推送循环里"""

    def __init__(self):
        self.sig = None
        self._cfg_mtime = 0
        self._cfg = {}

    def _reload(self, force=False):
        try:
            m = os.path.getmtime(CONFIG_PATH)
        except OSError:
            return
        if force or m != self._cfg_mtime:
            self._cfg_mtime = m
            self._cfg = load_config()

    def _prepare(self, force):
        """在后台线程里做: 抓天气 / 渲染可能需要几秒"""
        sig = _signature(self._cfg)
        if sig == self.sig and not force:
            return sig, None
        return sig, build_card_packets(self._cfg)

    async def tick(self, write, force=False):
        self._reload(force)
        sig, pkts = await asyncio.to_thread(self._prepare, force)
        if pkts is None:
            return
        self.sig = sig
        for p in pkts:
            await write(p)
