# -*- coding: utf-8 -*-
"""
一键蓝牙对时 (一次性命令, 不常驻, 不需要后台服务)

设备已经改成独立万年历: 上电自己走时, 断电重启从 Flash 恢复时间。
本脚本只在你想校准时间时手动跑一次, 连上 -> 写 0x91 -> 断开, 全程几秒。

用法:
    python tools/sync_time.py                         # 自动扫描 DLG-CLOCK-* 并对时
    python tools/sync_time.py 18:BC:5A:5A:22:CA      # 指定 MAC, 更快
    python tools/sync_time.py --set "2026-11-19 08:30:00"
    python tools/sync_time.py --reset-bt             # 连接异常时关/开蓝牙 6 秒再连
    python tools/sync_time.py --nrf-help             # 打印手机 nRF Connect 手动写法

协议 (写入特征值 0000ff01-0000-1000-8000-00805f9b34fb, Write Request):
    91  YY_lo YY_hi  MM  DD  HH  MM  SS   ...
    YY = 年 (小端 16 位), MM = 月 (0-based: 0=1月 ... 11=12月),
    DD = 日 (1-based), HH/MM/SS = 时/分/秒。
    最少 8 字节, 后面可补 0。固件收到后自己反算农历/星期/节气/节日,
    并把时间写入 SPI Flash, 蓝牙断开后完全独立运行。
"""
import argparse
import asyncio
import datetime
import sys

from bleak import BleakClient, BleakScanner


async def reset_bluetooth(hold=6.0):
    """Windows Realtek 适配器反复重连后会进坏状态(服务发现失败/立刻断链)。
    用 WinRT Radio API 关 6 秒再开即可恢复, 不需要管理员权限。"""
    try:
        from winrt.windows.devices.radios import Radio, RadioState, RadioKind
    except ImportError:
        print("(未安装 winrt, 跳过蓝牙复位: pip install winrt-Windows.Devices.Radios)")
        return False

    await Radio.request_access_async()
    radios = await Radio.get_radios_async()
    bt = [r for r in radios if r.kind == RadioKind.BLUETOOTH]
    if not bt:
        print("(没找到蓝牙无线电设备)")
        return False
    for r in bt:
        print(f"关闭蓝牙 {r.name} ...")
        await r.set_state_async(RadioState.OFF)
    await asyncio.sleep(hold)
    for r in bt:
        print(f"重新打开蓝牙 {r.name} ...")
        await r.set_state_async(RadioState.ON)
    await asyncio.sleep(2.0)
    print("蓝牙已复位")
    return True

CHAR_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"
NAME_PREFIX = "DLG-CLOCK-"
DEFAULT_ADDR = None          # 可把自己的设备 MAC 写在这里, 省去扫描


def build_clock_packet(when: datetime.datetime) -> bytes:
    return bytes([
        0x91,
        when.year & 0xFF, (when.year >> 8) & 0xFF,
        when.month - 1, when.day,
        when.hour, when.minute, when.second,
        0, 0, 0, 0,
    ])


def decode_clock_reply(b: bytes) -> str:
    # 写入 ff01 会同时更新该特征值的数据库内容, 回读通常就是我们刚写的 0x91 包;
    # 真正的当前时间在连接时由固件 clock_push() 写入, 需要连上后立刻读才拿得到。
    if not b:
        return "空"
    if b[0] == 0x91:
        return f"写入回显(0x91 包已送达): {bytes(b[:8]).hex(' ')}"
    if len(b) < 7:
        return f"数据过短: {bytes(b).hex(' ')}"
    y = b[0] | (b[1] << 8)
    return (f"{y:04d}-{b[2] + 1:02d}-{b[3]:02d} "
            f"{b[4]:02d}:{b[5]:02d}:{b[6]:02d}")


async def find_device(preferred):
    if preferred:
        return preferred
    print("扫描 BLE 设备（10 秒）...")
    for d in await BleakScanner.discover(timeout=10.0):
        if (d.name or "").startswith(NAME_PREFIX):
            print(f"找到设备: {d.name}  {d.address}")
            return d.address
    return None


