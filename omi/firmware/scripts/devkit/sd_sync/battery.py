#!/usr/bin/env python3
"""Battery level, and when it is wrong, why.

    battery.py            read the level and the measurement behind it
    battery.py --watch 10 sample for 10 minutes, to see whether it tracks a charging cell

The standard Battery Service reports a single percentage, which is exactly as useful as it is
trustworthy. When the sense divider is not connected the ADC reads noise around zero, and the
firmware's discharge curve clamps that to 0% or 100% according to nothing but the sign of the
noise -- so a broken gauge looks like a flat battery, or a full one. This reads the inputs behind
the number instead: the raw ADC counts, the divider enable pin read back from the hardware, and
the driver's error codes.
"""
import argparse
import asyncio
import time

from bleak import BleakClient

import omi_sd


def show(diag, level):
    print(f"  reported      {level}%   (battery service)")
    print(f"  charger       {'CHARGING' if diag.charging else 'not charging'}   (P0.17 ~CHG)")
    print(f"  computed      {diag.battery_mv:,} mV -> {diag.percentage}%")
    print(f"  at the pin    {diag.adc_mv:,} mV   ({diag.raw_counts:,} raw counts)")
    print(f"  divider off   {diag.off_counts:,} raw counts")
    print(f"  control       {diag.vdd_mv:,} mV for the 3.3V rail   "
          f"({'ADC is sound' if diag.adc_trustworthy else 'ADC IS NOT MEASURING'})")
    print(f"  divider       P0.14 {'LOW - enabled' if diag.divider_enabled else 'HIGH - DISABLED'}"
          f"{'' if diag.enable_is_output else ', NOT AN OUTPUT'}")

    errors = {"init": diag.init_err, "adc setup": diag.setup_err,
              "gpio": diag.gpio_err, "adc read": diag.read_err, "call": diag.call_err}
    reported = {name: code for name, code in errors.items() if code}
    print(f"  errors        {reported if reported else 'none'}")
    print(f"\n  >> {diag.verdict()}")


async def watch(client, minutes):
    """A charging cell rises, so a working gauge must move. Anything else is not measuring."""
    print(f"\n  sampling for {minutes} min -- plug in USB so the cell is charging\n")
    seen, deadline = [], time.time() + minutes * 60
    while time.time() < deadline:
        diag = await omi_sd.read_battery_diag(client)
        seen.append(diag.battery_mv)
        print(f"  {time.strftime('%H:%M:%S')}  {diag.battery_mv:,} mV  {diag.percentage}%  "
              f"(pin {diag.adc_mv:,} mV)", flush=True)
        await asyncio.sleep(30)

    spread = max(seen) - min(seen)
    print(f"\n  moved {spread:,} mV over {minutes} min "
          f"({min(seen):,} to {max(seen):,})")
    if spread < 20:
        print("  a charging cell that does not move is not being measured")


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--watch", type=float, metavar="MIN",
                    help="sample for MIN minutes to see whether the reading tracks the cell")
    args = ap.parse_args()

    device = await omi_sd.find_device()
    if not device:
        print("device not advertising")
        return 1

    async with BleakClient(device, timeout=30.0) as client:
        level = (await client.read_gatt_char(omi_sd.BATTERY_LEVEL_CHAR))[0]
        try:
            diag = await omi_sd.read_battery_diag(client)
        except Exception as exc:
            print(f"  reported {level}%, but no diagnostic: {exc}")
            return 1
        show(diag, level)

        if args.watch:
            await watch(client, args.watch)
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
