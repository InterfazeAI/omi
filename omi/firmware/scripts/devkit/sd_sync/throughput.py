"""Measure sync throughput off the card.

    python3 throughput.py [seconds]

Healthy is ~14 KB/s. Around 0.1 KB/s means the sync thread is being starved rather than the
card being slow - see omi/firmware/devkit/DEBUGGING.md.
"""
import asyncio
import sys
import time

from bleak import BleakClient

import omi_sd


async def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 25

    device = await omi_sd.find_device()
    if not device:
        print("device not found - is it advertising? a failed SD mount stops BLE entirely")
        return

    async with BleakClient(device, timeout=25.0) as client:
        await asyncio.sleep(2.0)
        info = await omi_sd.read_info(client)
        # Measure against the segment being recorded, which is the contended case: the reader
        # and the recorder are then competing for the same file and the same mutex.
        segment = info.count
        start = max(0, info.newest_bytes - 300_000)
        got = [0]

        def on_notify(_, data):
            if len(bytes(data)) > 1:
                got[0] += len(bytes(data))

        await client.start_notify(omi_sd.CMD_CHAR, on_notify)
        print(f"  pulling from {start:,} of {info.newest_bytes:,} in segment {segment}", flush=True)
        await client.write_gatt_char(
            omi_sd.CMD_CHAR,
            bytes([omi_sd.READ_COMMAND, segment, (start >> 24) & 0xFF, (start >> 16) & 0xFF,
                   (start >> 8) & 0xFF, start & 0xFF]),
            response=True)

        began = time.time()
        for _ in range(seconds // 5):
            await asyncio.sleep(5)
            elapsed = time.time() - began
            print(f"    {elapsed:4.0f}s  {got[0]:8,} bytes  {got[0]/elapsed/1024:6.2f} KB/s",
                  flush=True)

        try:
            await client.write_gatt_char(
                omi_sd.CMD_CHAR, bytes([omi_sd.STOP_COMMAND, 1, 0, 0, 0, 0]), response=True)
            await client.stop_notify(omi_sd.CMD_CHAR)
        except Exception:
            pass


asyncio.run(main())
