#!/usr/bin/env python3
"""Test OTA: Nordic Secure DFU BLE upload via bleak (same flow as the webapp)."""
import asyncio, binascii, io, json, struct, sys, time, zipfile

import serial
from bleak import BleakClient, BleakScanner

DFU_SERVICE = "0000fe59-0000-1000-8000-00805f9b34fb"
DFU_CTRL = "8ec90001-f315-4f60-9fb8-838830daea50"
DFU_PACKET = "8ec90002-f315-4f60-9fb8-838830daea50"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

PKG = sys.argv[1] if len(sys.argv) > 1 else "build/dfu_packages/initiator_legacy_ble_v2.zip"
APP_NAME = sys.argv[2] if len(sys.argv) > 2 else "UWB"
PORT = "/dev/cu.usbmodem0007602209081"


def load_pkg(path):
    z = zipfile.ZipFile(path)
    manifest = json.loads(z.read("manifest.json"))["manifest"]["application"]
    return z.read(manifest["dat_file"]), z.read(manifest["bin_file"])


async def boot_dfu_over_ble():
    """Send BOOT,DFU to the application over NUS. Returns True on success."""
    dev = await BleakScanner.find_device_by_name(APP_NAME, timeout=6)
    if not dev:
        return False
    try:
        async with BleakClient(dev, timeout=15) as client:
            await client.write_gatt_char(NUS_RX, b"BOOT,DFU\n", response=False)
            print(f"BOOT,DFU envoyé en BLE à {APP_NAME}")
            await asyncio.sleep(0.5)
        return True
    except Exception as exc:
        # une déconnexion brutale est attendue: le boîtier reset en DFU
        print(f"(déconnexion attendue: {exc})")
        return True


async def main():
    dat, bin_ = load_pkg(PKG)
    print(f"package: dat={len(dat)} B, bin={len(bin_)} B")

    # 0. skip serial phase if bootloader already advertising
    dev = await BleakScanner.find_device_by_name("UWBR_DFU", timeout=4)
    if dev:
        print("déjà en mode DFU")
    elif await boot_dfu_over_ble():
        time.sleep(1.0)
    else:
        # 1. reboot app into DFU (retry until ACK, UART RX can be flaky at full CSV rate)
        ser = serial.Serial(PORT, 460800, timeout=0.1)
        acked = False
        for attempt in range(8):
            ser.reset_input_buffer()
            ser.write(b"\nBOOT,DFU\n")
            deadline = time.time() + 1.5
            buf = b""
            while time.time() < deadline and not acked:
                buf += ser.read(4096)
                if b"ACK,BOOT_DFU" in buf:
                    acked = True
            if acked:
                print(f"ACK,BOOT_DFU (essai {attempt + 1})")
                break
        ser.close()
        if not acked:
            print("BOOT,DFU jamais acquitté")
            return 1
        time.sleep(1.0)

    # 2. find bootloader
    for _ in range(10):
        if dev:
            break
        dev = await BleakScanner.find_device_by_name("UWBR_DFU", timeout=4)
    if not dev:
        print("UWBR_DFU introuvable")
        return 1
    print(f"bootloader: {dev.address}")

    resp_q: asyncio.Queue = asyncio.Queue()

    async with BleakClient(dev) as client:
        mtu = client.mtu_size
        psize = max(20, mtu - 3)
        print(f"MTU={mtu}, packet size={psize}")

        def on_notify(_, data):
            resp_q.put_nowait(bytes(data))

        await client.start_notify(DFU_CTRL, on_notify)

        async def req(op, payload=b""):
            await client.write_gatt_char(DFU_CTRL, bytes([op]) + payload, response=True)
            r = await asyncio.wait_for(resp_q.get(), 10)
            assert r[0] == 0x60 and r[1] == op, f"resp {r.hex()}"
            assert r[2] == 0x01, f"op 0x{op:02x} error 0x{r[2]:02x} ext={r[3:].hex()}"
            return r[3:]

        async def write_data(data):
            for i in range(0, len(data), psize):
                await client.write_gatt_char(DFU_PACKET, data[i:i + psize], response=False)
                await asyncio.sleep(0.004)  # pace: CoreBluetooth drops WWR when its queue overflows

        await req(0x02, struct.pack("<H", 0))  # PRN=0

        # init packet
        await req(0x01, struct.pack("<BI", 1, len(dat)))
        await write_data(dat)
        off, crc = struct.unpack("<II", (await req(0x03))[:8])
        assert off == len(dat) and crc == binascii.crc32(dat), "CRC init"
        await req(0x04)
        print("init packet accepté (signature OK)")

        # firmware
        sel = await req(0x06, b"\x02")
        max_size = struct.unpack("<I", sel[:4])[0]
        print(f"max object size={max_size}")
        crc = 0
        t0 = time.time()
        for off0 in range(0, len(bin_), max_size):
            chunk = bin_[off0:off0 + max_size]
            expect_crc = binascii.crc32(chunk, crc)
            for retry in range(3):
                await req(0x01, struct.pack("<BI", 2, len(chunk)))
                await write_data(chunk)
                off, rcrc = struct.unpack("<II", (await req(0x03))[:8])
                if off == off0 + len(chunk) and rcrc == expect_crc:
                    break
                print(f"  retry chunk @{off0} (off={off}, essai {retry + 1})")
            else:
                raise AssertionError(f"CRC data @{off0}")
            crc = expect_crc
            await req(0x04)
            done = off0 + len(chunk)
            rate = done / (time.time() - t0) / 1024
            print(f"  {done}/{len(bin_)} B ({rate:.1f} KB/s)")
        print(f"transfert terminé en {time.time()-t0:.1f} s — le boîtier redémarre")
    return 0


sys.exit(asyncio.run(main()))
