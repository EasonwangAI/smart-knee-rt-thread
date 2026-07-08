"""Train a fatigue classifier using ONLY EMG features (no IMU).

Use this when IMU data is unreliable (e.g., device not worn on body).
Only uses the 7 EMG time-domain features that the firmware computes.

Usage:
    python train_fatigue_emg_only.py session_*_features.csv --save-model fatigue_emg.joblib
"""
from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import (accuracy_score, classification_report,
                              confusion_matrix, f1_score, roc_auc_score)
from sklearn.model_selection import GroupKFold, train_test_split

# ONLY EMG features - no IMU (gyro/acc/tilt)
EMG_ONLY_FEATURES = [
    "mav",   # Mean Absolute Value
    "rms",   # Root Mean Square
    "wl",    # Waveform Length
    "iemg",  # Integrated EMG
    "var",   # Variance
    "zc",    # Zero Crossings
    "ssc",   # Slope Sign Changes
]


def load_and_merge_csvs(paths):
    dfs = []
    for p in paths:
        if not p.exists():
            print(f"WARNING: {p} not found, skipping", file=sys.stderr)
            continue
        df = pd.read_csv(p)
        dfs.append(df)
    if not dfs:
        raise SystemExit("ERROR: no valid CSV files found")
    merged = pd.concat(dfs, ignore_index=True)
    return merged


def prepare_fatigue_dataset(df: pd.DataFrame):
    df = df[df["label"].isin(["FreshSquat", "FatiguedSquat"])].copy()
    if len(df) == 0:
        raise SystemExit("ERROR: no FreshSquat or FatiguedSquat windows found.")
    df["fatigue"] = (df["label"] == "FatiguedSquat").astype(int)
    df["group"] = df["session"]
    return df


