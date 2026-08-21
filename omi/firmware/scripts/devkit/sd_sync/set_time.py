"""Set the recorder's clock, so index marks carry a date instead of only an uptime.

Until an app does this, every index record stores epoch 0 and audio can only be placed
relative to the recording itself. Reads the clock back and checks the device did not reboot,
because writing the time is the one command that makes the firmware touch the card from a
Bluetooth callback — the shape of bug described in DEBUGGING.md trap 7.

    python3 set_time.py
"""
import asyncio
import struct
import sys
import time

from bleak import BleakClient

import omi_sd

TIME_WRITE_CHAR = "19b10031-e8f2-537e-4f6c-d104768a1214"
TIME_READ_CHAR = "19b10032-e8f2-537e-4f6c-d104768a1214"


async def run():
    device = await omi_sd.find_device()
    if not device:
        print("device not found - is it advertising? a failed SD mount stops BLE entirely")
        return 1

    async with BleakClient(device, timeout=30.0) as client:
        before = await omi_sd.read_info(client)

        epoch = int(time.time())
        await client.write_gatt_char(TIME_WRITE_CHAR, struct.pack("<I", epoch), response=True)
        print(f"  set clock to {epoch} ({time.strftime('%H:%M:%S')})")

        # The record is written by the storage thread, not in the write handler, so give it a
        # moment before judging whether anything survived.
        await asyncio.sleep(2.0)

        read_back = struct.unpack("<I", await client.read_gatt_char(TIME_READ_CHAR))[0]
        after = await omi_sd.read_info(client)

        print(f"  device reports {read_back} (drift {read_back - epoch:+d}s)")
        print(f"  {after}")

        if read_back == 0:
            print("  FAILED: clock still unset")
            return 1
        # Every counter restarts at zero on boot, so a drop means the device reset under us.
        if after.sync_errors < before.sync_errors:
            print(f"  FAILED: device rebooted (sync errors {before.sync_errors} -> "
                  f"{after.sync_errors})")
            return 1

        print("  clock set, device still up")
        return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(run()))
