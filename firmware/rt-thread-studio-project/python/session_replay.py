"""Replay a captured FIT monitor LIVE log against the trained RF action
classifier. The firmware emits LIVE rows at ~20 Hz with per-128ms-window
EMG features + raw IMU samples; this script slides a 3-second window
across the log, aggregates features to match the training schema, scales
them into the same physical units as KneE-PAD, and asks the model what
exercise was being performed.

Input:  a text file containing the STM32 console output (whatever you
captured with PuTTY / screen / Tera Term, with the new LIVE row format
that includes ax/ay/az/gx/gy/gz at the end).

Output: predictions every WINDOW_HOP_S seconds, plus an aggregate summary.

Usage:
    python python/session_replay.py path/to/serial_log.txt
    python python/session_replay.py path/to/serial_log.txt --window 3 --hop 1
"""
from __future__ import annotations

import argparse
import statistics
from collections import Counter
from pathlib import Path

import numpy as np
import pandas as pd
from joblib import load

# --- Hardware scale factors ----------------------------------------------
# ADS1292R: VREF 2.42 V, gain 6, 24-bit signed (firmware default config)
ADS_LSB_V = (2.42 * 2.0) / (6.0 * (1 << 24))    # ~48 nV per LSB

# MPU6050 (firmware default: ±2g accel, ±250 dps gyro)
ACC_LSB_G   = 1.0 / 16384.0
GYRO_LSB_DPS = 1.0 / 131.0

# --- Defaults --------------------------------------------------------------
LIVE_HZ_NOMINAL = 20.0   # firmware print rate (PRINT_THREAD_HZ in main.c)
WINDOW_S = 3.0
HOP_S    = 1.0

MODEL_PATH = Path(__file__).resolve().parent / "kneepad_action_rf.joblib"

FEATURE_COLS = [
    "mav", "rms", "wl", "iemg", "var", "zc", "ssc",
    "swing", "tilt_std", "tilt_p95", "tilt_zc",
    "gyro_mean", "gyro_max", "gyro_std", "acc_std",
]


def parse_live_rows(path: Path):
    """Yield dicts of LIVE columns. Tolerates surrounding noise / banner /
    CAL / REP lines."""
    rows = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("LIVE,"):
                continue
            parts = line.split(",")
            # New format: 22 fields including the leading 'LIVE'
            if len(parts) < 22:
                continue
            try:
                # Accept floats and trim trailing whitespace -- some serial
                # captures add CRLF artifacts.
                def i(x): return int(float(x))
                rows.append(dict(
                    t_ms      = i(parts[1]),
                    ac        = i(parts[2]),
                    rms       = i(parts[3]),
                    mav       = i(parts[4]),
                    wl        = i(parts[5]),
                    zc        = i(parts[6]),
                    ssc       = i(parts[7]),
                    zcr_x1k   = i(parts[8]),
                    fatigue   = i(parts[9]),
                    alert     = i(parts[10]),
                    emg_phase = parts[11].strip(),
                    emg_status= parts[12].strip(),
                    mot_phase = parts[13].strip(),
                    angle_x10 = i(parts[14]),
                    active    = i(parts[15]),
                    ax        = i(parts[16]),
                    ay        = i(parts[17]),
                    az        = i(parts[18]),
                    gx        = i(parts[19]),
                    gy        = i(parts[20]),
                    gz        = i(parts[21]),
                ))
            except Exception:
                continue
    return rows


def aggregate_window(rows, window_s):
    """Aggregate LIVE rows in a window into the model's feature schema.
    Units match the KneE-PAD training data (EMG in volts, accel in g, gyro
    in dps).
    """
    if not rows:
        return None

    # EMG window-level features come straight from LIVE (each LIVE row is a
    # 128 ms aggregate). Note adjacent rows can repeat the same window state
    # because the print rate (20 Hz) > window-eval rate (~7.8 Hz). For robust
    # aggregation we take statistics across the rows regardless of duplicates.
    mav_arr = np.array([r["mav"] for r in rows], dtype=np.float64) * ADS_LSB_V
    rms_arr = np.array([r["rms"] for r in rows], dtype=np.float64) * ADS_LSB_V
    # wl: per-256-sample window sum-of-diffs in ADC counts. Each sample
    # appears in ~4 hop windows (window/hop ratio), so a naive sum overcounts.
    wl_arr  = np.array([r["wl"]  for r in rows], dtype=np.float64) * ADS_LSB_V
    zc_arr  = np.array([r["zc"]  for r in rows], dtype=np.float64)
    ssc_arr = np.array([r["ssc"] for r in rows], dtype=np.float64)

    # Aggregate to "feature over the whole window" matching training schema.
    # Training computed these on raw 3 s of filt[n]; we approximate by
    # treating each LIVE row's mav/rms as representative of its 128 ms
    # contribution and averaging.
    mav  = float(mav_arr.mean())
    rms  = float(np.sqrt(np.mean(rms_arr ** 2)))
    # wl_3s: each LIVE.wl counts 4x as many sample-pairs as it should once
    # we average. We approximate the per-unique-sample WL by averaging then
    # multiplying by the expected number of unique samples in the window.
    expected_n_samples = int(round(window_s * 500.0))   # firmware EMG fs
    wl  = float(wl_arr.mean()) * (expected_n_samples / 256.0)
    iemg = float(mav * expected_n_samples)
    var  = float(rms_arr.mean() ** 2 - mav_arr.mean() ** 2)
    # zc / ssc: similar correction for the 4x overlap.
    zc  = float(zc_arr.mean() * (expected_n_samples / 256.0))
    ssc = float(ssc_arr.mean() * (expected_n_samples / 256.0))

    # IMU features (raw int16 -> physical units).
    angle_deg = np.array([r["angle_x10"] for r in rows], dtype=np.float64) / 10.0
    angle_rel = angle_deg - angle_deg[0]
    swing    = float(angle_rel.max() - angle_rel.min())
    tilt_std = float(angle_rel.std())
    tilt_p95 = float(np.percentile(np.abs(angle_rel), 95))

    sgn = np.sign(angle_rel - angle_rel.mean())
    tilt_zc = int(np.sum(np.abs(np.diff(sgn)) > 0))

    ax = np.array([r["ax"] for r in rows], dtype=np.float64) * ACC_LSB_G
    ay = np.array([r["ay"] for r in rows], dtype=np.float64) * ACC_LSB_G
    az = np.array([r["az"] for r in rows], dtype=np.float64) * ACC_LSB_G
    gx = np.array([r["gx"] for r in rows], dtype=np.float64) * GYRO_LSB_DPS
    gy = np.array([r["gy"] for r in rows], dtype=np.float64) * GYRO_LSB_DPS
    gz = np.array([r["gz"] for r in rows], dtype=np.float64) * GYRO_LSB_DPS

    gyro_mag = np.sqrt(gx * gx + gy * gy + gz * gz)
    acc_mag  = np.sqrt(ax * ax + ay * ay + az * az)

    return dict(
        mav=mav, rms=rms, wl=wl, iemg=iemg, var=var, zc=zc, ssc=ssc,
        swing=swing, tilt_std=tilt_std, tilt_p95=tilt_p95, tilt_zc=tilt_zc,
        gyro_mean=float(gyro_mag.mean()),
        gyro_max =float(gyro_mag.max()),
        gyro_std =float(gyro_mag.std()),
        acc_std  =float(acc_mag.std()),
    )


