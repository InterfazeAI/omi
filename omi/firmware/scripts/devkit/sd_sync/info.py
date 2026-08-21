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
        total, saved = await omi_sd.read_size(client)

    print(f"  recording   {total:,} bytes  (~{total/BYTES_PER_SECOND/60:.1f} min of audio)")
    print(f"  read offset {saved:,} bytes")
    print(f"  full sync   ~{total/1024/SYNC_KB_PER_SECOND/60:.1f} min at {SYNC_KB_PER_SECOND:.0f} KB/s\n")


asyncio.run(main())
