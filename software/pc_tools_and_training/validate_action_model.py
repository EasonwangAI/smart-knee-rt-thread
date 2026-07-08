"""Validate the external KneE-PAD action model on our own knee-brace logs.

Input logs come from monitor.py and contain LIVE rows plus MARK rows. Press
keys in monitor.py to create MARK rows:

    1 Squat, 2 Walking, 4 Deadlift, 0 Rest

The script builds fixed-length windows inside each marked action interval,
extracts the 15 features expected by kneepad_action_rf.joblib, runs prediction,
and writes a feature/prediction CSV next to the log or into --out-dir.

Usage:
    python validate_action_model.py session_20260602_150000.log
    python validate_action_model.py session_a.log session_b.log --window-s 3
"""
from __future__ import annotations

import argparse
import warnings
from collections import Counter
from pathlib import Path

import numpy as np
import pandas as pd
from joblib import load

warnings.filterwarnings("ignore", category=UserWarning)


MODEL_PATH = Path(r"D:\RT-ThreadStudio\workspace\test_pro3\python\kneepad_action_rf.joblib")
TRAIN_FEATURES_PATH = Path(r"D:\RT-ThreadStudio\workspace\test_pro3\python\kneepad_action_features.csv")

EMG_FS_HZ = 500.0
LIVE_FS_HZ = 20.0
EMG_WINDOW_SAMPLES = 256

ACC_LSB_G = 1.0 / 16384.0
GYRO_LSB_DPS = 1.0 / 131.0
ADS_VREF = 2.42

FEATURE_COLS = [
    "mav", "rms", "wl", "iemg", "var", "zc", "ssc",
    "swing", "tilt_std", "tilt_p95", "tilt_zc",
    "gyro_mean", "gyro_max", "gyro_std", "acc_std",
]

TARGET_ACTIONS = ["Squat", "Deadlift", "Walking"]

LABEL_MAP = {
    "squat": "Squat",
    "freshsquat": "Squat",
    "fatiguedsquat": "Squat",
    "walking": "Walking",
    "walk": "Walking",
    "deadlift": "Deadlift",
    "hardpull": "Deadlift",
    "hiphinge": "Deadlift",
    # Legacy labels from the external KneE-PAD model. These are not treated as
    # Deadlift because they are a different movement.
    "legext": "LegExt",
    "extension": "LegExt",
    "legextension": "LegExt",
    "kneeext": "LegExt",
    "kneeextension": "LegExt",
}


def ads_lsb_mv(gain: float) -> float:
    return 1000.0 * (2.0 * ADS_VREF) / (gain * (1 << 24))


def parse_live(line: str):
    if not line.startswith("LIVE,"):
        return None
    parts = line.strip().split(",")
    if len(parts) < 22:
        return None
    try:
        return {
            "t_ms": int(float(parts[1])),
            "ac": int(float(parts[2])),
            "rms": int(float(parts[3])),
            "mav": int(float(parts[4])),
            "wl": int(float(parts[5])),
            "zc": int(float(parts[6])),
            "ssc": int(float(parts[7])),
            "angle_x10": int(float(parts[14])),
            "active": int(float(parts[15])),
            "ax": int(float(parts[16])),
            "ay": int(float(parts[17])),
            "az": int(float(parts[18])),
            "gx": int(float(parts[19])),
            "gy": int(float(parts[20])),
            "gz": int(float(parts[21])),
        }
    except ValueError:
        return None


def parse_mark(line: str):
    if not line.startswith("MARK,"):
        return None
    parts = line.strip().split(",", 2)
    if len(parts) < 3:
        return None
    try:
        return int(float(parts[1])), parts[2].strip()
    except ValueError:
        return None


def normalize_label(label: str):
    key = "".join(ch for ch in label.lower() if ch.isalnum())
    return LABEL_MAP.get(key)


def read_log(path: Path):
    lives = []
    marks = []
    other_labels = Counter()

    with path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            live = parse_live(line)
            if live is not None:
                lives.append(live)
                continue
            mark = parse_mark(line)
            if mark is not None:
                t_ms, raw_label = mark
                label = normalize_label(raw_label)
                if label is None:
                    other_labels[raw_label] += 1
                marks.append((t_ms, raw_label, label))

    lives_df = pd.DataFrame(lives)
    if not lives_df.empty:
        lives_df = lives_df.sort_values("t_ms").drop_duplicates("t_ms")
    marks.sort(key=lambda x: x[0])
    return lives_df, marks, other_labels


def label_intervals(marks, last_t_ms: int, guard_ms: int):
    intervals = []
    for i, (start, raw_label, label) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else last_t_ms
        if label is None:
            continue
        a = start + guard_ms
        b = end - guard_ms
        if b > a:
            intervals.append((a, b, raw_label, label))
    return intervals


