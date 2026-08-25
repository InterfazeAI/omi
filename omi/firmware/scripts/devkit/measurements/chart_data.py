#!/usr/bin/env python3
"""Turn the stitched drain samples into chart-ready series and a voltage->SoC mapping.

State of charge is derived from *time*, not from the cell chemistry: the load is constant, so the
fraction of runtime remaining is the only honest percentage available. Death time is only known to
within a 6.5 h window, so every derived percentage is reported as a range, not a point value.
"""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parent / "out"

d = json.load(open(OUT / "soc_curve.json"))
s = d["samples"]
MON_H = d["monitored_h"]          # 36.8 h of logging
LAST_MV = d["last_mv"]            # 3455 mV at the last sample
TAIL_RATE = abs(d["rates"]["knee   3490-3455"])  # 18.9 mV/h, already 2x the plateau

# Death is bounded, not known: the logger stopped at 13:23 and the cell was reported flat at 19:53.
D_MIN, D_MID, D_MAX = 0.0, 3.25, 6.5
TOTALS = {"min": MON_H + D_MIN, "mid": MON_H + D_MID, "max": MON_H + D_MAX}


def smooth(vals, w=15):
    out = []
    for i in range(len(vals)):
        lo, hi = max(0, i - w // 2), min(len(vals), i + w // 2 + 1)
        win = sorted(vals[lo:hi])
        out.append(win[len(win) // 2])          # median kills the +-40 mV outliers
    return out


mv_s = smooth([p["mv"] for p in s])

# Downsample to a readable number of points, keeping the last sample.
N = 68
step = max(1, len(s) // N)
idx = list(range(0, len(s), step))
if idx[-1] != len(s) - 1:
    idx.append(len(s) - 1)

curve = [{"h": round(s[i]["h"], 2), "mv": mv_s[i], "fw": s[i]["fw"]} for i in idx]

# First time the smoothed trace reaches each firmware table voltage.
FW_TABLE = [(4074, 100), (4029, 95), (3983, 90), (3938, 85), (3893, 80), (3847, 70),
            (3802, 60), (3756, 50), (3665, 40), (3619, 30), (3528, 20), (3437, 10),
            (3346, 5), (3255, 2), (3164, 1), (3000, 0)]

rows = []
for mv, fw_pct in FW_TABLE:
    hit_h = next((s[i]["h"] for i in range(len(s)) if mv_s[i] <= mv), None)
    row = {"mv": mv, "fw": fw_pct}
    if hit_h is not None:
        row["h"] = round(hit_h, 1)
        for k, tot in TOTALS.items():
            row[k] = round(max(0.0, (tot - hit_h) / tot * 100), 1)
        row["status"] = "measured"
    else:
        # Never reached while logging. How long past the last sample would it need?
        need_h = (LAST_MV - mv) / TAIL_RATE
        row["need_h"] = round(need_h, 1)
        row["status"] = "reachable" if need_h <= D_MAX else "unreachable"
    rows.append(row)

out = {
    "curve": curve,
    "rows": rows,
    "monitored_h": MON_H,
    "last_mv": LAST_MV,
    "tail_rate": round(TAIL_RATE, 1),
    "totals": {k: round(v, 1) for k, v in TOTALS.items()},
    "rates": d["rates"],
    "n_samples": len(s),
}
json.dump(out, open(OUT / "soc_chart.json", "w"), indent=1)

print(f"curve points {len(curve)}   from {curve[0]['mv']} mV to {curve[-1]['mv']} mV")
print(f"totals (h)   {out['totals']}")
print(f"\n{'mV':>6} {'fw%':>4} {'hit@h':>7} {'min%':>6} {'mid%':>6} {'max%':>6}  status")
for r in rows:
    if r["status"] == "measured":
        print(f"{r['mv']:>6} {r['fw']:>4} {r['h']:>7} {r['min']:>6} {r['mid']:>6} {r['max']:>6}  measured")
    else:
        print(f"{r['mv']:>6} {r['fw']:>4} {'-':>7} {'-':>6} {'-':>6} {'-':>6}  "
              f"{r['status']} (needs {r['need_h']} h past last sample)")