def quick_heuristic(feat):
    """Sanity-check ruleset that does *not* use the model. Catches obvious
    cases when the model misfires due to scale mismatch."""
    if feat["gyro_max"] > 80 and feat["acc_std"] > 0.15:
        return "Walking", "high gyro_max + acc_std"
    if feat["swing"] > 25:
        return "Squat", f"swing={feat['swing']:.1f} deg"
    if feat["swing"] < 8 and feat["mav"] > 0:
        return "LegExt", f"low swing={feat['swing']:.1f}, seated knee work"
    return "?", "no clear rule"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", help="STM32 serial-output log file")
    ap.add_argument("--model", default=str(MODEL_PATH))
    ap.add_argument("--window", type=float, default=WINDOW_S)
    ap.add_argument("--hop",    type=float, default=HOP_S)
    ap.add_argument("--show-features", action="store_true",
                    help="Print full feature vector for each window")
    args = ap.parse_args()

    rows = parse_live_rows(Path(args.log))
    if not rows:
        raise SystemExit(f"no LIVE rows parsed from {args.log}")

    # Estimate effective LIVE rate from timestamps for windowing.
    dur_s = (rows[-1]["t_ms"] - rows[0]["t_ms"]) / 1000.0
    rate  = (len(rows) - 1) / max(dur_s, 1e-6)
    print(f"parsed {len(rows)} LIVE rows; duration {dur_s:.1f} s; "
          f"effective rate {rate:.1f} Hz")
    print(f"phases seen: {dict(Counter(r['emg_phase'] for r in rows))}")
    print(f"motion phases seen: {dict(Counter(r['mot_phase'] for r in rows))}")
    print()

    rows_per_window = max(2, int(round(args.window * rate)))
    rows_per_hop    = max(1, int(round(args.hop    * rate)))

    print(f"sliding window: {args.window}s ({rows_per_window} rows), "
          f"hop {args.hop}s ({rows_per_hop} rows)")
    print()

    model = load(args.model)

    summary = Counter()
    print(f"{'t_start_s':>9}  {'predicted':>10}  {'prob_top':>8}  "
          f"{'heuristic':>10}  {'rule':<40}")
    print("-" * 90)
    n_windows = 0
    for start in range(0, len(rows) - rows_per_window + 1, rows_per_hop):
        win = rows[start:start + rows_per_window]
        # Skip windows that contain calibration data -- the model was trained
        # only on RUNNING-phase data.
        if any(r["emg_phase"] != "RUN" for r in win):
            continue

        feat = aggregate_window(win, args.window)
        if feat is None:
            continue

        df = pd.DataFrame([{k: feat[k] for k in FEATURE_COLS}])
        pred = model.predict(df)[0]
        proba = dict(zip(model.classes_, model.predict_proba(df)[0]))
        top_prob = max(proba.values())

        h_label, h_reason = quick_heuristic(feat)

        t_start = win[0]["t_ms"] / 1000.0
        print(f"{t_start:9.1f}  {pred:>10}  {top_prob:8.2f}  "
              f"{h_label:>10}  {h_reason:<40}")

        if args.show_features:
            print("           features:", {k: f"{feat[k]:.3g}" for k in FEATURE_COLS})

        summary[pred] += 1
        n_windows += 1

    print()
    print("=" * 70)
    print(f"total windows: {n_windows}")
    print("prediction summary:")
    for cls, cnt in summary.most_common():
        bar = "#" * (cnt * 40 // max(summary.values()))
        print(f"  {cls:<10} {cnt:>4} ({100*cnt/max(n_windows,1):.1f}%)  {bar}")


if __name__ == "__main__":
    main()
