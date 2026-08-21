"""Record for a fixed window, then pull back exactly that span and decode it.

    python3 record_and_pull.py [seconds] [out.wav]

Marks where the recording is, waits while you speak, then downloads only what was added.
Recording continues throughout and nothing is deleted. If the ring rotates to a new segment
during the window, both parts are fetched and joined.
"""
import asyncio
import os
import sys
import time

from bleak import BleakClient

import omi_sd


async def main():
    began = time.time()
    window = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/Desktop/omi_recording.wav")

    device = await omi_sd.find_device()
    if not device:
        print("device not found - is it advertising? a failed SD mount stops BLE entirely")
        return

    async with BleakClient(device, timeout=25.0) as client:
        await asyncio.sleep(2.0)
        start_info = await omi_sd.read_info(client)
        start_seq = start_info.newest_seq
        start_off = start_info.newest_bytes
        print(f"\n  {start_info}")
        print(f"  mark at {start_off:,} bytes of segment {start_info.count}", flush=True)
        print(f"\n>>> SPEAK NOW - recording {window}s <<<\n", flush=True)
        for remaining in range(window, 0, -5):
            print(f"      {remaining:3d}s left", flush=True)
            await asyncio.sleep(5)

        end_info = await omi_sd.read_info(client)
        if end_info.oldest_seq > start_seq:
            print("  the segment being recorded at the start has already been evicted")
            return

        # Segment numbers are positions in the ring, so an eviction during the window shifts
        # them down. Convert through the stable sequence number instead.
        start_num = end_info.count - (end_info.newest_seq - start_seq)
        raw = b""
        elapsed = 0.0

        if end_info.newest_seq == start_seq:
            span = end_info.newest_bytes - start_off
            print(f"\n  recorded {span:,} bytes ({span/window:,.0f} B/s), downloading...", flush=True)
            raw, elapsed = await omi_sd.download(client, start_off, span, segment=start_num)
        else:
            rotations = end_info.newest_seq - start_seq
            print(f"\n  ring rotated {rotations}x during the window; fetching each part", flush=True)
            for i in range(rotations + 1):
                num = start_num + i
                begin = start_off if i == 0 else 0
                if i == rotations:
                    length = end_info.newest_bytes - begin
                else:
                    length = end_info.segment_bytes - begin
                if length <= 0:
                    continue
                part, took = await omi_sd.download(client, begin, length, segment=num)
                raw += part
                elapsed += took

    if omi_sd.save_and_report(raw, elapsed, out):
        print(f"  (asked for {window}s)")
    print(f"  total {omi_sd.format_duration(time.time() - began)}")


asyncio.run(main())