async def run(addr, when, auto_time=True):
    packet = build_clock_packet(when)
    print(f"连接 {addr} ... (Windows Realtek 蓝牙服务发现可能要 20~30 秒)")
    async with BleakClient(addr, timeout=60.0) as client:
        # 首次写入偶发失败, 重试 3 次 (与 pc_agent 相同的规避)
        for attempt in range(3):
            try:
                await client.write_gatt_char(CHAR_UUID, packet, response=True)
                break
            except Exception as e:
                if attempt == 2:
                    raise
                print(f"  第 {attempt + 1} 次写入失败({e}), 0.5 秒后重试")
                await asyncio.sleep(0.5)
        print(f"已发送对时包: {packet[:8].hex(' ')}  ->  "
              f"{when:%Y-%m-%d %H:%M:%S}")
        try:
            reply = await client.read_gatt_char(CHAR_UUID)
            print(f"设备回读时间: {decode_clock_reply(bytes(reply))}")
        except Exception as e:
            print(f"(回读失败, 不影响对时: {e})")

        # Windows 侧首次操作会触发很慢的 GATT 服务发现, 期间固件定时器被挤占,
        # 第一次写入的时间会慢几秒。发现完成后再写一包即时时间做精确校准。
        final = datetime.datetime.now() if auto_time else when
        packet2 = build_clock_packet(final)
        await client.write_gatt_char(CHAR_UUID, packet2, response=True)
        print(f"精确校准: {packet2[:8].hex(' ')}  ->  {final:%Y-%m-%d %H:%M:%S}")
    print("已断开。设备现在离线独立走时, 断电重启也会从 Flash 恢复。")


def main():
    ap = argparse.ArgumentParser(description="独立万年历 蓝牙一键对时")
    ap.add_argument("addr", nargs="?", default=None, help="设备 MAC, 不填则自动扫描")
    ap.add_argument("--set", dest="manual", default=None,
                    help='手动指定时间, 如 "2026-11-19 08:30:00"')
    ap.add_argument("--reset-bt", action="store_true",
                    help="连接前先关/开 Windows 蓝牙适配器 6 秒(修复 Realtek 坏状态)")
    ap.add_argument("--nrf-help", action="store_true",
                    help="打印手机 nRF Connect 手动对时步骤")
    args = ap.parse_args()

    if args.nrf_help:
        print("""
手机 nRF Connect 手动对时:
  1. 扫描并连接 DLG-CLOCK-xxxxxx
  2. 展开服务 UUID FF00, 找到特征 UUID FF01
  3. 点写入(Write Request / 向上箭头), 值类型选 BYTE ARRAY / HEX
  4. 输入 8 字节: 91 <年低> <年高> <月-1> <日> <时> <分> <秒>
     例 2026-11-19 08:30:00 -> 91 EA 07 0A 13 08 1E 00
        (2026 = 0x07EA, 11月 = 0x0A, 19日 = 0x13, 30分 = 0x1E)
  5. 发送后屏幕立即刷新, 右上角蓝牙图标绿色; 断开即可, 设备自己走时。
""".strip())
        return

    if args.manual:
        try:
            when = datetime.datetime.strptime(args.manual, "%Y-%m-%d %H:%M:%S")
        except ValueError:
            raise SystemExit('时间格式应为 "YYYY-MM-DD HH:MM:SS"')
    else:
        when = datetime.datetime.now()

    if args.reset_bt:
        asyncio.run(reset_bluetooth())

    auto_time = args.manual is None
    try:
        asyncio.run(run_sync(args.addr, when, auto_time))
    except Exception as e:
        # Windows Realtek 适配器被反复重连搞坏时, 关开蓝牙后自动再试一次
        if args.reset_bt:
            raise
        print(f"连接异常 ({e}), 复位蓝牙后重试一次...")
        asyncio.run(reset_bluetooth())
        asyncio.run(run_sync(args.addr, when, auto_time))


async def run_sync(addr, when, auto_time=True):
    addr = addr or DEFAULT_ADDR
    for attempt in range(2):
        addr = await find_device(addr)
        if addr:
            break
        print("没找到设备, 5 秒后重试...")
        addr = None
        await asyncio.sleep(5)
    if not addr:
        raise SystemExit("未找到 DLG-CLOCK-* 设备, 请确认价签已上电且在广播")
    await run(addr, when, auto_time)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n已取消")
    except Exception as e:
        print(f"对时失败: {e}")
        sys.exit(1)
