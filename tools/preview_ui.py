# -*- coding: utf-8 -*-
"""按 monitor_ui.c 的布局渲染 1:1 预览图。
布局常量需要和 firmware/pcmonitor/src/ui/monitor_ui.c 保持一致。"""
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "ui_preview.png")
CONSOLA = r"C:\Windows\Fonts\consola.ttf"
MDL2 = r"C:\Windows\Fonts\segmdl2.ttf"

MED = ImageFont.truetype(CONSOLA, 26)
BIG = ImageFont.truetype(CONSOLA, 52)
TINY = ImageFont.truetype(CONSOLA, 14)
BT = ImageFont.truetype(MDL2, 26)

MED_ADV, MED_ASC = 14, 20
TINY_ADV = 8
BIG_ADV, BIG_H, BIG_ASC = 29, 53, 39

C_BG = (0, 0, 0)
C_LINE = (33, 36, 33)
C_WHITE = (255, 255, 255)
C_CYAN = (0, 255, 255)
C_GREEN = (0, 255, 0)
C_YELLOW = (255, 255, 0)
C_RED = (255, 0, 0)
C_GRAY = (132, 130, 132)

LEFT_X0, LEFT_X1 = 4, 96
RIGHT_X0, RIGHT_X1 = 114, 316
RIGHT_CX = (RIGHT_X0 + RIGHT_X1) // 2
BAND_X0, BAND_X1 = 98, 112

NET_LBL_Y = 4
DL_LBL_Y, DL_VAL_Y = 24, 40
UL_LBL_Y, UL_VAL_Y = 72, 88
DIV_L_Y = 120
CK_LBL_Y = 126
CK_VAL_Y = 148
CK_BAR_Y = 178
BT_X, BT_Y = 4, 206
HOST_X, HOST_Y = 22, 210

TIME_Y = 2
DIV_R_Y = 60
CPU_LBL_Y, CPU_VAL_Y, CPU_BAR_Y = 77, 68, 98
GPU_LBL_Y, GPU_VAL_Y, GPU_BAR_Y = 125, 116, 146
RAM_LBL_Y, RAM_VAL_Y, RAM_BAR_Y = 173, 164, 194
RAM_USED_Y = 206
TEMP_X = 146

INSET = [2, 1, 0, 0, 0, 0, 1, 2]


def text(d, x, y, s, font, adv, color):
    cx = x
    for ch in s:
        d.text((cx, y), ch, font=font, fill=color)
        cx += adv


def text_right(d, right, y, s, font, adv, color):
    text(d, right - len(s) * adv + 1, y, s, font, adv, color)


def bar(d, x0, y0, x1, pct, color):
    w = x1 - x0 + 1
    wf = w * pct // 100
    for i in range(8):
        xa, xb = x0 + INSET[i], x1 - INSET[i]
        if xa > xb:
            continue
        d.rectangle([xa, y0 + i, xb, y0 + i], fill=C_LINE)
        if wf <= INSET[i]:
            continue
        xe = min(x0 + wf - 1, xb) if wf < w else xb
        if xe >= xa:
            d.rectangle([xa, y0 + i, xe, y0 + i], fill=color)


def bt_icon(img, x, y, color):
    tmp = Image.new("L", (40, 40), 0)
    ImageDraw.Draw(tmp).text((6, 6), "\ue702", font=BT, fill=255)
    glyph = tmp.crop(tmp.getbbox())
    img.paste(Image.new("RGB", glyph.size, color), (x, y), glyph)


