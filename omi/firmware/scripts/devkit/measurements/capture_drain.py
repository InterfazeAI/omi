"""Log the supply readings against a draining cell.

Settles one question: does VDDHDIV5 measure the battery? A gauge that is really watching the cell
must fall as the cell drains. One that sits still for hours while the board runs itself flat is
measuring something else, whatever its value looks like.

Run on battery, with USB out. Logs to out/drain.csv beside this script and prints as it goes.
"""
import asyncio
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
OUT.mkdir(exist_ok=True)
sys.path.insert(0, str(HERE.parent / "sd_sync"))

from bleak import BleakClient

import omi_sd

CSV = OUT / "drain.csv"
INTERVAL_S = 60

# Connect straight to the board rather than scanning for it. A peripheral stops advertising while
# it is connected, and because this one is bonded, macOS re-establishes the link on its own after
# any unclean exit -- so a scan-based reconnect waits for an advertisement that will never be sent
# while the previous link is still up. Addressing it directly attaches to the existing connection.
ADDRESS = "767AD1FF-EB82-B418-FB71-404DC1C245E4"


def decode(raw):
    """Pull the fields out directly: this runs against the probe firmware, not the trimmed one."""
    charging = bool(raw[21])
    vdd = struct.unpack("<i", raw[22:26])[0]
    vddh = struct.unpack("<i", raw[26:30])[0] if len(raw) >= 30 else None
    ain7 = struct.unpack("<i", raw[4:8])[0]
    return charging, vdd, vddh, ain7


async def sample_forever():
    began = time.time()
    with open(CSV, "w") as f:
        f.write("elapsed_min,charging,vdd_mv,vddh_mv,ain7_mv\n")

    while True:
        try:
            async with BleakClient(ADDRESS, timeout=30.0) as client:
                while True:
                    raw = await client.read_gatt_char(omi_sd.BATTERY_DIAG_CHAR)
                    charging, vdd, vddh, ain7 = decode(raw)
                    mins = (time.time() - began) / 60

                    with open(CSV, "a") as f:
                        f.write(f"{mins:.1f},{int(charging)},{vdd},{vddh},{ain7}\n")
                    print(f"  {time.strftime('%H:%M:%S')}  t+{mins:6.1f}min  "
                          f"vddh {vddh:6,} mV  vdd {vdd:6,} mV  charging {charging}", flush=True)

                    await asyncio.sleep(INTERVAL_S)
        except Exception as exc:
            # The board dying of a flat battery is the expected end of this test, so a dropped
            # link is a result rather than an error. Keep retrying so the last sample before it
            # went is preserved.
            print(f"  link lost ({type(exc).__name__}: {exc}); retrying", flush=True)
            await asyncio.sleep(30)


if __name__ == "__main__":
    try:
        asyncio.run(sample_forever())
    except KeyboardInterrupt:
        print("\n  stopped")
