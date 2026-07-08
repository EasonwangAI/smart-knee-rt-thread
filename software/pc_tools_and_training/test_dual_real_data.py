"""Test dual prediction on real session data.

Verifies the dual-function system on actual collected data.

Usage:
    python test_dual_real_data.py <features_csv> [<features_csv> ...]
"""
from __future__ import annotations

import argparse
import sys
import warnings
from collections import Counter
from pathlib import Path

import numpy as np
import pandas as pd
from joblib import load

warnings.filterwarnings('ignore', category=UserWarning)


def compute_derived_features(mav, rms, wl, iemg, var, zc, ssc):
    return [
        zc / (mav + 1e-9),
        ssc / (rms + 1e-9),
        rms / (mav + 1e-9),
        wl / (iemg + 1e-9),
        var / ((rms ** 2) + 1e-9),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+", help="Feature CSV files")
    args = ap.parse_args()

    # Load models
    action_model_path = Path(r"D:\mpu_wave\rt_thread_project\test_pro3\python\kneepad_action_rf.joblib")
    fatigue_model_path = Path(r"D:\mpu_wave\training_curated\fatigue_emg_balanced_sessions.joblib")

    print("Loading models...")
    action_model = load(action_model_path)
    fatigue_model = load(fatigue_model_path)
    print(f"  [OK] Action model")
    print(f"  [OK] Fatigue model")
    print()

    # Load all CSV files
    dfs = []
    for csv_path in args.csvs:
        p = Path(csv_path)
        if not p.exists():
            print(f"WARNING: {p} not found")
            continue
        dfs.append(pd.read_csv(p))
    df = pd.concat(dfs, ignore_index=True)

    print(f"Loaded {len(df)} total windows")
    print(f"True labels: {Counter(df['label']).most_common()}")
    print()

    # Function 1: Action classification (15 features)
    action_features = df[[
        'mav', 'rms', 'wl', 'iemg', 'var', 'zc', 'ssc',
        'swing', 'tilt_std', 'tilt_p95', 'tilt_zc',
        'gyro_mean', 'gyro_max', 'gyro_std', 'acc_std'
    ]].values

    action_preds = action_model.predict(action_features)

    # Function 2: Fatigue detection (12 features)
    fatigue_features_list = []
    for _, row in df.iterrows():
        derived = compute_derived_features(
            row['mav'], row['rms'], row['wl'],
            row['iemg'], row['var'], row['zc'], row['ssc']
        )
        fatigue_features_list.append([
            row['mav'], row['rms'], row['wl'], row['iemg'],
            row['var'], row['zc'], row['ssc'],
            derived[0], derived[1], derived[2], derived[3], derived[4]
        ])
    fatigue_features = np.array(fatigue_features_list)
    fatigue_preds = fatigue_model.predict(fatigue_features)
    fatigue_probs = fatigue_model.predict_proba(fatigue_features)[:, 1]
    fatigue_labels = np.where(fatigue_preds == 1, 'Fatigued', 'Fresh')

    # Report
    print("=" * 70)
    print("DUAL FUNCTION RESULTS")
    print("=" * 70)
    print()

    print("Function 1: Action Classification")
    print("-" * 70)
    print(f"Predictions: {Counter(action_preds).most_common()}")
    if 'Deadlift' not in [str(c) for c in action_model.classes_]:
        print("NOTE: current action model has no Deadlift class.")
    print()

    print("True Label vs Action Prediction:")
    crosstab = pd.crosstab(df['label'], action_preds)
    print(crosstab.to_string())
    print()

    print("Function 2: Fatigue Detection")
    print("-" * 70)
    print(f"Predictions: {Counter(fatigue_labels).most_common()}")
    print()

    # Performance: only on Fresh/Fatigued labeled data
    is_fatigue_label = df['label'].isin(['FreshSquat', 'FatiguedSquat'])
    true_fatigue = (df.loc[is_fatigue_label, 'label'] == 'FatiguedSquat').astype(int)
    pred_fatigue = fatigue_preds[is_fatigue_label]

    if len(true_fatigue) > 0:
        from sklearn.metrics import accuracy_score, f1_score, confusion_matrix
        acc = accuracy_score(true_fatigue, pred_fatigue)
        f1 = f1_score(true_fatigue, pred_fatigue, zero_division=0)
        cm = confusion_matrix(true_fatigue, pred_fatigue)

        print(f"Fatigue Detection Accuracy: {acc:.1%}")
        print(f"Fatigue Detection F1-score: {f1:.1%}")
        print()
        print("Confusion Matrix (true vs predicted):")
        print(f"              Pred Fresh  Pred Fatigued")
        print(f"True Fresh        {cm[0,0]:>4}        {cm[0,1]:>4}")
        print(f"True Fatigued     {cm[1,0]:>4}        {cm[1,1]:>4}")
    print()

    # Combined view
    print("=" * 70)
    print("Combined Output Distribution")
    print("=" * 70)
    combined = pd.DataFrame({
        'true_label': df['label'].values,
        'action': action_preds,
        'fatigue': fatigue_labels,
    })

    print()
    print("By True Label:")
    for true_label in sorted(combined['true_label'].unique()):
        sub = combined[combined['true_label'] == true_label]
        n = len(sub)
        action_counts = Counter(sub['action'])
        fatigue_counts = Counter(sub['fatigue'])

        print(f"\n  {true_label} ({n} windows):")
        print(f"    Actions:  ", end="")
        for k, v in action_counts.most_common():
            print(f"{k}={v} ({v/n:.0%})  ", end="")
        print()
        print(f"    Fatigue:  ", end="")
        for k, v in fatigue_counts.most_common():
            print(f"{k}={v} ({v/n:.0%})  ", end="")
        print()

    print()
    print("=" * 70)


if __name__ == "__main__":
    main()
