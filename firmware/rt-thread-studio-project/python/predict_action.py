"""Run the trained RandomForest action classifier on a single .npy trial,
mimicking how on-device inference would work after sliding-window feature
extraction.

Usage:
    python python/predict_action.py D:\\datasets\\dataset\\Subject_1\\0\\Trial_1
    python python/predict_action.py --csv path/to/your_recording.csv
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd
from joblib import load

# Reuse the feature extractor from training.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from train_action_classifier import (
    extract_features,
    LABEL_TO_ACTION,
)


MODEL_PATH = Path(__file__).resolve().parent / "kneepad_action_rf.joblib"

FEATURE_COLS = [
    "mav", "rms", "wl", "iemg", "var", "zc", "ssc", "mnf", "mdf",
    "swing", "tilt_std", "tilt_p95", "tilt_zc",
    "gyro_mean", "gyro_max", "gyro_std", "acc_std",
]


def predict_trial_dir(trial_dir: Path, model):
    feat = extract_features(trial_dir / "emg.npy", trial_dir / "imu.npy")
    x = pd.DataFrame([{k: feat[k] for k in FEATURE_COLS}])
    pred = model.predict(x)[0]
    proba = dict(zip(model.classes_, model.predict_proba(x)[0]))
    return pred, proba, feat


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trial_dir", help="Path to a KneE-PAD Trial_N directory")
    ap.add_argument("--model", default=str(MODEL_PATH))
    args = ap.parse_args()

    model = load(args.model)

    tdir = Path(args.trial_dir)
    pred, proba, feat = predict_trial_dir(tdir, model)

    print(f"trial:       {tdir}")
    print(f"emg_channel: {feat['emg_ch']}  imu_sensor: {feat['imu_idx']}")
    print()
    print(f"prediction:  {pred}")
    print(f"probabilities:")
    for cls, p in sorted(proba.items(), key=lambda x: -x[1]):
        bar = "#" * int(round(p * 50))
        print(f"  {cls:<10} {p:.3f}  {bar}")
    print()

    # If we can identify the ground truth from the path, show it.
    try:
        raw_lab = tdir.parent.name
        if raw_lab in LABEL_TO_ACTION:
            truth = LABEL_TO_ACTION[raw_lab]
            ok = "OK" if truth == pred else "MISS"
            print(f"ground truth (from path): {truth} (KneE-PAD label {raw_lab})  [{ok}]")
    except Exception:
        pass


if __name__ == "__main__":
    main()
