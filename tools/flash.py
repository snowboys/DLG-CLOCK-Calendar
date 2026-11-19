# -*- coding: utf-8 -*-
"""一键烧录 pcmonitor 固件到价签 (DA14585, 代码运行在 RAM)

**只依赖 SEGGER J-Link**，不需要 ARM 工具链、不需要 SDK、不需要编译。

做的事:
  1. 读取 bin 向量表里的初始 SP / Reset 地址
  2. 用 JLink.exe 跑一段 Commander 脚本: 复位 -> 载入 RAM -> 写 SP/PC -> 启动 CPU

用法:
    python tools/flash.py
    python tools/flash.py --bin firmware/release/pcmonitor.bin
    python tools/flash.py --jlink "C:\\Program Files (x86)\\SEGGER\\JLink"
"""
import argparse
import os
import shutil
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOAD_ADDR = 0x07FC0000

JLINK_DIRS = [
    r"C:\Program Files (x86)\SEGGER\JLink",
    r"C:\Program Files\SEGGER\JLink",
    r"C:\Program Files (x86)\SEGGER\JLink_V630d",
]


def find_jlink(arg):
    for d in ([arg] if arg else []) + JLINK_DIRS:
        exe = os.path.join(d, "JLink.exe")
        if os.path.isfile(exe):
            return exe
    exe = shutil.which("JLink.exe") or shutil.which("JLink")
    if exe:
        return exe
    raise SystemExit("找不到 JLink.exe，请安装 SEGGER J-Link，或用 --jlink 指定安装目录")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "firmware", "release", "pcmonitor.bin"))
    ap.add_argument("--jlink")
    ap.add_argument("--speed", default="1000")
    args = ap.parse_args()

    if not os.path.isfile(args.bin):
        raise SystemExit("找不到固件文件: " + args.bin)
    with open(args.bin, "rb") as f:
        head = f.read(8)
    sp, pc = struct.unpack("<II", head)
    print(f"固件: {args.bin} ({os.path.getsize(args.bin)} 字节)  SP={sp:#x}  Reset={pc:#x}")

    # 复制到 ASCII 临时路径: J-Link Commander 对中文/空格路径支持不稳定
    tmp_bin = os.path.join(tempfile.gettempdir(), "pcmonitor_flash.bin")
    shutil.copyfile(args.bin, tmp_bin)

    script = os.path.join(tempfile.gettempdir(), "pcmonitor_flash.jlink")
    with open(script, "w", encoding="ascii") as f:
        f.write("device Cortex-M0\n")
        f.write("si SWD\n")
        f.write(f"speed {args.speed}\n")
        f.write("connect\n")
        f.write("r\n")                              # 复位, 清掉可能存在的 lockup
        f.write("h\n")
        f.write('loadbin "%s", 0x%08X\n' % (tmp_bin.replace(os.sep, "/"), LOAD_ADDR))
        # wreg 在部分 J-Link 版本不可用, 直接用 CoreSight 的 DCRDR/DCRSR 写 SP
        f.write("w4 0xE000EDF8, 0x%08X\n" % sp)      # DCRDR = SP
        f.write("w4 0xE000EDF4, 0x0001000D\n")       # DCRSR: REGSEL=13(R13/SP), 写
        f.write("sleep 100\n")
        f.write("SetPC 0x%08X\n" % pc)               # Commander 自带的 SetPC
        # 置 "写 Flash" 触发标记: 固件 selflash() 看到它就把当前镜像写入 SPI Flash
        # (0x07FD1000 在堆与栈之间的空闲 RAM, 启动代码不会清; 写完固件自动清零)
        f.write("w4 0x07FD1000, 0x464C5348\n")
        f.write("g\n")
        f.write("sleep 2000\n")
        f.write("qc\n")

    exe = find_jlink(args.jlink)
    print("写入 RAM 并启动...")
    r = subprocess.run([exe, "-CommanderScript", script, "-ExitOnError", "1"],
                       capture_output=True, text=True, errors="ignore")
    out = (r.stdout or "") + (r.stderr or "")

    bad = [k for k in ("Could not connect", "Illegal", "Syntax:", "Failed", "ERROR")
           if k in out]
    if "Downloading file" not in out or "Script processing completed" not in out or bad:
        print(out[-3000:])
        raise SystemExit("烧录失败" + (f" ({', '.join(bad)})" if bad else ""))

    print("完成。设备应在 1~2 秒内点亮屏幕并开始广播 DLG-CLOCK-*")


if __name__ == "__main__":
    main()
