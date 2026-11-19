"""ota.py - 通过 BLE 给副屏升级固件

原理 (与固件 src/epd/spi_flash.c 的 ota_handle 对应):
  Flash 里有两个槽, 各带 64 字节 0x7051 头, 头里第 3 字节是序列号, 引导器挑大的启动。
  升级只写非活动槽, 第 0 页(含头)最后才落盘, 所以中途断连/断电不会变砖。

用法:
    python tools\\ota.py                           # 自动扫描设备, 用 build 里的 bin
    python tools\\ota.py 18:BC:5A:38:28:81
    python tools\\ota.py --bin firmware\\release\\pcmonitor_middle.bin
    python tools\\ota.py --keep-agent              # 升级后不恢复后台推送
"""
import argparse
import asyncio
import os
import struct
import subprocess
import sys
import time

import psutil
from bleak import BleakClient

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pc_agent as agent

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TASK = "PcMonitorAgent"
GUI_EXE = "PcMonitorGUI"

PAGE = 256          # 固件里的 flash 页
HDR = 64            # 槽头长度
CHUNK = 128         # 0xA2/0xA3 各带 128 字节


def _sh(*args):
    return subprocess.run(["schtasks"] + list(args), stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL,
                          creationflags=0x08000000).returncode


def _procs(keyword):
    out = []
    for p in psutil.process_iter(["name", "cmdline"]):
        name = (p.info.get("name") or "").lower()
        if not name.startswith("python"):
            continue
        cmd = p.info.get("cmdline") or []
        if any(keyword in str(a) for a in cmd):
            out.append(p)
    return out


def agent_stop():
    _sh("/End", "/TN", TASK)
    _sh("/Change", "/TN", TASK, "/DISABLE")
    for p in _procs("pc_agent.py"):
        try:
            p.kill()
        except Exception:
            pass
    deadline = time.time() + 6
    while _procs("pc_agent.py") and time.time() < deadline:
        time.sleep(0.2)
    return not _procs("pc_agent.py")


def agent_start():
    _sh("/Change", "/TN", TASK, "/ENABLE")
    _sh("/Run", "/TN", TASK)


def crc32(data, crc=0):
    """与固件 spi_flash.c 的 crc32(0, buf, n) 完全一致 (无初值/结果异或)"""
    for b in data:
        crc = (crc >> 8) ^ _TAB[(crc ^ b) & 0xFF]
    return crc & 0xFFFFFFFF


def _crc32_table():
    tab = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        tab.append(c)
    return tab


_TAB = _crc32_table()


def load_version(binf):
    """读 build.py 生成的 pcmonitor.json; 没有就返回 0"""
    import json
    for cand in (binf + ".json", os.path.join(os.path.dirname(binf), "pcmonitor.json")):
        if not os.path.exists(cand):
            continue
        try:
            meta = json.load(open(cand, encoding="utf-8"))
        except Exception:
            continue
        if meta.get("size") in (None, os.path.getsize(binf)):
            return int(meta.get("version", 0))
    return 0


def build_packet(binf):
    """64 字节槽头 + 镜像, 补齐到 256 的整数倍"""
    data = open(binf, "rb").read()
    size = len(data)
    crc = crc32(data)
    ver = load_version(binf)

    hdr = bytearray(b"\xff" * HDR)
    hdr[0:4] = b"\x70\x51\xaa\xff"        # 序列号 0xFF = 未提交, 由固件提交时改写
    struct.pack_into("<I", hdr, 4, size)
    struct.pack_into("<I", hdr, 8, crc)
    struct.pack_into("<I", hdr, 28, ver)

    blob = bytes(hdr) + data
    pad = (-len(blob)) % PAGE
    blob += b"\xff" * pad
    return blob, size, crc, ver


def pkt_start(size, ver, crc):
    return bytes([0xA0, 0, 0, 0]) + struct.pack("<III", size, ver, crc)


def pkt_lo(data):
    return bytes([0xA2]) + b"\x00" * 7 + data


def pkt_hi(data):
    return bytes([0xA3]) + b"\x00" * 7 + data