def zero_crossings(values: np.ndarray) -> int:
    if values.size < 2:
        return 0
    a = values[:-1]
    b = values[1:]
    return int(np.sum((a > 0) & (b < 0) | ((a < 0) & (b > 0))))


def extract_window_features(win: pd.DataFrame, lsb_mv: float):
    duration_s = max(0.001, (float(win["t_ms"].iloc[-1]) - float(win["t_ms"].iloc[0])) / 1000.0)
    emg_samples = max(1.0, duration_s * EMG_FS_HZ)

    rms_win_mv = win["rms"].to_numpy(np.float64) * lsb_mv
    mav_win_mv = win["mav"].to_numpy(np.float64) * lsb_mv
    wl_win_mv = win["wl"].to_numpy(np.float64) * lsb_mv

    mav = float(np.mean(mav_win_mv))
    rms = float(np.sqrt(np.mean(rms_win_mv * rms_win_mv)))
    # LIVE wl/zc/ssc are 256-sample rolling-window totals. Convert them into
    # approximate full-window totals so they resemble KneE-PAD trial features.
    wl = float(np.mean(wl_win_mv) * emg_samples / EMG_WINDOW_SAMPLES)
    iemg = float(mav * emg_samples)
    var = float(np.mean(rms_win_mv * rms_win_mv))
    zc = float(np.mean(win["zc"].to_numpy(np.float64)) * emg_samples / EMG_WINDOW_SAMPLES)
    ssc = float(np.mean(win["ssc"].to_numpy(np.float64)) * emg_samples / EMG_WINDOW_SAMPLES)

    tilt = win["angle_x10"].to_numpy(np.float64) / 10.0
    tilt_rel = tilt - tilt[0]
    swing = float(np.max(tilt_rel) - np.min(tilt_rel))
    tilt_std = float(np.std(tilt_rel))
    tilt_p95 = float(np.percentile(np.abs(tilt_rel), 95))
    tilt_zc = float(zero_crossings(tilt_rel))

    gx = win["gx"].to_numpy(np.float64) * GYRO_LSB_DPS
    gy = win["gy"].to_numpy(np.float64) * GYRO_LSB_DPS
    gz = win["gz"].to_numpy(np.float64) * GYRO_LSB_DPS
    gyro_mag = np.sqrt(gx * gx + gy * gy + gz * gz)
    gyro_mean = float(np.mean(gyro_mag))
    gyro_max = float(np.max(gyro_mag))
    gyro_std = float(np.std(gyro_mag))

    ax = win["ax"].to_numpy(np.float64) * ACC_LSB_G
    ay = win["ay"].to_numpy(np.float64) * ACC_LSB_G
    az = win["az"].to_numpy(np.float64) * ACC_LSB_G
    acc_mag = np.sqrt(ax * ax + ay * ay + az * az)
    acc_std = float(np.std(acc_mag))

    return {
        "mav": mav, "rms": rms, "wl": wl, "iemg": iemg,
        "var": var, "zc": zc, "ssc": ssc,
        "swing": swing, "tilt_std": tilt_std, "tilt_p95": tilt_p95,
        "tilt_zc": tilt_zc, "gyro_mean": gyro_mean, "gyro_max": gyro_max,
        "gyro_std": gyro_std, "acc_std": acc_std,
        "duration_s": duration_s, "live_rows": int(len(win)),
    }


def build_feature_rows(log_path: Path, window_s: float, hop_s: float,
                       guard_s: float, min_coverage: float, ads_gain: float):
    lives, marks, other_labels = read_log(log_path)
    if lives.empty:
        return pd.DataFrame(), {"live_rows": 0, "marks": len(marks), "other_labels": other_labels}

    guard_ms = int(round(guard_s * 1000.0))
    intervals = label_intervals(marks, int(lives["t_ms"].max()), guard_ms)
    min_rows = max(3, int(round(window_s * LIVE_FS_HZ * min_coverage)))
    lsb_mv = ads_lsb_mv(ads_gain)

    rows = []
    for start_ms, end_ms, raw_label, label in intervals:
        t = start_ms
        step_ms = int(round(hop_s * 1000.0))
        win_ms = int(round(window_s * 1000.0))
        while t + win_ms <= end_ms:
            w0 = t
            w1 = t + win_ms
            win = lives[(lives["t_ms"] >= w0) & (lives["t_ms"] < w1)]
            if len(win) >= min_rows:
                feats = extract_window_features(win, lsb_mv)
                feats.update({
                    "source": str(log_path),
                    "t0_ms": int(w0),
                    "t1_ms": int(w1),
                    "raw_label": raw_label,
                    "true_action": label,
                })
                rows.append(feats)
            t += step_ms

    meta = {
        "live_rows": int(len(lives)),
        "marks": int(len(marks)),
        "intervals": int(len(intervals)),
        "other_labels": other_labels,
    }
    return pd.DataFrame(rows), meta


