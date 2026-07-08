"""Look for the dropped-sample symptom: host_time vs seq cadence and ZC distribution.

Goal: confirm firmware is dropping ADS1292 samples (not just dropping prints), and
quantify the effective sample rate the EMG model actually sees.
"""
import csv
import statistics
from pathlib import Path
from collections import Counter

CSV_PATH = Path(__file__).with_name("ads1292_capture.csv")

rows = []
with CSV_PATH.open("r", encoding="utf-8") as f:
    reader = csv.DictReader(f)
    for r in reader:
        try:
            rows.append({
                "ts": float(r["host_time"]),
                "seq": int(r["seq"]),
                "emg": int(r["emg"]),
                "rms": int(r["rms"]),
                "zc": int(r["zc"]),
                "angle": int(r["angle_or_ch1"]),
            })
        except Exception:
            pass

print(f"rows: {len(rows)}")
print(f"duration: {rows[-1]['ts'] - rows[0]['ts']:.3f} s")
print(f"seq delta: {rows[-1]['seq'] - rows[0]['seq']}")

deltas = [rows[i]["ts"] - rows[i-1]["ts"] for i in range(1, len(rows)) if rows[i]["ts"] - rows[i-1]["ts"] > 0]
print(f"\n=== Host-time inter-print delta (only positive) ===")
if deltas:
    print(f"  N={len(deltas)}  mean={statistics.mean(deltas)*1000:.2f} ms  median={statistics.median(deltas)*1000:.2f} ms")
    print(f"  min={min(deltas)*1000:.2f} ms  max={max(deltas)*1000:.2f} ms")

zero_deltas = sum(1 for i in range(1, len(rows)) if rows[i]["ts"] == rows[i-1]["ts"])
print(f"  rows with same timestamp as previous: {zero_deltas} ({100*zero_deltas/len(rows):.1f}%)")

# Each print = 5 raw samples at 500 SPS = 10 ms apart if no drop.
# If host_time delta > 10 ms median (excl. zero-bunched rows), firmware is slower than expected.
nonzero_deltas = [d for d in deltas if d > 0.001]
print(f"\n=== Excluding < 1ms deltas (which are batched UART arrivals) ===")
if nonzero_deltas:
    print(f"  N={len(nonzero_deltas)}  mean={statistics.mean(nonzero_deltas)*1000:.2f} ms  median={statistics.median(nonzero_deltas)*1000:.2f} ms")
    p90 = sorted(nonzero_deltas)[int(len(nonzero_deltas)*0.9)]
    p99 = sorted(nonzero_deltas)[int(len(nonzero_deltas)*0.99)]
    print(f"  p90={p90*1000:.2f} ms  p99={p99*1000:.2f} ms")

# Group by 1-second buckets to see actual sample rate over time.
print("\n=== Effective print rate over time (per 5-sec window) ===")
buckets = {}
t0 = rows[0]["ts"]
for r in rows:
    b = int((r["ts"] - t0) // 5)
    buckets[b] = buckets.get(b, 0) + 1
rates = [v/5 for k, v in sorted(buckets.items())]
print(f"  N buckets: {len(rates)}, mean print rate: {statistics.mean(rates):.1f} Hz")
print(f"  min/max: {min(rates):.1f} / {max(rates):.1f} Hz")
print(f"  first few buckets: {rates[:10]}")
print(f"  last few:          {rates[-10:]}")

# ZC distribution -- a key indicator of whether the signal looks like real ~250 SPS data.
zcs = [r["zc"] for r in rows]
zc_counter = Counter(zcs)
print(f"\n=== ZC distribution (out of 64-sample window) ===")
for k in sorted(zc_counter.keys())[:20]:
    bar = "#" * (zc_counter[k] * 40 // max(zc_counter.values()))
    print(f"  zc={k:3d}: {zc_counter[k]:5d} {bar}")
print(f"  ...")
print(f"  zc range: {min(zcs)} - {max(zcs)}")
print(f"  median {statistics.median(zcs)}")

# How fast does the EMG raw signal change between consecutive prints?
# Compare avg |emg[i] - emg[i-1]|, both raw and after a DC tracker.
diffs = [abs(rows[i]["emg"] - rows[i-1]["emg"]) for i in range(1, len(rows))]
print(f"\n=== |delta-emg| between consecutive printed samples ===")
print(f"  mean={statistics.mean(diffs):.0f}  median={statistics.median(diffs):.0f}  max={max(diffs)}")
print(f"  (this is the raw ADC code difference, gain 6, ~2.42V/2^23 LSB = 5.77 nV/LSB ideal,")
print(f"   so 1e4 LSB = 57.7 uV which is typical EMG amplitude)")

# Detect "stuck" printed samples (repeated identical emg) -- another drop indicator.
stuck_runs = 0
cur_run = 1
max_run = 1
for i in range(1, len(rows)):
    if rows[i]["emg"] == rows[i-1]["emg"]:
        cur_run += 1
        max_run = max(max_run, cur_run)
    else:
        if cur_run >= 3:
            stuck_runs += 1
        cur_run = 1
print(f"\n=== 'stuck' identical EMG runs (>=3 same values) ===")
print(f"  number of runs: {stuck_runs},  longest run: {max_run}")
