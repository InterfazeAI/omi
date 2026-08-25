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
import math
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

    # The reported percentage is built on the smoothed mean, not on the fresh read above. Showing
    # both is the point: their difference is the jitter the gauge used to publish directly.
    if diag.smoothed_mv:
        jitter = diag.jitter_mv
        print(f"  smoothed      {diag.smoothed_mv:,} mV   "
              f"(this read is {jitter:+,} mV off the mean)")
    else:
        print("  smoothed      not yet -- the guard's window fills ~80 s after boot")

    # Frozen at the first settled mean, so it stays a load measurement instead of drifting into
    # "how far has the cell discharged since boot". Only exists on a boot that ran on battery,
    # because charging skips the gate that takes the rested reading.
    sag = diag.load_sag_mv
    if sag is None:
        print("  load sag      not measured this boot (gate skipped: charging, or older firmware)")
    else:
        print(f"  load sag      {sag:,} mV   ({diag.boot_mv:,} mV at boot, rested)")

    errors = {"init": diag.init_err, "adc setup": diag.setup_err,
              "gpio": diag.gpio_err, "adc read": diag.read_err, "call": diag.call_err}
    reported = {name: code for name, code in errors.items() if code}
    print(f"  errors        {reported if reported else 'none'}")
    print(f"\n  >> {diag.verdict()}")


# How long a drain run tolerates silence before calling the board dead rather than unlucky. Long
# enough that a passing BLE glitch or a host radio hiccup has recovered, short enough not to waste
# the tail of an overnight run.
#
# Expressed in minutes and converted per run, because a fixed *count* means whatever the sampling
# interval happens to be. This was 6 samples with a comment claiming half an hour, which was true
# of no spacing the tool actually uses -- at the 0.5 min default it aborted after about three
# minutes of trouble and then printed "last reading before it went", turning a brief radio problem
# into a bogus measurement of when the battery died.
DEAD_AFTER_MIN = 30


async def watch(address, minutes, every, fine_below=None, fine_every=None):
    """Track the cell over time: charging it must rise, running it must fall.

    Opens a connection per sample instead of holding one. A link kept up for hours will drop --
    the board reboots, the host sleeps, the radio glitches -- and a drain test that dies at 3am
    has measured nothing. A failed sample is recorded and the run continues.

    `fine_below` tightens the interval once the cell falls under it. Li-ion is near-linear in the
    middle and then drops away quickly, so one spacing cannot serve both: wide enough to sit
    through a flat night is far too coarse across the knee, which is the part worth measuring.

    `fine_every` therefore defaults to half of `every` rather than to a constant. It used to
    default to 1.0 min against an `every` of 0.5, which doubled the spacing across the knee -- the
    one region the option exists to sample harder -- while announcing that it was tightening.
    """
    if fine_every is None:
        fine_every = every / 2
    if fine_below and fine_every >= every:
        print(f"  !! --fine-every {fine_every:g} min is not shorter than --every {every:g} min, "
              f"so the knee will be sampled more coarsely than the plateau", flush=True)

    print(f"\n  sampling every {every:g} min for {minutes:g} min"
          + (f", every {fine_every:g} min below {fine_below:,} mV" if fine_below else "")
          + "\n", flush=True)
    seen, deadline = [], time.time() + minutes * 60
    start, misses, interval = time.time(), 0, every
    # Whatever number of consecutive misses adds up to DEAD_AFTER_MIN at this spacing, floored so
    # a very wide interval still takes a few tries before declaring the board gone.
    dead_after = max(3, math.ceil(DEAD_AFTER_MIN / every))

    while time.time() < deadline:
        try:
            async with BleakClient(address, timeout=20.0) as client:
                diag = await omi_sd.read_battery_diag(client)
            seen.append((time.time() - start, diag.battery_mv))
            misses = 0
            print(f"  {time.strftime('%H:%M:%S')}  t+{(time.time()-start)/60:6.1f}min  "
                  f"{diag.battery_mv:,} mV  {diag.percentage:3}%  "
                  f"{'CHARGING' if diag.charging else 'on cell '}  "
                  f"(pin {diag.adc_mv:,} mV, vdd {diag.vdd_mv:,} mV)", flush=True)
            if fine_below and diag.battery_mv < fine_below and interval != fine_every:
                verb = "tightening" if fine_every < interval else "changing"
                interval = fine_every
                print(f"  -- under {fine_below:,} mV, {verb} to {fine_every:g} min", flush=True)
        except Exception as exc:
            misses += 1
            print(f"  {time.strftime('%H:%M:%S')}  unreachable ({misses}): "
                  f"{type(exc).__name__}", flush=True)
            if misses >= dead_after:
                print(f"\n  unreachable for {misses} straight samples "
                      f"(~{DEAD_AFTER_MIN:g} min) -- board is off", flush=True)
                break
        await asyncio.sleep(interval * 60)

    if not seen:
        print("\n  no samples -- the board was never reachable")
        return

    if misses >= dead_after:
        last_t, last_mv = seen[-1]
        print(f"  last reading before it went: {last_mv:,} mV at t+{last_t/3600:.1f} h")

    mv = [v for _, v in seen]
    hours = (seen[-1][0] - seen[0][0]) / 3600 or 1e-9
    change = mv[-1] - mv[0]   # signed as the cell sees it: discharging is negative
    print(f"\n  {len(seen)} samples over {hours:.1f} h: {mv[0]:,} -> {mv[-1]:,} mV "
          f"(range {min(mv):,}-{max(mv):,})")
    print(f"  net {change:+,} mV, {change/hours:+.0f} mV/h")
    if abs(change) < 20:
        print("  flat: either it is not measuring, or nothing was drawing from the cell")


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--watch", type=float, metavar="MIN",
                    help="sample for MIN minutes to see whether the reading tracks the cell")
    ap.add_argument("--every", type=float, default=0.5, metavar="MIN",
                    help="minutes between samples while watching (default 0.5)")
    ap.add_argument("--fine-below", type=int, metavar="MV",
                    help="tighten the sample interval once the cell drops below MV")
    ap.add_argument("--fine-every", type=float, default=None, metavar="MIN",
                    help="minutes between samples below --fine-below (default: half of --every)")
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

    # Outside the `async with`: watch() reconnects per sample, so this link must be closed first.
    if args.watch:
        await watch(device, args.watch, args.every, args.fine_below, args.fine_every)
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
