"""Slice a monitor.py log file into labeled feature windows.

Reads a session_*.log file produced by monitor.py, applies the user's
keyboard MARK rows to assign action labels to each LIVE timestamp, then
slides a 3-second window across the LIVE stream and computes per-window
features matching the schema used by the KneE-PAD action classifier.

Output:
  D:\\mpu_wave\\<session>_features.csv  (one row per window)

Usage:
    python auto_label.py D:\\mpu_wave\\session_20260525_120000.log
    python auto_label.py session_20260525_120000.log --window 3 --hop 1
    python auto_label.py session_20260525_120000.log --keep-rest

The --keep-rest flag keeps Rest windows in the output (default drops them
since they are not useful for action classification training).
"""
from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path

import numpy as np

# ADS1292R config in firmware: gain 6, VREF 2.42 V
ADS_LSB_V    = 2.0 * 2.42 / (6.0 * (1 << 24))
ACC_LSB_G    = 1.0 / 16384.0
GYRO_LSB_DPS = 1.0 / 131.0

# Default analysis window matches train_action_classifier.py (~3 s, hop 1 s).
DEFAULT_WINDOW_S = 3.0
DEFAULT_HOP_S    = 1.0

# Features to emit per window. Names match train_action_classifier's
# FEATURE_COLS list so the produced CSV can be concatenated with KneE-PAD's
# kneepad_action_features.csv for joint training.
FEATURE_COLS = [
    "mav", "rms", "wl", "iemg", "var", "zc", "ssc",
    "swing", "tilt_std", "tilt_p95", "tilt_zc",
    "gyro_mean", "gyro_max", "gyro_std", "acc_std",
]


def parse_log(path: Path):
    """Return (live_rows, marks, meta) from a monitor.py log."""
    live_rows = []
    marks = []
    meta = {}
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line:
                continue
            if line.startswith("#"):
                continue
            if line.startswith("LIVE,"):
                p = line.split(",")
                if len(p) < 22:
                    continue
                try:
                    i = lambda x: int(float(x))
                    live_rows.append(dict(
                        t_ms=i(p[1]), ac=i(p[2]), rms=i(p[3]), mav=i(p[4]),
                        wl=i(p[5]), zc=i(p[6]), ssc=i(p[7]),
                        emg_phase=p[11].strip(),
                        emg_status=p[12].strip(),
                        mot_phase=p[13].strip(),
                        angle_x10=i(p[14]),
                        ax=i(p[16]), ay=i(p[17]), az=i(p[18]),
                        gx=i(p[19]), gy=i(p[20]), gz=i(p[21]),
                    ))
                except Exception:
                    continue
            elif line.startswith("MARK,"):
                p = line.split(",")
                if len(p) >= 3:
                    try:
                        marks.append((int(float(p[1])), p[2].strip()))
                    except Exception:
                        pass
            elif line.startswith("CAL,"):
                # First REST_READY tells us calibration is done; useful as
                # a "skip everything before this" anchor.
                if "REST_READY" in line and "first_cal_t_ms" not in meta:
                    if live_rows:
                        meta["first_cal_t_ms"] = live_rows[-1]["t_ms"]
    return live_rows, marks, meta


def label_at(t_ms: int, marks):
    """Find the active label at time t_ms by walking the mark list (sorted
    ascending). The label set by mark[i] applies from t_i (inclusive) up to
    t_{i+1} (exclusive). Before the first mark, label is 'Rest'."""
    label = "Rest"
    for t, lab in marks:
        if t <= t_ms:
            label = lab
        else:
            break
    return label


