#!/usr/bin/env python3
"""Both drain logs on one axis, to show the plateau."""
import re
from datetime import datetime, timedelta
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOGS = HERE / "logs"
OUT = HERE / "out"
OUT.mkdir(exist_ok=True)

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SAMPLE = re.compile(r"(\d\d:\d\d:\d\d)\s+t\+\s*([\d.]+)min\s+([\d,]+) mV\s+(\d+)%")


def load(path, day):
    out, prev = [], None
    for line in open(path):
        m = SAMPLE.search(line)
        if not m:
            continue
        t = datetime.strptime(m.group(1), "%H:%M:%S").replace(year=2026, month=8, day=day)
        if prev and t < prev:
            t += timedelta(days=1)
        prev = t
        out.append((t, int(m.group(3).replace(",", ""))))
    return out


pts = (load(LOGS / "drain-2026-08-23-overnight.log", 23)
       + load(LOGS / "drain-2026-08-23-to-empty.log", 23)
       + load(LOGS / "drain-2026-08-24-to-empty.log", 24))
t0 = pts[0][0]
hrs = [(t - t0).total_seconds() / 3600 for t, _ in pts]
mv = [v for _, v in pts]

win = 11
smooth = [sum(mv[max(0, i - win + 1):i + 1]) / len(mv[max(0, i - win + 1):i + 1])
          for i in range(len(mv))]

fig, ax = plt.subplots(figsize=(11, 6))
ax.scatter(hrs, mv, s=6, color="#1a1a1a", alpha=.22, label="raw samples (±40 mV jitter)")
ax.plot(hrs, smooth, color="#0b6", lw=2.6, label="11-sample mean")

ax.axhline(3600, color="#e8a", ls="--", lw=1.4)
ax.text(0.2, 3607, "3,600 mV — the 'low' I first proposed. Reached at 24 h with 29% left.",
        color="#c67", fontsize=9)
ax.axhline(3400, color="#e55", ls="--", lw=1.4)
ax.text(0.2, 3407, "3,400 mV — regulator dropout", color="#c33", fontsize=9)

ax.annotate("steep upper region\n−23 mV/h", xy=(4, 3930), xytext=(1.5, 3700),
            arrowprops=dict(arrowstyle="->", color="#555"), fontsize=9, color="#333")
ax.annotate("plateau, 14–24 h\nas little as −3 mV/h", xy=(20, 3618), xytext=(11, 3810),
            arrowprops=dict(arrowstyle="->", color="#555"), fontsize=9, color="#333")
ax.annotate("steepening again: −12 mV/h\nover the last 2 h — leaving\nthe plateau",
            xy=(hrs[-1] - 0.6, smooth[-1]), xytext=(24.5, 3730),
            arrowprops=dict(arrowstyle="->", color="#a33"), fontsize=9, color="#a33")

ax.set_xlabel("hours on battery (continuous recording, BLE up, no USB)")
ax.set_ylabel("cell voltage (mV)")
ax.set_title(f"Omi DevKit 2 — {hrs[-1]:.1f} h on one charge so far, "
             f"{mv[0]:,} → {smooth[-1]:,.0f} mV", fontsize=12)
ax.legend(loc="upper right", fontsize=9)
ax.grid(alpha=.2)
ax.set_ylim(3350, 4080)
ax.set_xlim(-0.5, max(hrs) + 0.5)

plt.tight_layout()
png = OUT / "drain_combined.png"
plt.savefig(png, dpi=130)
print(f"{len(mv)} samples over {hrs[-1]:.1f} h -> {png}")
