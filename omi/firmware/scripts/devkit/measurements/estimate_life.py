#!/usr/bin/env python3
"""Estimate remaining runtime from the two drain logs.

Single samples jitter ~40 mV, so every rate here comes from a least-squares fit over a window,
never from differencing two readings.
"""
import re
from datetime import datetime, timedelta
from pathlib import Path

LOGS = Path(__file__).resolve().parent / "logs"

SAMPLE = re.compile(r"(\d\d:\d\d:\d\d)\s+t\+\s*([\d.]+)min\s+([\d,]+) mV\s+(\d+)%")


def load(path, day):
    out = []
    prev = None
    for line in open(path):
        m = SAMPLE.search(line)
        if not m:
            continue
        t = datetime.strptime(m.group(1), "%H:%M:%S").replace(year=2026, month=8, day=day)
        if prev and t < prev:          # crossed midnight
            t += timedelta(days=1)
        prev = t
        out.append((t, int(m.group(3).replace(",", "")), int(m.group(4))))
    return out


def fit(points):
    """Least-squares mV/hour over the given (time, mV) points."""
    if len(points) < 3:
        return 0.0
    t0 = points[0][0]
    xs = [(t - t0).total_seconds() / 3600 for t, _, _ in points]
    ys = [mv for _, mv, _ in points]
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0


a = load(LOGS / "drain-2026-08-23-overnight.log", 23)
b = load(LOGS / "drain-2026-08-23-to-empty.log", 23)
c = load(LOGS / "drain-2026-08-24-to-empty.log", 24)
all_pts = a + b + c

t_start, t_now = all_pts[0][0], all_pts[-1][0]
elapsed = (t_now - t_start).total_seconds() / 3600

# Smooth the tail so the "current" voltage is not one noisy sample.
recent_mv = sum(mv for _, mv, _ in all_pts[-15:]) / len(all_pts[-15:])
recent_pct = sum(p for _, _, p in all_pts[-15:]) / len(all_pts[-15:])

print(f"on battery for {elapsed:.1f} h   {all_pts[0][1]:,} -> {recent_mv:,.0f} mV "
      f"(smoothed), {recent_pct:.0f}%")
print(f"overall            {fit(all_pts):+6.1f} mV/h")
print(f"first 8 h (upper)  {fit(a):+6.1f} mV/h")
print(f"after the gap      {fit(b + c):+6.1f} mV/h")
for hrs in (8, 4, 2):
    win = [p for p in all_pts if (t_now - p[0]).total_seconds() / 3600 <= hrs]
    print(f"last {hrs:>2} h           {fit(win):+6.1f} mV/h   ({len(win)} samples)")

rate = -fit([p for p in all_pts if (t_now - p[0]).total_seconds() / 3600 <= 6])
print(f"\nprojection at the current {rate:.1f} mV/h, IF it stayed linear:")
for target, what in ((3500, "knee, cell getting low"),
                     (3400, "regulator dropout ~3.4 V"),
                     (3000, "firmware 0%")):
    print(f"  to {target:,} mV   {(recent_mv - target)/rate:5.1f} h more   ({what})")

pct_rate = (all_pts[0][2] - recent_pct) / elapsed
print(f"\nby the firmware's own percentage: {pct_rate:.2f} pts/h, "
      f"{recent_pct/pct_rate:.1f} h left, {elapsed + recent_pct/pct_rate:.0f} h total")
