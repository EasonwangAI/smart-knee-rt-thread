from __future__ import annotations

import argparse
from pathlib import Path

import pandas as pd
from joblib import dump
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import classification_report, confusion_matrix
from sklearn.model_selection import GroupKFold


ROOT = Path(__file__).resolve().parent
DEFAULT_CSV = ROOT / "features" / "features_lg0_binary_1s_hop0p5.csv"
DEFAULT_MODEL = ROOT / "fatigue_rf_lg0.joblib"

META_COLS = {
    "source_file",
    "state",
    "fatigue_label",
    "condition",
    "subject",
    "trial",
    "window_index",
    "start_sample",
    "end_sample",
    "start_s",
    "end_s",
    "fs_hz",
    "n_channels",
    "n_samples",
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", default=str(DEFAULT_CSV))
    parser.add_argument("--model-out", default=str(DEFAULT_MODEL))
    parser.add_argument("--folds", type=int, default=5)
    parser.add_argument("--n-estimators", type=int, default=400)
    parser.add_argument("--max-depth", type=int, default=12)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--no-save", action="store_true")
    args = parser.parse_args()

    csv_path = Path(args.csv)
    df = pd.read_csv(csv_path)
    if df.empty:
        raise SystemExit(f"empty CSV: {csv_path}")

    feature_cols = [c for c in df.columns if c not in META_COLS]
    X = df[feature_cols]
    y = df["fatigue_label"]
    groups = df["source_file"]

    n_groups = groups.nunique()
    n_splits = min(args.folds, n_groups)
    if n_splits < 2:
        raise SystemExit("need at least two trials/groups for grouped validation")

    clf = RandomForestClassifier(
        n_estimators=args.n_estimators,
        max_depth=args.max_depth,
        class_weight="balanced",
        random_state=args.seed,
        n_jobs=-1,
    )

    print(f"CSV: {csv_path}")
    print(f"rows={len(df)} features={len(feature_cols)} groups={n_groups}")
    print("label counts:", y.value_counts().to_dict())
    print()

    cv = GroupKFold(n_splits=n_splits)
    y_true_all = []
    y_pred_all = []
    for fold, (train_idx, test_idx) in enumerate(cv.split(X, y, groups), start=1):
        clf.fit(X.iloc[train_idx], y.iloc[train_idx])
        pred = clf.predict(X.iloc[test_idx])
        y_true_all.extend(y.iloc[test_idx].tolist())
        y_pred_all.extend(pred.tolist())
        acc = (pred == y.iloc[test_idx].to_numpy()).mean()
        test_groups = sorted(set(groups.iloc[test_idx]))
        print(f"fold {fold}: acc={acc:.3f}, test_groups={test_groups}")

    print()
    print("confusion matrix, rows=true [Ideal=0, Fatigue=1], cols=pred")
    print(confusion_matrix(y_true_all, y_pred_all, labels=[0, 1]))
    print()
    print(classification_report(
        y_true_all,
        y_pred_all,
        labels=[0, 1],
        target_names=["Ideal", "Fatigue"],
        digits=3,
        zero_division=0,
    ))

    clf.fit(X, y)
    if not args.no_save:
        out = Path(args.model_out)
        dump({"model": clf, "feature_cols": feature_cols}, out)
        print(f"model saved: {out}")


if __name__ == "__main__":
    main()