def add_emg_ratios(df: pd.DataFrame):
    """Add EMG-only derived features that may help distinguish fatigue."""
    df = df.copy()

    # Frequency-to-amplitude ratios (fatigue shifts frequency content)
    df["zc_per_mav"] = df["zc"] / (df["mav"] + 1e-9)
    df["ssc_per_rms"] = df["ssc"] / (df["rms"] + 1e-9)

    # RMS/MAV ratio (muscle fiber recruitment pattern)
    df["rms_mav_ratio"] = df["rms"] / (df["mav"] + 1e-9)

    # Waveform complexity
    df["wl_per_iemg"] = df["wl"] / (df["iemg"] + 1e-9)

    # Variance normalized by amplitude
    df["var_per_rms2"] = df["var"] / ((df["rms"] ** 2) + 1e-9)

    return df


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+", help="one or more *_features.csv files")
    ap.add_argument("--folds", type=int, default=5)
    ap.add_argument("--n-estimators", type=int, default=200)
    ap.add_argument("--max-depth", type=int, default=10)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--save-model", default="")
    ap.add_argument("--add-ratios", action="store_true",
                    help="add derived EMG ratio features")
    args = ap.parse_args()

    paths = []
    for pattern in args.csvs:
        if "*" in pattern:
            paths.extend(Path(".").glob(pattern))
        else:
            paths.append(Path(pattern))

    print(f"Loading {len(paths)} CSV file(s)...")
    df = load_and_merge_csvs(paths)
    print(f"  total rows: {len(df)}")

    df = prepare_fatigue_dataset(df)
    print(f"  Fresh/Fatigued rows: {len(df)}")
    print(f"  class distribution: {Counter(df['label']).most_common()}")
    print(f"  sessions: {df['session'].nunique()}")

    if args.add_ratios:
        df = add_emg_ratios(df)
        feature_cols = EMG_ONLY_FEATURES + [
            "zc_per_mav", "ssc_per_rms", "rms_mav_ratio",
            "wl_per_iemg", "var_per_rms2",
        ]
        print(f"  using {len(feature_cols)} features (7 base + 5 derived)")
    else:
        feature_cols = EMG_ONLY_FEATURES
        print(f"  using {len(feature_cols)} EMG-only features")

    missing = [c for c in feature_cols if c not in df.columns]
    if missing:
        raise SystemExit(f"ERROR: missing feature columns: {missing}")

    out_csv = Path("fatigue_emg_features.csv")
    df.to_csv(out_csv, index=False)
    print(f"\nSaved dataset: {out_csv}")

    X = df[feature_cols]
    y = df["fatigue"]
    groups = df["group"]

    X = X.replace([np.inf, -np.inf], np.nan).fillna(0)

    rf = RandomForestClassifier(
        n_estimators=args.n_estimators,
        max_depth=args.max_depth,
        class_weight="balanced",
        random_state=args.seed,
        n_jobs=-1,
    )

    print(f"\nRunning {args.folds}-fold session-grouped CV...")
    if df["session"].nunique() < args.folds:
        print(f"WARNING: only {df['session'].nunique()} sessions, "
              f"using simple train/test split instead of {args.folds}-fold CV")
        X_train, X_test, y_train, y_test = train_test_split(
            X, y, test_size=0.25, random_state=args.seed, stratify=y)
        rf.fit(X_train, y_train)
        y_pred = rf.predict(X_test)
        y_true = y_test.values
        y_prob = rf.predict_proba(X_test)[:, 1]
    else:
        gkf = GroupKFold(n_splits=args.folds)
        y_true_all, y_pred_all, y_prob_all = [], [], []
        for fold_i, (tr, te) in enumerate(gkf.split(X, y, groups=groups)):
            rf.fit(X.iloc[tr], y.iloc[tr])
            y_hat = rf.predict(X.iloc[te])
            y_proba = rf.predict_proba(X.iloc[te])[:, 1]
            acc = accuracy_score(y.iloc[te], y_hat)
            test_sessions = sorted(set(groups.iloc[te]))
            print(f"  fold {fold_i}: n_test={len(te)} acc={acc:.3f} "
                  f"sessions={test_sessions}")
            y_true_all.append(y.iloc[te].values)
            y_pred_all.append(y_hat)
            y_prob_all.append(y_proba)
        y_true = np.concatenate(y_true_all)
        y_pred = np.concatenate(y_pred_all)
        y_prob = np.concatenate(y_prob_all)

    print("\n" + "=" * 60)
    print("EMG-ONLY FATIGUE CLASSIFICATION (Fresh=0, Fatigued=1)")
    print("=" * 60)
    acc = accuracy_score(y_true, y_pred)
    f1 = f1_score(y_true, y_pred)
    auc = roc_auc_score(y_true, y_prob)
    print(f"Accuracy:  {acc:.3f}")
    print(f"F1-score:  {f1:.3f}")
    print(f"ROC AUC:   {auc:.3f}")
    print()

    cm = confusion_matrix(y_true, y_pred)
    print("Confusion Matrix (rows=true, cols=pred):")
    print("              Fresh  Fatigued")
    print(f"Fresh         {cm[0,0]:>5}  {cm[0,1]:>8}")
    print(f"Fatigued      {cm[1,0]:>5}  {cm[1,1]:>8}")
    print()

    print(classification_report(y_true, y_pred,
                                target_names=["Fresh", "Fatigued"],
                                digits=3, zero_division=0))

    rf.fit(X, y)
    importances = pd.Series(rf.feature_importances_,
                           index=feature_cols).sort_values(ascending=False)
    print("=" * 60)
    print("FEATURE IMPORTANCE (EMG-only)")
    print("=" * 60)
    for name, imp in importances.items():
        bar = "#" * int(round(imp * 60))
        print(f"  {name:<16} {imp:.4f}  {bar}")

    if args.save_model:
        from joblib import dump
        dump(rf, args.save_model)
        print(f"\nModel saved: {args.save_model}")
        print(f"  Features used: {feature_cols}")
        print(f"  Classes: 0=Fresh, 1=Fatigued")
        print(f"  Note: EMG-only model (no IMU features)")
    else:
        print("\nTo save the model, use --save-model <path.joblib>")


if __name__ == "__main__":
    main()
