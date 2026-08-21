"""Pull an explicit byte range off the card and decode it to WAV.

    python3 pull_range.py <start> <length> [out.wav]

Useful for re-decoding a span you already know about without recording again - the audio
stays on the card until it is overwritten.
"""
import asyncio
import os
import sys

from bleak import BleakClient

import omi_sd


async def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    start, length = int(sys.argv[1]), int(sys.argv[2])
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser("~/Desktop/omi_pull.wav")

    device = await omi_sd.find_device()
    if not device:
        print("device not found - is it advertising? a failed SD mount stops BLE entirely")
        return

    async with BleakClient(device, timeout=25.0) as client:
        await asyncio.sleep(2.0)
        total, _ = await omi_sd.read_size(client)
        aligned = start - (start % omi_sd.SD_BLE_SIZE)
        print(f"  card {total:,}; pulling {length:,} from {aligned:,} (block-aligned)", flush=True)
        raw, elapsed = await omi_sd.download(client, start, length)

    omi_sd.save_and_report(raw, elapsed, out)


asyncio.run(main())