def window_features(rows):
    """Compute the 15-feature vector for a list of LIVE rows in a window."""
    if not rows:
        return None

    # EMG features: each LIVE row already carries 128 ms aggregate values
    # (mav/rms/wl/zc/ssc) computed by the firmware. We aggregate them.
    mav_arr = np.array([r["mav"] for r in rows], dtype=np.float64) * ADS_LSB_V
    rms_arr = np.array([r["rms"] for r in rows], dtype=np.float64) * ADS_LSB_V
    wl_arr  = np.array([r["wl"]  for r in rows], dtype=np.float64) * ADS_LSB_V
    zc_arr  = np.array([r["zc"]  for r in rows], dtype=np.float64)
    ssc_arr = np.array([r["ssc"] for r in rows], dtype=np.float64)

    # Approximate "feature over the whole 3 s of filt[n]" matching training.
    # See session_replay.py for derivation: each per-window value applies to
    # 256 samples, but adjacent windows overlap (window 256 / hop 64 = 4x).
    # We average across all rows then scale by samples-in-this-window / 256.
    duration_s = (rows[-1]["t_ms"] - rows[0]["t_ms"]) / 1000.0
    if duration_s <= 0:
        duration_s = len(rows) / 20.0  # fall back to nominal 20 Hz print rate
    expected_samples = int(round(duration_s * 500.0))
    scale = expected_samples / 256.0

    mav  = float(mav_arr.mean())
    rms  = float(np.sqrt(np.mean(rms_arr ** 2)))
    wl   = float(wl_arr.mean()  * scale)
    iemg = float(mav * expected_samples)
    var  = float(rms_arr.mean() ** 2 - mav_arr.mean() ** 2)
    zc   = float(zc_arr.mean()  * scale)
    ssc  = float(ssc_arr.mean() * scale)

    # IMU features.
    angle = np.array([r["angle_x10"] for r in rows], dtype=np.float64) / 10.0
    angle_rel = angle - angle[0]
    swing    = float(angle_rel.max() - angle_rel.min())
    tilt_std = float(angle_rel.std())
    tilt_p95 = float(np.percentile(np.abs(angle_rel), 95))
    sgn = np.sign(angle_rel - angle_rel.mean())
    tilt_zc = int((np.abs(np.diff(sgn)) > 0).sum())

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", help="path to a monitor.py session log file")
    ap.add_argument("--window", type=float, default=DEFAULT_WINDOW_S,
                    help="window length in seconds")
    ap.add_argument("--hop", type=float, default=DEFAULT_HOP_S,
                    help="hop in seconds")
    ap.add_argument("--keep-rest", action="store_true",
                    help="include Rest-labeled windows in the output")
    ap.add_argument("--out", default="",
                    help="output CSV path (default: <log>_features.csv)")
    ap.add_argument("--require-running", action="store_true", default=True,
                    help="only include windows fully in RUN phase (default on)")
    ap.add_argument("--no-require-running", dest="require_running",
                    action="store_false")
    args = ap.parse_args()

    log_path = Path(args.log)
    if not log_path.exists():
        print(f"ERROR: {log_path} not found", file=sys.stderr)
        sys.exit(1)

    out_path = Path(args.out) if args.out else \
        log_path.with_name(log_path.stem + "_features.csv")

    rows, marks, meta = parse_log(log_path)
    if not rows:
        print(f"no LIVE rows parsed from {log_path}", file=sys.stderr)
        sys.exit(1)

    # Sort marks by time.
    marks.sort(key=lambda x: x[0])

    print(f"parsed {len(rows)} LIVE rows, {len(marks)} marks")
    print(f"first/last LIVE t_ms: {rows[0]['t_ms']} -> {rows[-1]['t_ms']} "
          f"({(rows[-1]['t_ms']-rows[0]['t_ms'])/1000:.1f} s)")
    if marks:
        print("marks:")
        for t, lab in marks:
            offset = (t - rows[0]['t_ms']) / 1000.0
            print(f"  +{offset:7.1f}s  {lab}")
    else:
        print("WARNING: no MARK rows found -- all windows will be labeled "
              "'Rest' or 'Unlabeled'. Did you press the digit keys?")

    if args.require_running:
        # Trim to first row where emg_phase == "RUN" (skip CAL, ACTIVE_CAL).
        running_rows = [r for r in rows if r["emg_phase"] == "RUN"]
        if not running_rows:
            print("WARNING: no rows in RUN phase. Output will be empty.",
                  file=sys.stderr)
            running_rows = rows
        else:
            print(f"using {len(running_rows)} RUN-phase rows of {len(rows)}")
        rows = running_rows

    # Build a uniform time index. Use rows' actual t_ms for window edges --
    # this naturally handles UART jitter.
    win_ms = int(args.window * 1000)
    hop_ms = int(args.hop * 1000)

    out_rows = []
    label_counts = Counter()
    n_dropped_rest = 0
    n_dropped_mixed = 0

    t0 = rows[0]["t_ms"]
    t_end = rows[-1]["t_ms"]

    win_start = t0
    while win_start + win_ms <= t_end:
        win_end = win_start + win_ms
        win = [r for r in rows if win_start <= r["t_ms"] < win_end]
        if len(win) < 5:
            win_start += hop_ms
            continue

        # Labels of every row in this window. If they disagree, drop the
        # window (transition zone) unless they're all 'Rest'.
        row_labels = [label_at(r["t_ms"], marks) for r in win]
        uniq = set(row_labels)
        if len(uniq) > 1:
            n_dropped_mixed += 1
            win_start += hop_ms
            continue
        label = row_labels[0]

        if label == "Rest" and not args.keep_rest:
            n_dropped_rest += 1
            win_start += hop_ms
            continue

        feat = window_features(win)
        if feat is None:
            win_start += hop_ms
            continue

        feat["t_start_ms"] = win[0]["t_ms"]
        feat["t_end_ms"]   = win[-1]["t_ms"]
        feat["n_rows"]     = len(win)
        feat["label"]      = label
        feat["session"]    = log_path.stem
        out_rows.append(feat)
        label_counts[label] += 1

        win_start += hop_ms

    print()
    print(f"emitted {len(out_rows)} windows")
    print(f"dropped: {n_dropped_rest} rest, {n_dropped_mixed} mixed-label")
    print("label counts:")
    for lab, n in label_counts.most_common():
        print(f"  {lab:<14} {n:>4}")

    # Write CSV.
    cols = ["session", "t_start_ms", "t_end_ms", "n_rows", "label"] + FEATURE_COLS
    with out_path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in out_rows:
            w.writerow({k: r.get(k, "") for k in cols})
    print(f"\nwrote: {out_path}")


if __name__ == "__main__":
    main()
