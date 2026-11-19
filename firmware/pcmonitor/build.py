# -*- coding: utf-8 -*-
"""pcmonitor 固件构建脚本 (自包含, 不依赖 Keil 工程文件)

用法:
    python build.py                     # 自动在脚本所在目录的 4 级上层找 SDK
    python build.py --sdk D:\\SDK_6.0.22.1401\\DA145xx_SDK\\6.0.22.1401
    python build.py --gcc "C:\\...\\bin"  # 指定 arm-none-eabi-gcc 所在目录

产物: build/pcmonitor.bin / pcmonitor.hex
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REL_PROJ = "projects/target_apps/ble_examples/pcmonitor/"

# 需要 lib_extract 里预编译的 SDK 目标文件 (Keil .lib 解包)
LIB_NAME = "da14585_586.lib"


def _crc32_table():
    tab = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        tab.append(c)
    return tab


CRC32_TAB = _crc32_table()      # 与 spi_flash.c 的 crc32_tab 相同


def find_sdk(arg):
    """确定 SDK 根目录"""
    cand = []
    if arg:
        cand.append(arg)
    if os.environ.get("DA14585_SDK"):
        cand.append(os.environ["DA14585_SDK"])
    # 脚本位于 <SDK>/projects/target_apps/ble_examples/pcmonitor 时向上找 4 级
    p = HERE
    for _ in range(4):
        p = os.path.dirname(p)
    cand.append(p)
    for c in cand:
        if c and os.path.isdir(os.path.join(c, "sdk")):
            return os.path.abspath(c)
    raise SystemExit("找不到 SDK, 请用 --sdk 指定 DA145xx_SDK/6.0.22.1401 的路径")


def find_gcc(arg):
    if arg:
        exe = os.path.join(arg, "arm-none-eabi-gcc.exe")
        if os.path.isfile(exe):
            return arg
        raise SystemExit("--gcc 目录里没有 arm-none-eabi-gcc.exe: " + arg)
    which = shutil.which("arm-none-eabi-gcc")
    if which:
        return os.path.dirname(which)
    # 常见安装位置
    for base in (r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi",
                 r"C:\Program Files\Arm GNU Toolchain arm-none-eabi",
                 os.path.expanduser(r"~\AppData\Local\Programs\arm-gnu-toolchain")):
        if not os.path.isdir(base):
            continue
        for d in sorted(os.listdir(base), reverse=True):
            b = os.path.join(base, d, "bin")
            if os.path.isfile(os.path.join(b, "arm-none-eabi-gcc.exe")):
                return b
    raise SystemExit("找不到 arm-none-eabi-gcc, 请安装 Arm GNU Toolchain 或用 --gcc 指定")


def load_list(name):
    path = os.path.join(HERE, name)
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip().replace(REL_PROJ, "{PROJ}/")
            if line:
                out.append(line)
    return out


def resolve(item, sdk):
    if item.startswith("{PROJ}/"):
        return os.path.join(HERE, item[len("{PROJ}/"):].replace("/", os.sep))
    return os.path.join(sdk, item.replace("/", os.sep))


def obj_path(src, build):
    tag = re.sub(r"[^A-Za-z0-9]", "_", src)
    h = hashlib.md5(tag.encode()).hexdigest()[:6]
    name = os.path.basename(src)
    if name == "jump_table.c":
        return os.path.join(build, "jump_table.o")   # 链接脚本按名字摘取 heap 段
    return os.path.join(build, name + "." + h + ".o")


def extract_libs(sdk, outdir):
    """Keil .lib 是 ar 归档, 直接解包出 .o 供 GCC 链接"""
    lib = os.path.join(sdk, "sdk", "platform", "system_library", "output", "Keil_5", LIB_NAME)
    if not os.path.isfile(lib):
        raise SystemExit("缺少 SDK 预编译库: " + lib)
    data = open(lib, "rb").read()
    if data[:8] != b"!<arch>\n":
        raise SystemExit("不是有效的 ar 归档: " + lib)
    entries, off = [], 8
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        name = hdr[0:16].decode("ascii", "ignore").rstrip()
        size = int(hdr[48:58].decode().strip())
        entries.append((name, data[off + 60:off + 60 + size]))
        off += 60 + size + (size & 1)
    longnames = b"".join(b for n, b in entries if n == "//")
    os.makedirs(outdir, exist_ok=True)
    for name, body in entries:
        if name in ("/", "//"):
            continue
        if name.startswith("/"):
            idx = int(name[1:])
            end = longnames.find(b"/\n", idx)
            real = longnames[idx:end].decode("ascii", "ignore").rstrip("/")
        else:
            real = name.rstrip("/")
        with open(os.path.join(outdir, real), "wb") as f:
            f.write(body)
    print(f"已解包 SDK 预编译库 -> {outdir}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk")
    ap.add_argument("--gcc")
    args = ap.parse_args()

    sdk = find_sdk(args.sdk)
    gcc_dir = find_gcc(args.gcc)
    panel = os.environ.get("PANEL_MIDDLE", "0")     # 0=侧灯版, 1=中间灯版
    extra = os.environ.get("EXTRA_CFLAGS", "").strip()
    build = os.path.join(HERE, "build")
    os.makedirs(build, exist_ok=True)

    gcc = os.path.join(gcc_dir, "arm-none-eabi-gcc.exe")
    cpp = os.path.join(gcc_dir, "arm-none-eabi-cpp.exe")
    objcopy = os.path.join(gcc_dir, "arm-none-eabi-objcopy.exe")

    includes = [resolve(i, sdk) for i in load_list("includes.txt")]
    includes += [os.path.join(HERE, "src", "lcd"), os.path.join(HERE, "src", "ui")]

    cflags = [
        "-mcpu=cortex-m0", "-mthumb", "-Os", "-g0", "-std=gnu99",
        "-ffunction-sections", "-fdata-sections", "-fno-strict-aliasing",
        "-Wno-attributes", "-Wno-unused-function", "-Wno-incompatible-pointer-types",
        "-include", os.path.join(HERE, "src", "config", "da1458x_config_basic.h"),
        "-include", os.path.join(HERE, "src", "config", "da1458x_config_advanced.h"),
        "-include", os.path.join(HERE, "src", "config", "user_config.h"),
    ] + [f"-I{p}" for p in includes]

    asflags = ["-mcpu=cortex-m0", "-mthumb", "-x", "assembler-with-cpp",
               "-I", os.path.join(sdk, "sdk", "platform", "include", "CMSIS", "5.9.0",
                                  "CMSIS", "Core", "Include")]

    srcs = [resolve(s, sdk) for s in load_list("sources.txt")]
    startup = [
        os.path.join(sdk, "sdk", "platform", "arch", "boot", "GCC", "startup_DA14585_586.S"),
        os.path.join(sdk, "sdk", "platform", "arch", "boot", "GCC", "ivtable_DA14585_586.S"),
        os.path.join(HERE, "startup", "crt0_pcmonitor.c"),
    ]
    all_src = srcs + startup

    # 版本号 = 源码 + src 下所有头文件内容 hash。
    # selflash() 靠版本号判断是否需要把当前固件写入 Flash, 只改头文件也会更新。
    vfiles = set(all_src)
    for root, _, names in os.walk(os.path.join(HERE, "src")):
        for n in names:
            if n.endswith((".c", ".h")):
                vfiles.add(os.path.join(root, n))
    hv = hashlib.md5()
    for s in sorted(vfiles):
        try:
            with open(s, "rb") as f:
                hv.update(os.path.basename(s).encode())
                hv.update(f.read())
        except OSError:
            pass
    ver = 0xA5000000 | (int(hv.hexdigest()[:6], 16) & 0xFFFFFF)
    hv.update(("PANEL_MIDDLE=" + panel).encode())     # 面板版本也参与版本号
    if extra:
        hv.update(("EXTRA=" + extra).encode())
    ver = 0xA5000000 | (int(hv.hexdigest()[:6], 16) & 0xFFFFFF)
    cflags.append(f"-DPANEL_MIDDLE={panel}")
    if extra:
        cflags += extra.split()

    verf = os.path.join(build, ".srcver")
    old_ver = None
    if os.path.exists(verf):
        try:
            old_ver = int(open(verf).read().strip(), 16)
        except ValueError:
            old_ver = None
    if old_ver != ver:
        for f in os.listdir(build):
            if f.endswith(".o"):
                os.remove(os.path.join(build, f))
        with open(verf, "w") as f:
            f.write(f"{ver:08X}\n")
    print(f"firmware version: 0x{ver:08X}")

    def compile_one(src):
        obj = obj_path(src, build)
        if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
            return obj, "cached", None
        flags = asflags if src.lower().endswith(".s") else cflags
        r = subprocess.run([gcc] + flags + ["-c", src, "-o", obj],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return obj, "FAIL", r.stderr[-3000:]
        return obj, "ok", None

    print(f"SDK : {sdk}")
    print(f"GCC : {gcc_dir}")
    print(f"编译 {len(all_src)} 个文件...")
    fails = []
    with ThreadPoolExecutor(max_workers=8) as ex:
        for obj, status, err in ex.map(compile_one, all_src):
            if status == "FAIL":
                fails.append((obj, err))
    if fails:
        for obj, err in fails[:3]:
            print(f"===== {os.path.basename(obj)} =====")
            print(err)
        sys.exit(1)

    # 预处理链接脚本
    lds_in = os.path.join(sdk, "sdk", "common_project_files", "ldscripts",
                          "ldscript_DA14585_586.lds.S")
    lds_out = os.path.join(build, "ldscript.ld")
    cmd = [cpp, "-E", "-P", "-x", "c"] + [f"-I{p}" for p in includes] + [
        "-imacros", os.path.join(HERE, "src", "config", "da1458x_config_basic.h"),
        "-imacros", os.path.join(HERE, "src", "config", "da1458x_config_advanced.h"),
        "-imacros", os.path.join(HERE, "src", "config", "user_config.h"),
        lds_in, "-o", lds_out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("链接脚本预处理失败:", r.stderr[-2000:])
        sys.exit(1)

    lib_dir = os.path.join(build, "lib_extract")
    if not os.path.isdir(lib_dir) or not any(f.endswith(".o") for f in os.listdir(lib_dir)):
        extract_libs(sdk, lib_dir)
    prelib = [os.path.join(lib_dir, f) for f in os.listdir(lib_dir) if f.endswith(".o")]

    elf = os.path.join(build, "pcmonitor.elf")
    cmd = [gcc, "-mcpu=cortex-m0", "-mthumb", "-nostartfiles", "-specs=nano.specs",
           "-T", lds_out,
           "-L", os.path.join(sdk, "sdk", "common_project_files", "misc"),
           "-Wl,--gc-sections", "-Wl,-Map," + os.path.join(build, "pcmonitor.map"),
           "-o", elf] + [obj_path(s, build) for s in all_src] + prelib + ["-lgcc", "-lc", "-lnosys"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("链接失败:", r.stderr[-4000:])
        sys.exit(1)

    binf = os.path.join(build, "pcmonitor.bin")
    hexf = os.path.join(build, "pcmonitor.hex")
    subprocess.run([objcopy, "-O", "binary", elf, binf], check=True)
    subprocess.run([objcopy, "-O", "ihex", elf, hexf], check=True)

    # 给 OTA 工具用的元信息: 版本号 / 长度 / CRC (与固件里 crc32(0,...) 同算法)
    data = open(binf, "rb").read()
    crc = 0
    for byte in data:
        crc = (crc >> 8) ^ CRC32_TAB[(crc ^ byte) & 0xFF]
    with open(os.path.join(build, "pcmonitor.json"), "w", encoding="utf-8") as f:
        json.dump({"version": ver, "size": len(data), "crc": crc}, f, indent=2)

    print(f"\n完成: {binf} ({os.path.getsize(binf)} 字节)")
    print(f"      {hexf}")


if __name__ == "__main__":
    main()
