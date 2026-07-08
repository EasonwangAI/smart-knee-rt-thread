"""Train a sklearn random forest on KneE-PAD per-trial features.

The point: my single-channel firmware can't reliably distinguish Squat_WT/FL
from CORRECT with threshold rules (98% of all classes scored GOOD/OK in the
threshold-based confusion matrix). The question is whether a learned model
on the *same* per-trial features extracts enough signal to do better than
chance, especially in a subject-independent setting.

Critical methodological choice: GroupKFold by subject. If we shuffle trials
across subjects we leak per-subject baselines into the model and overstate
accuracy. Real deployment on a new patient should match the cross-subject
generalization estimate.

Usage:
    python python/train_squat_rf.py [--csv path] [--folds 5]
"""
from __future__ import annotations

import argparse
import statistics
from collections import Counter
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import (classification_report, confusion_matrix,
                              f1_score, accuracy_score)
from sklearn.model_selection import GroupKFold

DEFAULT_CSV = Path(__file__).resolve().parent / "kneepad_squat_scores.csv"


def add_engineered_features(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()

    # Symmetry ratio (matches main.c's quality criterion).
    lo = np.minimum(df["descent_ms"], df["ascent_ms"])
    hi = np.maximum(df["descent_ms"], df["ascent_ms"])
    df["symmetry"] = lo / np.where(hi == 0, 1, hi)
    df["tempo"]    = df["descent_ms"] + df["ascent_ms"] + df["bottom_ms"]
    df["dn_up_ratio"] = df["descent_ms"] / np.where(df["ascent_ms"] == 0, 1, df["ascent_ms"])

    # Per-subject normalized amplitude features. This mirrors the active
    # baseline calibration on the board: divide by the subject's median
    # CORRECT MAV so we compare like-with-like across users.
    subj_base = (df[df["true"] == "CORRECT"]
                 .groupby("subject")["mav"].median()
                 .rename("subj_base_mav"))
    df = df.join(subj_base, on="subject")
    df["mav_n"] = df["mav"] / df["subj_base_mav"].replace(0, np.nan)
    df["rms_n"] = df["rms"] / df["subj_base_mav"].replace(0, np.nan)
    df["wl_n"]  = df["wl"]  / df["subj_base_mav"].replace(0, np.nan)
    df.fillna({"mav_n": 0, "rms_n": 0, "wl_n": 0}, inplace=True)

    return df


def eval_fold(model, X, y, groups, folds):
    gkf = GroupKFold(n_splits=folds)
    y_true_all, y_pred_all = [], []
    for tr, te in gkf.split(X, y, groups=groups):
        model.fit(X.iloc[tr], y.iloc[tr])
        y_hat = model.predict(X.iloc[te])
        y_true_all.append(y.iloc[te].values)
        y_pred_all.append(y_hat)
    return np.concatenate(y_true_all), np.concatenate(y_pred_all)


def report_block(title, y_true, y_pred, classes):
    print()
    print(f"=== {title} ===")
    acc = accuracy_score(y_true, y_pred)
    macro_f1 = f1_score(y_true, y_pred, labels=classes, average="macro")
    print(f"accuracy = {acc:.3f}    macro-F1 = {macro_f1:.3f}")
    print()
    print("confusion matrix (rows=true, cols=pred):")
    cm = confusion_matrix(y_true, y_pred, labels=classes)
    header = "          " + "  ".join(f"{c:>9}" for c in classes)
    print(header)
    for i, c in enumerate(classes):
        row = "  ".join(f"{cm[i, j]:>9}" for j in range(len(classes)))
        print(f"{c:<10}{row}")
    print()
    print(classification_report(y_true, y_pred, labels=classes, digits=3, zero_division=0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=str(DEFAULT_CSV))
    ap.add_argument("--folds", type=int, default=5)
    ap.add_argument("--n-estimators", type=int, default=400)
    ap.add_argument("--max-depth", type=int, default=10)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    df = pd.read_csv(args.csv)
    df = add_engineered_features(df)

    print(f"loaded {len(df)} rows, {df['subject'].nunique()} subjects")
    print(f"class distribution: {Counter(df['true']).most_common()}")

    feature_cols = [
        # geometric / temporal
        "peak_deg", "descent_ms", "bottom_ms", "ascent_ms",
        "symmetry", "tempo", "dn_up_ratio",
        # EMG amplitude (per-subject normalized to be cross-user-comparable)
        "mav_n", "rms_n", "wl_n",
        # EMG amplitude raw (absolute level may also carry info)
        "mav", "rms", "wl",
        # EMG frequency content
        "zc", "ssc",
    ]
    X = df[feature_cols]
    y = df["true"]
    groups = df["subject"]

    classes_3 = ["CORRECT", "Squat_WT", "Squat_FL"]

    rf = RandomForestClassifier(
        n_estimators=args.n_estimators,
        max_depth=args.max_depth,
        class_weight="balanced",
        random_state=args.seed,
        n_jobs=-1,
    )

    print(f"\nRunning {args.folds}-fold subject-grouped CV with RandomForest"
          f"(n_estimators={args.n_estimators}, max_depth={args.max_depth})...")

    y_true, y_pred = eval_fold(rf, X, y, groups, args.folds)
    report_block("3-class: CORRECT / Squat_WT / Squat_FL", y_true, y_pred, classes_3)

    # Binary: CORRECT vs WRONG (combine WT+FL).
    y_bin = y.where(y == "CORRECT", "WRONG")
    y_true_b, y_pred_b = eval_fold(rf, X, y_bin, groups, args.folds)
    report_block("Binary: CORRECT vs WRONG", y_true_b, y_pred_b, ["CORRECT", "WRONG"])

    # Feature importance from a final fit on all data.
    rf.fit(X, y)
    importances = pd.Series(rf.feature_importances_, index=feature_cols).sort_values(ascending=False)
    print()
    print("=== Feature importance (final fit on all data) ===")
    for name, imp in importances.items():
        bar = "#" * int(round(imp * 80))
        print(f"  {name:<14} {imp:.4f}  {bar}")

    # Save a small comparison vs the firmware threshold scorer.
    fw = df["pred"]
    fw_3 = fw.where(fw.isin(["GOOD", "OK", "WEAK", "WRONG"]), "OTHER")
    # Map firmware's quality labels to a binary "wrong-ish" view:
    # GOOD/OK -> CORRECT-ish, WEAK/WRONG -> WRONG-ish
    fw_bin = np.where(fw.isin(["GOOD", "OK"]), "CORRECT", "WRONG")
    print()
    print("=== Firmware threshold scorer, same trials, binary view ===")
    report_block("Firmware threshold (binary)", y_bin.values, fw_bin, ["CORRECT", "WRONG"])


if __name__ == "__main__":
    main()
