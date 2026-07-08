"""Train a 3-class action classifier (Squat / LegExtension / Walking) on
KneE-PAD, simulating the user's single-leg knee-brace hardware:

  - 1 EMG channel (the most active one per trial)
  - 1 IMU sensor (the highest-tilt-swing one per trial)
  - 500 SPS EMG, 50 Hz IMU (resampled to match the firmware)

The full KneE-PAD label set is:
    0  Squat (correct)
    1  Squat_WT (wrong: weight transfer)
    2  Squat_FL (wrong: front leg)
    3  Extension (correct)
    4  Extension_NF (wrong: not full)
    5  Extension_LL (wrong: lifted/abducted)
    6  Walking (correct)
    7  Walking variant
    8  Walking variant

For *action classification* we collapse this to 3 macro classes:
    Squat       <- {0, 1, 2}
    LegExt      <- {3, 4, 5}
    Walking     <- {6, 7, 8}

Methodology:
  - Each trial -> one feature vector (single EMG channel + single IMU's accel)
  - Subject-grouped 5-fold CV (no subject overlap between train/test)
  - RandomForest classifier
  - Reports confusion matrix, per-class precision/recall, feature importance

Output:
  - python/kneepad_action_features.csv  (per-trial feature dump for later use)
  - kneepad_rf_model.joblib             (trained model on full data, optional)

Usage:
    python python/train_action_classifier.py [--root D:\\datasets\\dataset]
"""
from __future__ import annotations

import argparse
import statistics
from collections import Counter
from pathlib import Path

import numpy as np
import pandas as pd
from scipy.signal import sosfilt
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import (accuracy_score, classification_report,
                              confusion_matrix, f1_score)
from sklearn.model_selection import GroupKFold

EMG_FS = 1259.0
IMU_FS = 148.0
TARGET_EMG_FS = 500.0
TARGET_IMU_FS = 50.0

BIQUAD_COEFS = [
    dict(b0=0.8370891906,  b1=-1.6741783811, b2=0.8370891906,
         a1=-1.6474599811, a2=0.7008967812),
    dict(b0=0.9902986180,  b1=-1.6023368229, b2=0.9902986180,
         a1=-1.6023368229, a2=0.9805972359),
    dict(b0=0.3913357725,  b1=0.7826715450,  b2=0.3913357725,
         a1=0.3695273774,  a2=0.1958157127),
]

LABEL_TO_ACTION = {
    "0": "Squat",   "1": "Squat",   "2": "Squat",
    "3": "LegExt",  "4": "LegExt",  "5": "LegExt",
    "6": "Walking", "7": "Walking", "8": "Walking",
}
ACTIONS = ["Squat", "LegExt", "Walking"]


def biquad_cascade(x):
    sos = np.array([[c["b0"], c["b1"], c["b2"], 1.0, c["a1"], c["a2"]]
                    for c in BIQUAD_COEFS], dtype=np.float64)
    return sosfilt(sos, x.astype(np.float64))


def resample_linear(x, src_fs, dst_fs):
    n_in = len(x)
    dur = (n_in - 1) / src_fs
    n_out = int(round(dur * dst_fs)) + 1
    t_in  = np.linspace(0, dur, n_in)
    t_out = np.linspace(0, dur, n_out)
    return np.interp(t_out, t_in, x)


def pick_active_emg_channel(emg):
    """Channel with the largest std after coarse DC removal."""
    detrended = emg - emg.mean(axis=1, keepdims=True)
    return int(detrended.std(axis=1).argmax())


def pick_thigh_imu(imu):
    """IMU sensor (0..7) whose accel tilt swing is largest."""
    best_idx, best_swing = 0, -1.0
    for s in range(8):
        ax = imu[s * 6 + 0]; ay = imu[s * 6 + 1]; az = imu[s * 6 + 2]
        tilt = np.degrees(np.arctan2(ax, np.sqrt(ay * ay + az * az)))
        swing = float(tilt.max() - tilt.min())
        if swing > best_swing:
            best_swing = swing
            best_idx = s
    return best_idx, best_swing


