"""Curate high-quality windows from recent monitor.py logs for training.

Outputs:
    D:\\mpu_wave\\training_curated\\action_windows.csv
    D:\\mpu_wave\\training_curated\\fatigue_windows.csv
    D:\\mpu_wave\\training_curated\\session_quality_report.csv

Action target labels:
    Squat / Deadlift / Walking

Fatigue target labels:
    FreshSquat / FatiguedSquat
"""
from __future__ import annotations

import argparse
import csv
import math
from collections import Counter
from pathlib import Path

import numpy as np
import pandas as pd


ROOT = Path(r"D:\mpu_wave")
OUT_DIR = ROOT / "training_curated"

ADS_LSB_V = 2.0 * 2.42 / (6.0 * (1 << 24))
ACC_LSB_G = 1.0 / 16384.0
GYRO_LSB_DPS = 1.0 / 131.0

WINDOW_S = 3.0
HOP_S = 1.0
LIVE_FS_HZ = 20.0
EMG_FS_HZ = 500.0
EMG_WINDOW_SAMPLES = 256

FEATURE_COLS = [
    "mav", "rms", "wl", "iemg", "var", "zc", "ssc",
    "swing", "tilt_std", "tilt_p95", "tilt_zc",
    "gyro_mean", "gyro_max", "gyro_std", "acc_std",
]

ACTION_LABELS = {"Squat", "Deadlift", "Walking"}
FATIGUE_LABELS = {"FreshSquat", "FatiguedSquat"}


def normalize_action_label(label: str):
    if label in ("FreshSquat", "FatiguedSquat"):
        return "Squat"
    if label in ACTION_LABELS:
        return label
    return None


def parse_live(line: str):
    if not line.startswith("LIVE,"):
        return None
    p = line.strip().split(",")
    if len(p) < 22:
        return None
    try:
        i = lambda x: int(float(x))
        return dict(
            t_ms=i(p[1]), ac=i(p[2]), rms=i(p[3]), mav=i(p[4]),
            wl=i(p[5]), zc=i(p[6]), ssc=i(p[7]),
            emg_phase=p[11].strip(), emg_status=p[12].strip(),
            mot_phase=p[13].strip(), angle_x10=i(p[14]), active=i(p[15]),
            ax=i(p[16]), ay=i(p[17]), az=i(p[18]),
            gx=i(p[19]), gy=i(p[20]), gz=i(p[21]),
        )
    except Exception:
        return None


def parse_mark(line: str):
    if not line.startswith("MARK,"):
        return None
    p = line.strip().split(",", 2)
    if len(p) < 3:
        return None
    try:
        return int(float(p[1])), p[2].strip()
    except Exception:
        return None


def label_at(t_ms: int, marks):
    label = "Rest"
    for t, lab in marks:
        if t <= t_ms:
            label = lab
        else:
            break
    return label


def read_log(path: Path):
    live = []
    marks = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            row = parse_live(line)
            if row is not None:
                live.append(row)
                continue
            mark = parse_mark(line)
            if mark is not None:
                marks.append(mark)
    marks.sort(key=lambda x: x[0])
    return live, marks


def zero_crossings(x: np.ndarray) -> int:
    if len(x) < 2:
        return 0
    a = x[:-1]
    b = x[1:]
    return int(np.sum(((a > 0) & (b < 0)) | ((a < 0) & (b > 0))))


