"""Train the target 3-class action model: Squat / Deadlift / Walking.

This is the model the knee-brace project actually needs. It should be trained
from our own monitor.py logs after running validate_action_model.py to extract
window-level features.

Example:
    python validate_action_model.py session_a.log session_b.log --out-dir D:\\mpu_wave\\action_train
    python train_action_squat_deadlift_walking.py D:\\mpu_wave\\action_train\\action_validation_features.csv

Output:
    D:\\mpu_wave\\action_squat_deadlift_walking_rf.joblib
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd
from joblib import dump
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix, f1_score
from sklearn.model_selection import GroupKFold, train_test_split


OUT_MODEL = Path(r"D:\mpu_wave\action_squat_deadlift_walking_rf.joblib")
TARGET_ACTIONS = ["Squat", "Deadlift", "Walking"]

FEATURE_COLS = [
    "mav", "rms", "wl", "iemg", "var", "zc", "ssc",
    "swing", "tilt_std", "tilt_p95", "tilt_zc",
    "gyro_mean", "gyro_max", "gyro_std", "acc_std",
]


def load_features(paths):
    dfs = []
    for p in paths:
        path = Path(p)
        if not path.exists():
            print(f"WARNING: missing file: {path}")
            continue
        dfs.append(pd.read_csv(path))
    if not dfs:
        raise SystemExit("no valid feature CSV files")
    return pd.concat(dfs, ignore_index=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+", help="CSV files produced by validate_action_model.py")
    ap.add_argument("--save-model", default=str(OUT_MODEL))
    ap.add_argument("--n-estimators", type=int, default=200)
    ap.add_argument("--max-depth", type=int, default=10)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    df = load_features(args.csvs)
    missing = [c for c in FEATURE_COLS + ["true_action"] if c not in df.columns]
    if missing:
        raise SystemExit(f"missing required columns: {missing}")

    df = df[df["true_action"].isin(TARGET_ACTIONS)].copy()
    if df.empty:
        raise SystemExit("no Squat / Deadlift / Walking rows found")

    counts = df["true_action"].value_counts()
    absent = [a for a in TARGET_ACTIONS if a not in counts]
    if absent:
        raise SystemExit(f"missing target action data: {absent}. Collect these labels first.")
    if counts.min() < 20:
        print(f"WARNING: smallest class has only {counts.min()} windows; accuracy will be unstable.")

    X = df[FEATURE_COLS].replace([np.inf, -np.inf], np.nan).fillna(0)
    y = df["true_action"]
    groups = df["source"] if "source" in df.columns else None

    print("Target action dataset")
    print(f"  rows: {len(df)}")
    print(f"  class distribution: {counts.to_dict()}")
    print()

    rf_kwargs = dict(
        n_estimators=args.n_estimators,
        max_depth=args.max_depth,
        class_weight="balanced",
        random_state=args.seed,
        n_jobs=-1,
    )

    y_true_all = []
    y_pred_all = []
    group_count = int(groups.nunique()) if groups is not None else 0
    if groups is not None and group_count >= 3:
        n_splits = min(5, group_count)
        print(f"Grouped CV by source log: {n_splits} folds")
        splitter = GroupKFold(n_splits=n_splits)
        for fold_i, (tr, te) in enumerate(splitter.split(X, y, groups=groups)):
            model = RandomForestClassifier(**rf_kwargs)
            model.fit(X.iloc[tr], y.iloc[tr])
            pred = model.predict(X.iloc[te])
            print(f"  fold {fold_i}: n_test={len(te)} acc={accuracy_score(y.iloc[te], pred):.3f}")
            y_true_all.extend(y.iloc[te])
            y_pred_all.extend(pred)
    else:
        print("Simple stratified train/test split")
        x_tr, x_te, y_tr, y_te = train_test_split(
            X, y, test_size=0.25, stratify=y, random_state=args.seed
        )
        model = RandomForestClassifier(**rf_kwargs)
        model.fit(x_tr, y_tr)
        pred = model.predict(x_te)
        y_true_all.extend(y_te)
        y_pred_all.extend(pred)

    print()
    print("Validation result")
    print(f"  accuracy: {accuracy_score(y_true_all, y_pred_all):.3f}")
    print(f"  macro-F1: {f1_score(y_true_all, y_pred_all, labels=TARGET_ACTIONS, average='macro'):.3f}")
    print("  confusion matrix rows=true, cols=pred:")
    print(confusion_matrix(y_true_all, y_pred_all, labels=TARGET_ACTIONS))
    print()
    print(classification_report(y_true_all, y_pred_all, labels=TARGET_ACTIONS, digits=3, zero_division=0))

    final_model = RandomForestClassifier(**rf_kwargs)
    final_model.fit(X, y)

    out = Path(args.save_model)
    out.parent.mkdir(parents=True, exist_ok=True)
    dump(final_model, out)
    print(f"saved model: {out}")
    print(f"classes: {list(final_model.classes_)}")
    print(f"n_features: {final_model.n_features_in_}")


if __name__ == "__main__":
    main()