def extract_features(emg_path, imu_path):
    emg = np.load(emg_path)
    imu = np.load(imu_path)

    # EMG single channel.
    ch = pick_active_emg_channel(emg)
    raw = emg[ch].astype(np.float64)
    emg500 = resample_linear(raw, EMG_FS, TARGET_EMG_FS)
    filt = biquad_cascade(emg500)

    # Time-domain EMG features over the full trial (~3 s).
    abs_x = np.abs(filt)
    mav  = float(abs_x.mean())
    rms  = float(np.sqrt((filt ** 2).mean()))
    wl   = float(np.abs(np.diff(filt)).sum())
    iemg = float(abs_x.sum())
    var  = float(filt.var())

    # ZC / SSC with a threshold derived from this trial's std (so it scales
    # with the per-trial amplitude rather than a global constant).
    sigma = float(filt.std())
    th = max(1e-4, sigma * 0.5)
    a = filt[:-1]; b = filt[1:]
    zc_mask = (np.abs(a) > th) & (np.abs(b) > th) & ((a > 0) != (b > 0))
    zc = int(zc_mask.sum())
    if len(filt) >= 3:
        d = np.diff(filt); d1 = d[:-1]; d2 = d[1:]
        ssc_mask = (((d1 > 0) & (d2 < 0)) | ((d1 < 0) & (d2 > 0))) & (
                   (np.abs(d1) > th) | (np.abs(d2) > th))
        ssc = int(ssc_mask.sum())
    else:
        ssc = 0

    # Crude mean / median frequency proxies via power spectrum (small FFT).
    n = len(filt)
    if n > 16:
        N = 1 << int(np.ceil(np.log2(n)))
        spec = np.abs(np.fft.rfft(filt, n=N)) ** 2
        freqs = np.fft.rfftfreq(N, 1.0 / TARGET_EMG_FS)
        psum = spec.sum() + 1e-12
        mnf = float((freqs * spec).sum() / psum)
        csum = np.cumsum(spec)
        mdf_idx = int(np.searchsorted(csum, csum[-1] / 2))
        mdf = float(freqs[min(mdf_idx, len(freqs) - 1)])
    else:
        mnf = mdf = 0.0

    # IMU features from the chosen thigh sensor.
    sensor_idx, _ = pick_thigh_imu(imu)
    ax = imu[sensor_idx * 6 + 0]
    ay = imu[sensor_idx * 6 + 1]
    az = imu[sensor_idx * 6 + 2]
    gx = imu[sensor_idx * 6 + 3]
    gy = imu[sensor_idx * 6 + 4]
    gz = imu[sensor_idx * 6 + 5]

    tilt = np.degrees(np.arctan2(ax, np.sqrt(ay * ay + az * az)))
    tilt_rel = tilt - tilt[0]
    swing = float(tilt_rel.max() - tilt_rel.min())
    tilt_std = float(tilt_rel.std())
    tilt_p95 = float(np.percentile(np.abs(tilt_rel), 95))

    gyro_mag = np.sqrt(gx * gx + gy * gy + gz * gz)
    gyro_mean = float(gyro_mag.mean())
    gyro_max  = float(gyro_mag.max())
    gyro_std  = float(gyro_mag.std())

    acc_mag = np.sqrt(ax * ax + ay * ay + az * az)
    acc_std = float(acc_mag.std())

    # Cadence proxy: count zero crossings of tilt_rel (works well for walking).
    tilt_zc = int((np.sign(tilt_rel[:-1]) * np.sign(tilt_rel[1:]) < 0).sum())

    return dict(
        emg_ch=ch, imu_idx=sensor_idx,
        # EMG features
        mav=mav, rms=rms, wl=wl, iemg=iemg, var=var,
        zc=zc, ssc=ssc, mnf=mnf, mdf=mdf,
        # IMU features
        swing=swing, tilt_std=tilt_std, tilt_p95=tilt_p95, tilt_zc=tilt_zc,
        gyro_mean=gyro_mean, gyro_max=gyro_max, gyro_std=gyro_std,
        acc_std=acc_std,
        # Trial duration (samples / IMU rate).
        duration_s=float(imu.shape[1] / IMU_FS),
    )