def window_features(rows):
    duration_s = max(0.001, (rows[-1]["t_ms"] - rows[0]["t_ms"]) / 1000.0)
    emg_samples = max(1.0, duration_s * EMG_FS_HZ)
    scale = emg_samples / EMG_WINDOW_SAMPLES

    mav_arr = np.asarray([r["mav"] for r in rows], dtype=np.float64) * ADS_LSB_V
    rms_arr = np.asarray([r["rms"] for r in rows], dtype=np.float64) * ADS_LSB_V
    wl_arr = np.asarray([r["wl"] for r in rows], dtype=np.float64) * ADS_LSB_V
    zc_arr = np.asarray([r["zc"] for r in rows], dtype=np.float64)
    ssc_arr = np.asarray([r["ssc"] for r in rows], dtype=np.float64)

    mav = float(np.mean(mav_arr))
    rms = float(np.sqrt(np.mean(rms_arr * rms_arr)))
    wl = float(np.mean(wl_arr) * scale)
    iemg = float(mav * emg_samples)
    var = float(max(0.0, np.mean(rms_arr * rms_arr) - mav * mav))
    zc = float(np.mean(zc_arr) * scale)
    ssc = float(np.mean(ssc_arr) * scale)

    angle = np.asarray([r["angle_x10"] for r in rows], dtype=np.float64) / 10.0
    angle_rel = angle - angle[0]
    swing = float(angle_rel.max() - angle_rel.min())
    tilt_std = float(angle_rel.std())
    tilt_p95 = float(np.percentile(np.abs(angle_rel), 95))
    tilt_zc = float(zero_crossings(angle_rel))

    gx = np.asarray([r["gx"] for r in rows], dtype=np.float64) * GYRO_LSB_DPS
    gy = np.asarray([r["gy"] for r in rows], dtype=np.float64) * GYRO_LSB_DPS
    gz = np.asarray([r["gz"] for r in rows], dtype=np.float64) * GYRO_LSB_DPS
    gyro_mag = np.sqrt(gx * gx + gy * gy + gz * gz)

    ax = np.asarray([r["ax"] for r in rows], dtype=np.float64) * ACC_LSB_G
    ay = np.asarray([r["ay"] for r in rows], dtype=np.float64) * ACC_LSB_G
    az = np.asarray([r["az"] for r in rows], dtype=np.float64) * ACC_LSB_G
    acc_mag = np.sqrt(ax * ax + ay * ay + az * az)

    return dict(
        mav=mav, rms=rms, wl=wl, iemg=iemg, var=var, zc=zc, ssc=ssc,
        swing=swing, tilt_std=tilt_std, tilt_p95=tilt_p95, tilt_zc=tilt_zc,
        gyro_mean=float(gyro_mag.mean()),
        gyro_max=float(gyro_mag.max()),
        gyro_std=float(gyro_mag.std()),
        acc_std=float(acc_mag.std()),
        acc_mag_median=float(np.median(acc_mag)),
        imu_nonzero_ratio=float(np.mean(acc_mag > 0.05)),
        duration_s=duration_s,
    )


def add_fatigue_ratios(row):
    row = dict(row)
    row["zc_per_mav"] = row["zc"] / (row["mav"] + 1e-9)
    row["ssc_per_rms"] = row["ssc"] / (row["rms"] + 1e-9)
    row["rms_mav_ratio"] = row["rms"] / (row["mav"] + 1e-9)
    row["wl_per_iemg"] = row["wl"] / (row["iemg"] + 1e-9)
    row["var_per_rms2"] = row["var"] / ((row["rms"] ** 2) + 1e-9)
    return row


def quality_summary(live, marks):
    labels = Counter(label for _, label in marks)
    if not live:
        return dict(live_rows=0, marks=len(marks), labels=dict(labels))

    acc_mags = []
    angles = []
    mavs = []
    for r in live:
        acc_mags.append(math.sqrt(r["ax"] ** 2 + r["ay"] ** 2 + r["az"] ** 2) * ACC_LSB_G)
        angles.append(r["angle_x10"] / 10.0)
        mavs.append(r["mav"])

    imu_ok_ratio = float(np.mean(np.asarray(acc_mags) > 0.05))
    return dict(
        live_rows=len(live),
        marks=len(marks),
        labels=dict(labels),
        span_s=(live[-1]["t_ms"] - live[0]["t_ms"]) / 1000.0,
        imu_ok_ratio=imu_ok_ratio,
        acc_mag_median=float(np.median(acc_mags)),
        angle_range_deg=float(max(angles) - min(angles)),
        mav_median=float(np.median(mavs)),
    )


