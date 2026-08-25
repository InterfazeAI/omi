#!/usr/bin/env python3
"""Wait for a boot that ran the low-battery gate, then report the load sag.

The gate only samples when the charger is idle, so this stays quiet until the board is running on
the cell. boot_mv is taken before the radio, mic and card start; battery_mv is now, with all of
them running. The difference is the margin BATT_BOOT_MIN_MV has to cover.
"""
import asyncio
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "sd_sync"))
import omi_sd  # noqa: E402
from bleak import BleakClient  # noqa: E402

DEADLINE = time.time() + 420


async def main():
    addr = None
    while time.time() < DEADLINE:
        try:
            if addr is None:
                addr = await omi_sd.find_device()
            async with BleakClient(addr, timeout=20.0) as client:
                d = await omi_sd.read_battery_diag(client)

            state = "CHARGING" if d.charging else "on cell "
            if not d.boot_mv:
                print(f"  {time.strftime('%H:%M:%S')}  {state}  {d.battery_mv:,} mV  "
                      f"{d.percentage:3}%  -- gate has not run this boot", flush=True)
            else:
                sag = d.boot_mv - d.battery_mv
                print(f"\n  LOAD SAG {sag} mV", flush=True)
                print(f"    at boot, nothing running : {d.boot_mv:,} mV", flush=True)
                print(f"    now, everything running  : {d.battery_mv:,} mV  ({d.percentage}%)",
                      flush=True)
                print(f"    charger                  : {state.strip()}", flush=True)
                print(f"\n    gate threshold  3,470 mV", flush=True)
                print(f"    guard threshold 3,420 mV   margin 50 mV", flush=True)
                verdict = ("margin covers the sag" if sag < 50 else
                           "SAG EXCEEDS THE MARGIN -- gate threshold is too low")
                print(f"    >> {verdict}", flush=True)
                return
        except Exception as exc:
            print(f"  {time.strftime('%H:%M:%S')}  unreachable: {type(exc).__name__}", flush=True)
            addr = None
        await asyncio.sleep(6)

    print("\n  timed out waiting for a boot on battery", flush=True)


asyncio.run(main())