def main():
    img = Image.new("RGB", (320, 240), C_BG)
    d = ImageDraw.Draw(img)

    # ---- 左栏 ----
    try:
        lbl = Image.open(os.path.join(HERE, "net_label.png")).resize((92, 15), Image.LANCZOS)
        img.paste(Image.new("RGB", lbl.size, C_WHITE), (LEFT_X0, NET_LBL_Y), lbl)
    except (FileNotFoundError, OSError):
        text(d, LEFT_X0, NET_LBL_Y, "LAN ETH", TINY, TINY_ADV, C_WHITE)

    text(d, LEFT_X0, DL_LBL_Y, "DL", TINY, TINY_ADV, C_GRAY)
    text_right(d, LEFT_X1, DL_VAL_Y, "1.2M/s", MED, MED_ADV, C_WHITE)
    text(d, LEFT_X0, UL_LBL_Y, "UL", TINY, TINY_ADV, C_GRAY)
    text_right(d, LEFT_X1, UL_VAL_Y, "850K/s", MED, MED_ADV, C_WHITE)

    d.rectangle([LEFT_X0, DIV_L_Y, LEFT_X1, DIV_L_Y], fill=C_LINE)
    text(d, LEFT_X0, CK_LBL_Y, "C: DISK", TINY, TINY_ADV, C_GRAY)
    text_right(d, LEFT_X1, CK_VAL_Y, "94%", MED, MED_ADV, C_RED)
    bar(d, LEFT_X0, CK_BAR_Y, LEFT_X1, 94, C_RED)

    bt_icon(img, BT_X, BT_Y, C_GREEN)
    text(d, HOST_X, HOST_Y, "DESKTOP-7", TINY, TINY_ADV, C_WHITE)

    # ---- 右栏 ----
    hm, ss = "20:15", "33"
    x0 = RIGHT_CX - (5 * BIG_ADV + 2 * MED_ADV) // 2
    text(d, x0, TIME_Y, hm, BIG, BIG_ADV, C_CYAN)
    text(d, x0 + 5 * BIG_ADV, TIME_Y + BIG_ASC - MED_ASC, ss, MED, MED_ADV, C_CYAN)
    d.rectangle([RIGHT_X0, DIV_R_Y, RIGHT_X1, DIV_R_Y], fill=C_LINE)

    text(d, RIGHT_X0, CPU_LBL_Y, "CPU", TINY, TINY_ADV, C_GRAY)
    text(d, TEMP_X, CPU_LBL_Y, "62\u00b0C", TINY, TINY_ADV, C_GRAY)
    text_right(d, RIGHT_X1, CPU_VAL_Y, "55%", MED, MED_ADV, C_GREEN)
    bar(d, RIGHT_X0, CPU_BAR_Y, RIGHT_X1, 55, C_GREEN)

    text(d, RIGHT_X0, GPU_LBL_Y, "GPU", TINY, TINY_ADV, C_GRAY)
    text(d, TEMP_X, GPU_LBL_Y, "54\u00b0C", TINY, TINY_ADV, C_GRAY)
    text_right(d, RIGHT_X1, GPU_VAL_Y, "23%", MED, MED_ADV, C_GREEN)
    bar(d, RIGHT_X0, GPU_BAR_Y, RIGHT_X1, 23, C_GREEN)

    text(d, RIGHT_X0, RAM_LBL_Y, "RAM", TINY, TINY_ADV, C_GRAY)
    text_right(d, RIGHT_X1, RAM_VAL_Y, "74%", MED, MED_ADV, C_YELLOW)
    bar(d, RIGHT_X0, RAM_BAR_Y, RIGHT_X1, 74, C_YELLOW)
    text(d, RIGHT_X0, RAM_USED_Y, "8.0/16.0GB", TINY, TINY_ADV, C_GRAY)

    # ---- 漏液带保护区 ----
    band = Image.new("RGBA", (BAND_X1 - BAND_X0 + 1, 240), (200, 0, 0, 70))
    img.paste(Image.alpha_composite(
        img.crop((BAND_X0, 0, BAND_X1 + 1, 240)).convert("RGBA"), band).convert("RGB"),
        (BAND_X0, 0))
    d.line([BAND_X0, 0, BAND_X0, 239], fill=(0, 60, 90))
    d.line([BAND_X1, 0, BAND_X1, 239], fill=(0, 60, 90))

    img.resize((640, 480), Image.NEAREST).save(OUT)
    print("written:", OUT)


if __name__ == "__main__":
    main()