def curate_log(log_path: Path, window_s: float, hop_s: float):
    live, marks = read_log(log_path)
    report = quality_summary(live, marks)
    report["session"] = log_path.stem
    report["path"] = str(log_path)

    if not live:
        report["action_windows"] = 0
        report["fatigue_windows"] = 0
        report["status"] = "reject:no_live"
        return [], [], report

    win_ms = int(round(window_s * 1000))
    hop_ms = int(round(hop_s * 1000))
    min_rows = int(round(window_s * LIVE_FS_HZ * 0.6))

    action_rows = []
    fatigue_rows = []
    dropped_mixed = 0
    dropped_short = 0

    t = live[0]["t_ms"]
    t_end = live[-1]["t_ms"]
    while t + win_ms <= t_end:
        w0 = t
        w1 = t + win_ms
        win = [r for r in live if w0 <= r["t_ms"] < w1]
        if len(win) < min_rows:
            dropped_short += 1
            t += hop_ms
            continue

        row_labels = [label_at(r["t_ms"], marks) for r in win]
        if len(set(row_labels)) != 1:
            dropped_mixed += 1
            t += hop_ms
            continue
        raw_label = row_labels[0]
        if raw_label == "Rest":
            t += hop_ms
            continue

        feats = window_features(win)
        base = dict(
            session=log_path.stem,
            source_log=str(log_path),
            t_start_ms=win[0]["t_ms"],
            t_end_ms=win[-1]["t_ms"],
            n_rows=len(win),
            label=raw_label,
        )
        base.update(feats)

        action_label = normalize_action_label(raw_label)
        if action_label is not None:
            # Action needs a working IMU. Keep low-motion Squat/FreshSquat
            # windows out if the IMU is still all zero.
            if 0.5 <= feats["acc_mag_median"] <= 1.5 and feats["imu_nonzero_ratio"] >= 0.8:
                row = dict(base)
                row["true_action"] = action_label
                action_rows.append(row)

        if raw_label in FATIGUE_LABELS:
            # Fatigue is EMG-only, so IMU failure is not fatal. Exclude only
            # windows with empty/near-empty EMG.
            if feats["mav"] > 1e-7 and feats["rms"] > 1e-7 and feats["wl"] > 1e-5:
                row = add_fatigue_ratios(base)
                row["fatigue"] = 1 if raw_label == "FatiguedSquat" else 0
                row["group"] = log_path.stem
                fatigue_rows.append(row)

        t += hop_ms

    report["action_windows"] = len(action_rows)
    report["fatigue_windows"] = len(fatigue_rows)
    report["dropped_mixed"] = dropped_mixed
    report["dropped_short"] = dropped_short

    status = []
    if report.get("imu_ok_ratio", 0.0) < 0.8:
        status.append("imu_bad_for_action")
    if len(marks) == 0:
        status.append("no_marks")
    if len(action_rows) == 0:
        status.append("no_action_windows")
    if len(fatigue_rows) == 0:
        status.append("no_fatigue_windows")
    report["status"] = ";".join(status) if status else "ok"
    return action_rows, fatigue_rows, report


def write_csv(path: Path, rows):
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="*", help="session_*.log files; default uses D:\\mpu_wave recent logs")
    ap.add_argument("--out-dir", default=str(OUT_DIR))
    ap.add_argument("--window-s", type=float, default=WINDOW_S)
    ap.add_argument("--hop-s", type=float, default=HOP_S)
    args = ap.parse_args()

    if args.logs:
        logs = [Path(p) for p in args.logs]
    else:
        logs = sorted(ROOT.glob("session_*.log"), key=lambda p: p.stat().st_mtime)

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    action_all = []
    fatigue_all = []
    reports = []
    for log in logs:
        action_rows, fatigue_rows, report = curate_log(log, args.window_s, args.hop_s)
        action_all.extend(action_rows)
        fatigue_all.extend(fatigue_rows)
        reports.append(report)

    write_csv(out_dir / "action_windows.csv", action_all)
    write_csv(out_dir / "fatigue_windows.csv", fatigue_all)
    pd.DataFrame(reports).to_csv(out_dir / "session_quality_report.csv", index=False)

    print(f"logs scanned: {len(logs)}")
    print(f"action windows: {len(action_all)}")
    print(f"fatigue windows: {len(fatigue_all)}")
    if action_all:
        print("action labels:", Counter(r["true_action"] for r in action_all))
    if fatigue_all:
        print("fatigue labels:", Counter(r["label"] for r in fatigue_all))
    print(f"wrote: {out_dir}")


if __name__ == "__main__":
    main()