def walk_trials(root: Path):
    for sub in sorted(root.glob("Subject_*"), key=lambda p: int(p.name.split("_")[1])):
        for ldir in sorted(sub.iterdir(), key=lambda p: p.name):
            if not ldir.is_dir(): continue
            lab = ldir.name
            if lab not in LABEL_TO_ACTION: continue
            for tdir in sorted(ldir.glob("Trial_*"), key=lambda p: int(p.name.split("_")[1])):
                e = tdir / "emg.npy"; i = tdir / "imu.npy"
                if e.exists() and i.exists():
                    yield sub.name, lab, tdir.name, e, i


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=r"D:\datasets\dataset")
    ap.add_argument("--folds", type=int, default=5)
    ap.add_argument("--n-estimators", type=int, default=400)
    ap.add_argument("--max-depth", type=int, default=12)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--save-model", default="")
    args = ap.parse_args()

    root = Path(args.root)
    if not root.exists():
        raise SystemExit(f"dataset root not found: {root}")

    print("Extracting features from all trials...")
    rows = []
    n = 0
    skipped = 0
    for sub, lab, trial, emg_p, imu_p in walk_trials(root):
        try:
            # Quickly reject degenerate trials.
            emg = np.load(emg_p)
            imu = np.load(imu_p)
            if emg.ndim != 2 or imu.ndim != 2 or emg.shape[1] < 64 or imu.shape[1] < 8:
                skipped += 1
                continue
            feat = extract_features(emg_p, imu_p)
        except Exception as ex:
            skipped += 1
            continue
        feat["subject"] = sub
        feat["trial"]   = trial
        feat["raw_label"] = lab
        feat["action"] = LABEL_TO_ACTION[lab]
        rows.append(feat)
        n += 1
        if n % 400 == 0:
            print(f"  ...{n}")
    print(f"total trials: {n}  skipped: {skipped}")

    df = pd.DataFrame(rows)
    out_csv = Path(__file__).resolve().parent / "kneepad_action_features.csv"
    df.to_csv(out_csv, index=False)
    print(f"feature dump: {out_csv}")
    print()

    print("class distribution (3-class):", Counter(df["action"]))
    print("subjects:", df["subject"].nunique())
    print()

    feature_cols = [
        # EMG (all derivable from firmware's per-window LIVE output)
        "mav", "rms", "wl", "iemg", "var", "zc", "ssc",
        # IMU (all derivable from raw ax/ay/az/gx/gy/gz + angle in LIVE)
        "swing", "tilt_std", "tilt_p95", "tilt_zc",
        "gyro_mean", "gyro_max", "gyro_std", "acc_std",
    ]
    # NOTE: 'duration_s' deliberately excluded -- in firmware deployment we
    # always classify a fixed-size sliding window. 'mnf'/'mdf' excluded
    # because computing FFT on the MCU is heavy and the firmware doesn't
    # ship raw EMG samples to the host -- only per-window aggregates.
    X = df[feature_cols]
    y = df["action"]
    groups = df["subject"]

    rf = RandomForestClassifier(
        n_estimators=args.n_estimators,
        max_depth=args.max_depth,
        class_weight="balanced",
        random_state=args.seed,
        n_jobs=-1,
    )

    print(f"Running {args.folds}-fold subject-grouped CV...")
    gkf = GroupKFold(n_splits=args.folds)
    y_true_all, y_pred_all = [], []
    for fold_i, (tr, te) in enumerate(gkf.split(X, y, groups=groups)):
        rf.fit(X.iloc[tr], y.iloc[tr])
        y_hat = rf.predict(X.iloc[te])
        acc = accuracy_score(y.iloc[te], y_hat)
        print(f"  fold {fold_i}: n_test={len(te)} acc={acc:.3f} "
              f"test_subjects={sorted(set(groups.iloc[te]))}")
        y_true_all.append(y.iloc[te].values)
        y_pred_all.append(y_hat)
    y_true = np.concatenate(y_true_all)
    y_pred = np.concatenate(y_pred_all)

    print()
    print("=== Cross-validated 3-class results ===")
    print(f"accuracy = {accuracy_score(y_true, y_pred):.3f}")
    print(f"macro-F1 = {f1_score(y_true, y_pred, labels=ACTIONS, average='macro'):.3f}")
    print()
    cm = confusion_matrix(y_true, y_pred, labels=ACTIONS)
    print("confusion matrix (rows=true, cols=pred):")
    print("              " + "  ".join(f"{a:>9}" for a in ACTIONS))
    for i, a in enumerate(ACTIONS):
        row = "  ".join(f"{cm[i,j]:>9}" for j in range(len(ACTIONS)))
        print(f"{a:<12}  {row}")
    print()
    print(classification_report(y_true, y_pred, labels=ACTIONS, digits=3, zero_division=0))

    # Feature importance from a final fit.
    rf.fit(X, y)
    imp = pd.Series(rf.feature_importances_, index=feature_cols).sort_values(ascending=False)
    print("=== Feature importance (full-data fit) ===")
    for name, v in imp.items():
        bar = "#" * int(round(v * 80))
        print(f"  {name:<12} {v:.4f}  {bar}")

    if args.save_model:
        from joblib import dump
        dump(rf, args.save_model)
        print(f"\nmodel saved: {args.save_model}")


if __name__ == "__main__":
    main()
