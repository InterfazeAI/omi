#!/usr/bin/env python3
"""Stitch the three drain logs into one timeline and bound the cutoff voltage.

The load is constant (recording to SD + BLE advertising), so charge consumed is proportional to
elapsed time. That turns "what percent is this voltage" into "how much runtime is left", which is
measurable from the logs -- unlike the firmware's table, which is a generic cell profile.
"""
import datetime as dt
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOGS = HERE / "logs"
# Derived artifacts are reproducible from the logs beside them, so they are not committed.
OUT = HERE / "out"
OUT.mkdir(exist_ok=True)

LINE = re.compile(r"(\d\d):(\d\d):(\d\d)\s+t\+\s*([\d.]+)min\s+([\d,]+) mV\s+(\d+)%")

# (path, date of the FIRST sample). Runs are contiguous on battery; the gaps are logger downtime,
# not the board being off, so elapsed wall time is what counts.
RUNS = [
    (LOGS / "drain-2026-08-23-overnight.log", dt.date(2026, 8, 23)),
    (LOGS / "drain-2026-08-23-to-empty.log", dt.date(2026, 8, 23)),
    (LOGS / "drain-2026-08-24-to-empty.log", dt.date(2026, 8, 24)),
]

samples = []  # (datetime, mV, firmware_pct)
for path, d0 in RUNS:
    prev_min = None
    for line in open(path, errors="replace"):
        m = LINE.search(line)
        if not m:
            continue
        hh, mm, ss, tmin, mv, pct = m.groups()
        tmin = float(tmin)
        # t+ is monotonic within a run; use it to roll the date over midnight.
        day = d0 + dt.timedelta(days=0)
        base = dt.datetime.combine(d0, dt.time(int(hh), int(mm), int(ss)))
        if prev_min is not None and tmin < prev_min:
            continue
        prev_min = tmin
        samples.append([base, int(mv.replace(",", "")), int(pct), path, tmin])

# Roll dates: within a run, wall clock decreasing means midnight passed.
for path, _ in RUNS:
    run = [s for s in samples if s[3] == path]
    add = dt.timedelta(0)
    for i in range(1, len(run)):
        if run[i][0] + add < run[i - 1][0]:
            add += dt.timedelta(days=1)
        run[i][0] += add

samples.sort(key=lambda s: s[0])
t0 = samples[0][0]
for s in samples:
    s.append((s[0] - t0).total_seconds() / 3600.0)  # index 5: hours since start

print(f"samples           {len(samples)}")
print(f"first             {samples[0][0]}  {samples[0][1]:,} mV  fw {samples[0][2]}%")
print(f"last monitored    {samples[-1][0]}  {samples[-1][1]:,} mV  fw {samples[-1][2]}%")
monitored_h = samples[-1][5]
print(f"monitored span    {monitored_h:.1f} h")

DEATH_MIN = dt.datetime(2026, 8, 24, 13, 23)   # last sample
DEATH_MAX = dt.datetime(2026, 8, 24, 19, 53)   # user reported it flat
print(f"death window      {DEATH_MIN.strftime('%H:%M')}..{DEATH_MAX.strftime('%H:%M')} "
      f"({(DEATH_MAX-DEATH_MIN).total_seconds()/3600:.1f} h wide)")


def fit(lo_h, hi_h):
    """Least-squares mV/h over a window, to average out the +-40 mV single-sample jitter."""
    pts = [(s[5], s[1]) for s in samples if lo_h <= s[5] <= hi_h]
    n = len(pts)
    if n < 3:
        return None, 0
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    num = sum((x - mx) * (y - my) for x, y in pts)
    den = sum((x - mx) ** 2 for x in (p[0] for p in pts))
    return (num / den if den else 0), n


print("\n=== discharge rate by region ===")
regions = [("upper  4020-3810", 0, 8), ("plateau 3800-3500", 9, 33), ("knee   3490-3455", 34, 99)]
rates = {}
for label, lo, hi in regions:
    r, n = fit(lo, hi)
    rates[label] = r
    print(f"  {label:20s} {r:+7.1f} mV/h   (n={n})")

tail_rate = rates["knee   3490-3455"]
last_mv = samples[-1][1]

print("\n=== where could it have died? ===")
for h, name in [(0.0, "died immediately"), (3.25, "mid-window"), (6.5, "died at the far end")]:
    for mult, how in [(1.0, "same rate"), (2.0, "2x steeper"), (4.0, "4x steeper")]:
        v = last_mv + tail_rate * mult * h
        print(f"  +{h:4.1f} h {how:11s} -> {v:6.0f} mV")
    print()

print("=== firmware table says ===")
for mv, pct in [(3437, 10), (3346, 5), (3255, 2), (3164, 1), (3000, 0)]:
    dv = last_mv - mv
    hours = dv / abs(tail_rate) if tail_rate else float("inf")
    fits = "reachable" if hours <= 6.5 else f"NEEDS {hours:.0f} h > 6.5 h window"
    print(f"  {mv:,} mV = {pct:3d}%   {dv:4.0f} mV below last sample   {fits}")

out = {
    "samples": [{"h": round(s[5], 3), "mv": s[1], "fw": s[2]} for s in samples],
    "monitored_h": round(monitored_h, 2),
    "last_mv": last_mv,
    "rates": {k: round(v, 2) for k, v in rates.items()},
    "death_window_h": 6.5,
}
curve = OUT / "soc_curve.json"
json.dump(out, open(curve, "w"))
print(f"\nwrote {curve} ({len(samples)} samples)")
