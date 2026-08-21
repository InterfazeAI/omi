"""Record for a fixed window, then pull back exactly that span and decode it.

    python3 record_and_pull.py [seconds] [out.wav]

Marks the current recording length, waits while you speak, then downloads only the bytes
added in between. Recording continues throughout - the card is not cleared.
"""
import asyncio
import os
import sys

from bleak import BleakClient

import omi_sd


async def main():
    window = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/Desktop/omi_recording.wav")

    device = await omi_sd.find_device()
    if not device:
        print("device not found - is it advertising? a failed SD mount stops BLE entirely")
        return

    async with BleakClient(device, timeout=25.0) as client:
        await asyncio.sleep(2.0)
        start, _ = await omi_sd.read_size(client)
        print(f"\n  mark at {start:,} bytes", flush=True)
        print(f"\n>>> SPEAK NOW - recording {window}s <<<\n", flush=True)
        for remaining in range(window, 0, -5):
            print(f"      {remaining:3d}s left", flush=True)
            await asyncio.sleep(5)

        end, _ = await omi_sd.read_size(client)
        span = end - start
        print(f"\n  recorded {span:,} bytes ({span/window:,.0f} B/s), downloading...", flush=True)
        raw, elapsed = await omi_sd.download(client, start, span)

    if omi_sd.save_and_report(raw, elapsed, out):
        print(f"  (asked for {window}s)")


asyncio.run(main())