def pkt_end():
    return bytes([0xA4, 0, 0, 0])


async def run(args):
    binf = args.bin or os.path.join(ROOT, "firmware", "pcmonitor", "build", "pcmonitor.bin")
    if not os.path.exists(binf):
        binf = os.path.join(ROOT, "firmware", "release", "pcmonitor_middle.bin")
    if not os.path.exists(binf):
        print("找不到固件:", binf)
        return 1

    blob, size, crc, ver = build_packet(binf)
    pages = len(blob) // PAGE
    print("固件  : %s" % binf)
    print("镜像  : %d 字节, version=0x%08X, crc=0x%08X" % (size, ver, crc))
    print("包大小: %d 字节 / %d 页" % (len(blob), pages))

    if not args.keep_agent:
        print("暂停后台推送 ...")
        if not agent_stop():
            print("!! 后台推送进程没停掉, 请手动结束后再试")
            return 1
    await asyncio.sleep(2)

    addr = args.addr or await agent.find_device(None)
    if not addr:
        print("!! 没扫描到设备")
        return 1

    print("连接 %s ..." % addr)
    try:
        client = BleakClient(addr, timeout=20.0)
        await client.connect()
    except Exception as e:
        print("!! 连接失败:", repr(e))
        return 1
    print("已连接, MTU=%s" % getattr(client, "mtu_size", "?"))

    ok = False
    try:
        await client.write_gatt_char(agent.CHAR_UUID, pkt_start(size, ver, crc),
                                     response=True)
        t0 = time.time()
        for i in range(pages):
            page = blob[i * PAGE:(i + 1) * PAGE]
            await client.write_gatt_char(agent.CHAR_UUID, pkt_lo(page[:CHUNK]), response=True)
            await client.write_gatt_char(agent.CHAR_UUID, pkt_hi(page[CHUNK:]), response=True)
            if args.delay:
                await asyncio.sleep(args.delay)
            if i % 20 == 0 or i == pages - 1:
                pct = (i + 1) * 100 // pages
                print("  %3d%%  %d/%d 页  %.1fs" % (pct, i + 1, pages, time.time() - t0))
        print("发送完成, 提交校验 ...")
        try:
            await client.write_gatt_char(agent.CHAR_UUID, pkt_end(), response=True)
        except Exception as e:
            # 校验通过时固件会立刻软复位, 断连是预期行为
            print("  (连接已断开: %s)" % e)
        ok = True
    except Exception as e:
        print("!! 传输失败:", repr(e))
    finally:
        try:
            if client.is_connected:
                await client.disconnect()
        except Exception:
            pass

    if not ok:
        return 1

    # 等设备重启并重新广播, 再连一次确认活着
    print("等待设备重启 ...")
    for i in range(10):
        await asyncio.sleep(2)
        try:
            async with BleakClient(addr, timeout=10.0) as c2:
                p = agent.build_packet(psutil.net_io_counters(), time.time())[0]
                await c2.write_gatt_char(agent.CHAR_UUID, p, response=True)
            print("升级完成: 设备已重启并接受数据 (version=0x%08X)" % ver)
            return 0
        except Exception as e:
            if i == 9:
                print("!! 重启后连接失败:", repr(e))
                return 1
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", nargs="?", default=None, help="设备 MAC, 不填则扫描")
    ap.add_argument("--bin", default=None, help="要升级的 bin")
    ap.add_argument("--delay", type=float, default=0.0, help="每页额外延时(秒)")
    ap.add_argument("--keep-agent", action="store_true", help="结束后不恢复后台推送")
    args = ap.parse_args()

    for p in psutil.process_iter(["name"]):
        if (p.info.get("name") or "").lower().startswith(GUI_EXE.lower()):
            print("!! 请先关闭上位机 PcMonitorGUI, 它正占用 BLE 连接")
            return 1

    try:
        rc = asyncio.run(run(args))
    finally:
        if not args.keep_agent:
            print("恢复后台推送 ...")
            agent_start()
    print("完成" if rc == 0 else "失败")
    return rc


if __name__ == "__main__":
    sys.exit(main())
