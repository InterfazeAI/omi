"""Find the device and report what is on the card.

    python3 info.py

Prints the recording length and the saved read offset, and estimates how long the recording is
and how long a full sync would take at the measured ~15 KB/s.
"""
import asyncio

from bleak import BleakClient

import omi_sd

# Measured on a DevKit v2 at CELT 20 kbps; see devkit/DEBUGGING.md.
BYTES_PER_SECOND = 2391.0
SYNC_KB_PER_SECOND = 15.0


async def main():
    device = await omi_sd.find_device()
    if not device:
        print("device not found - is it advertising? a failed SD mount stops BLE entirely")
        return

    print(f"\n  {device.name or 'unnamed'}  [{device.address}]")
    async with BleakClient(device, timeout=25.0) as client:
        await asyncio.sleep(2.0)
        info = await omi_sd.read_info(client)

    total = info.total_bytes
    print(f"  segments    {info.count} on card, sequence {info.oldest_seq}..{info.newest_seq}")
    print(f"  segment     {info.segment_bytes:,} bytes "
          f"(~{info.segment_bytes/BYTES_PER_SECOND/60:.0f} min each)")
    print(f"  recording   segment {info.count}, {info.newest_bytes:,} bytes so far")
    print(f"  retained    ~{total:,} bytes (~{total/BYTES_PER_SECOND/3600:.1f} h of audio)")
    print(f"  read offset {info.saved_offset:,} bytes")
    if info.max_count:
        health = f"  ring        cap {info.max_count} segments, {info.evictions} evicted"
        if info.last_evict_err:
            health += f", LAST EVICTION FAILED errno {info.last_evict_err}"
        if info.count > info.max_count:
            health += f"  <-- OVER CAP by {info.count - info.max_count}"
        print(health)
        # fs_sync always fails on SD in this SDK (see DEBUGGING.md trap 1), so a nonzero count
        # is expected; a count that stays at 0 while recording would be the surprise.
        print(f"  sync errors {info.sync_errors:,} (expected nonzero, known SDK bug)")
    print(f"  full sync   ~{total/1024/SYNC_KB_PER_SECOND/60:.1f} min at {SYNC_KB_PER_SECOND:.0f} KB/s\n")


asyncio.run(main())
