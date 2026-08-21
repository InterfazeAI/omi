"""Pull an explicit byte range out of one segment and decode it to WAV.

    python3 pull_range.py <start> <length> [out.wav] [segment]

Offsets are within a segment, not across the whole ring; `segment` defaults to the one being
recorded into. Run info.py to see what is on the card. Useful for re-decoding a span you
already know about without recording again - audio stays until its segment is evicted.
"""
import asyncio
import os
import sys
import time

from bleak import BleakClient

import omi_sd


async def main():
    began = time.time()
    if len(sys.argv) < 3:
        print(__doc__)
        return
    start, length = int(sys.argv[1]), int(sys.argv[2])
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser("~/Desktop/omi_pull.wav")
    segment = int(sys.argv[4]) if len(sys.argv) > 4 else None

    device = await omi_sd.find_device()
    if not device:
        print("device not found - is it advertising? a failed SD mount stops BLE entirely")
        return

    async with BleakClient(device, timeout=25.0) as client:
        await asyncio.sleep(2.0)
        info = await omi_sd.read_info(client)
        if segment is None:
            segment = info.count
        elif segment < 1 or segment > info.count:
            print(f"  no segment {segment}; card holds 1..{info.count}")
            return
        aligned = start - (start % omi_sd.SD_BLE_SIZE)
        print(f"  {info}", flush=True)
        print(f"  pulling {length:,} from {aligned:,} of segment {segment}", flush=True)
        raw, elapsed = await omi_sd.download(client, start, length, segment=segment)

    omi_sd.save_and_report(raw, elapsed, out)
    print(f"  total {omi_sd.format_duration(time.time() - began)}")


asyncio.run(main())