def add_predictions(df: pd.DataFrame, model):
    if df.empty:
        return df
    X = df[FEATURE_COLS]
    pred = model.predict(X)
    prob = model.predict_proba(X)
    classes = list(model.classes_)

    out = df.copy()
    out["pred_action"] = pred
    for i, cls in enumerate(classes):
        out[f"prob_{cls}"] = prob[:, i]
    out["pred_confidence"] = np.max(prob, axis=1)
    return out


def print_report(df: pd.DataFrame, metas):
    print("=" * 72)
    print("Action model validation")
    print("=" * 72)
    print(f"Feature windows: {len(df)}")
    print(f"LIVE rows seen:  {sum(m['live_rows'] for m in metas)}")
    print(f"MARK rows seen:  {sum(m['marks'] for m in metas)}")
    ignored = Counter()
    for m in metas:
        ignored.update(m["other_labels"])
    if ignored:
        print(f"Ignored labels:  {dict(ignored)}")
    print()

    if df.empty:
        print("No usable action windows. Add MARK labels and keep each action segment longer than the window length.")
        return

    print("True actions:")
    print(Counter(df["true_action"]).most_common())
    print("Predicted actions:")
    print(Counter(df["pred_action"]).most_common())
    print()

    print("Target confusion matrix (rows=true, cols=pred):")
    extras = sorted((set(df["true_action"]) | set(df["pred_action"])) - set(TARGET_ACTIONS))
    labels = TARGET_ACTIONS + extras
    cm = pd.crosstab(df["true_action"], df["pred_action"]).reindex(index=labels, columns=labels, fill_value=0)
    print(cm.to_string())
    print()

    acc = float(np.mean(df["true_action"].to_numpy() == df["pred_action"].to_numpy()))
    print(f"Target window accuracy: {acc:.1%}")
    if "Deadlift" not in set(df["pred_action"]):
        print("NOTE: the loaded action model did not predict Deadlift. If the model classes do not include Deadlift, collect Deadlift data and train a new target model.")
    print()

    if TRAIN_FEATURES_PATH.exists():
        ref = pd.read_csv(TRAIN_FEATURES_PATH)
        own_med = df[FEATURE_COLS].median()
        ref_med = ref[FEATURE_COLS].median()
        ratio = (own_med / (ref_med.abs() + 1e-12)).sort_values(ascending=False)
        print("Feature median ratio vs KneE-PAD training data:")
        for name, value in ratio.items():
            flag = "  *" if value < 0.2 or value > 5.0 else ""
            print(f"  {name:<10} {value:>8.3f}{flag}")
        print("  * means the median is far from the external training domain.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+", help="monitor.py .log files")
    ap.add_argument("--model", default=str(MODEL_PATH))
    ap.add_argument("--out-dir", default="", help="where to write *_action_features.csv")
    ap.add_argument("--window-s", type=float, default=3.0)
    ap.add_argument("--hop-s", type=float, default=1.0)
    ap.add_argument("--guard-s", type=float, default=0.5)
    ap.add_argument("--min-coverage", type=float, default=0.6)
    ap.add_argument("--ads-gain", type=float, default=6.0, help="ADS1292R channel gain used for LIVE EMG features")
    args = ap.parse_args()

    model_path = Path(args.model)
    if not model_path.exists():
        raise SystemExit(f"model not found: {model_path}")
    model = load(model_path)
    if getattr(model, "n_features_in_", None) != len(FEATURE_COLS):
        raise SystemExit(f"model expects {getattr(model, 'n_features_in_', None)} features, script has {len(FEATURE_COLS)}")

    out_dir = Path(args.out_dir) if args.out_dir else None
    if out_dir is not None:
        out_dir.mkdir(parents=True, exist_ok=True)

    all_rows = []
    metas = []
    for log in args.logs:
        log_path = Path(log)
        rows, meta = build_feature_rows(
            log_path, args.window_s, args.hop_s,
            args.guard_s, args.min_coverage, args.ads_gain
        )
        metas.append(meta)
        if not rows.empty:
            all_rows.append(rows)

    df = pd.concat(all_rows, ignore_index=True) if all_rows else pd.DataFrame()
    df = add_predictions(df, model) if not df.empty else df
    print_report(df, metas)

    if not df.empty:
        if out_dir is not None:
            out_path = out_dir / "action_validation_features.csv"
        elif len(args.logs) == 1:
            p = Path(args.logs[0])
            out_path = p.with_name(p.stem + "_action_features.csv")
        else:
            out_path = Path.cwd() / "action_validation_features.csv"
        df.to_csv(out_path, index=False, encoding="utf-8")
        print()
        print(f"Saved: {out_path}")


if __name__ == "__main__":
    main()
