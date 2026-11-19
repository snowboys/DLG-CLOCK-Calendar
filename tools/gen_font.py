# -*- coding: utf-8 -*-
"""生成抗锯齿(4bit alpha)字库 font_aa.h。

字库: 每个字形 = adv x h 像素, 每像素 4bit alpha, 逐行打包 (每字节 2 像素,
      高半字节为左像素)。固件按 ramp[a] 做黑底 alpha 混合。
"""
import math
import os

from PIL import Image, ImageDraw, ImageFont

FW = r"D:\SDK_6.0.22.1401\DA145xx_SDK\6.0.22.1401\projects\target_apps\ble_examples\pcmonitor"
OUT_H = os.path.join(FW, "src", "ui", "font_aa.h")

CONSOLA = r"C:\Windows\Fonts\consola.ttf"
MDL2 = r"C:\Windows\Fonts\segmdl2.ttf"

SPECS = [
    # name,  size, charset  (BIG 专用于时钟 HH:MM, 真 52px 渲染, 不再是插值放大)
    ("BIG",  52, "0123456789:"),
    ("MED",  26, " 0123456789:%.MK/BSs-"),
    ("TINY", 14, " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%:-./+<>"),
]


def load(path, size):
    f = ImageFont.truetype(path, size)
    asc, desc = f.getmetrics()
    return f, asc, desc


def pack(img):
    w, h = img.size
    bpr = (w + 1) // 2
    vals = img.tobytes()
    data = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            hi = min(15, (vals[y * w + x] * 15 + 127) // 255)
            lo = vals[y * w + x + 1] if x + 1 < w else 0
            lo = min(15, (lo * 15 + 127) // 255)
            data.append((hi << 4) | lo)
    return bytes(data)


def render_chars(f, asc, desc, chars):
    adv = max(1, int(math.ceil(max(f.getlength(c) for c in chars))))
    h = asc + desc
    out = []
    for c in chars:
        img = Image.new("L", (adv, h), 0)
        ImageDraw.Draw(img).text((0, 0), c, font=f, fill=255)
        out.append(pack(img))
    return adv, h, out


def render_icon(path, size, code):
    f, asc, desc = load(path, size)
    tmp = Image.new("L", (size * 3, size * 3), 0)
    ImageDraw.Draw(tmp).text((size, size), code, font=f, fill=255)
    box = tmp.getbbox()
    if not box:
        return None
    img = tmp.crop(box)
    return img, img.size[0], img.size[1], pack(img)


def c_array(name, data, per_line=16):
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), per_line):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + per_line])
        lines.append(f"  {chunk},")
    lines.append("};")
    return lines


def main():
    head = [
        "/* 自动生成: tools/gen_font.py — Consolas 4bit alpha 抗锯齿字库, 请勿手改 */",
        "#ifndef FONT_AA_H",
        "#define FONT_AA_H",
        "#include <stdint.h>",
        "",
    ]
    meta = []
    for name, size, chars in SPECS:
        f, asc, desc = load(CONSOLA, size)
        adv, h, data = render_chars(f, asc, desc, chars)
        total = sum(len(d) for d in data)
        head.append(f"/* {name}: {adv}x{h}px, asc={asc}, {len(chars)} 字形, 共 {total}B */")
        head.append(f"#define FONT_{name}_ADV  {adv}")
        head.append(f"#define FONT_{name}_H    {h}")
        head.append(f"#define FONT_{name}_ASC  {asc}")
        head.append(f'static const char FONT_{name}_CHARS[] = "{chars}";')
        head.extend(c_array(f"FONT_{name}_BITS", b"".join(data)))
        head.append("")
        meta.append((name, adv, h, asc, len(chars), total))

    ic = render_icon(MDL2, 26, "\ue702")
    if ic:
        img, iw, ih, idata = ic
        head.append(f"/* BT: Segoe MDL2 E702, {iw}x{ih}px, {len(idata)}B */")
        head.append(f"#define FONT_BT_W {iw}")
        head.append(f"#define FONT_BT_H {ih}")
        head.extend(c_array("FONT_BT_BITS", idata))
        head.append("")
        print(f"BT icon {iw}x{ih} {len(idata)}B")
    head.append("#endif")

    with open(OUT_H, "w", encoding="utf-8") as fp:
        fp.write("\n".join(head) + "\n")

    for m in meta:
        print(f"{m[0]}: {m[1]}x{m[2]}px asc={m[3]} {m[4]}字形 {m[5]}B")
    print("total data:", sum(m[5] for m in meta), "B")
    print("written:", OUT_H, os.path.getsize(OUT_H), "bytes")


if __name__ == "__main__":
    main()
