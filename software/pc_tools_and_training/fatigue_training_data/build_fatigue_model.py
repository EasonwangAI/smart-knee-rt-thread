from __future__ import annotations

import json
import shutil
from pathlib import Path

import numpy as np
import pandas as pd
from joblib import dump
from sklearn.ensemble import RandomForestClassifier
from sklearn.impute import SimpleImputer
from sklearn.metrics import classification_report, confusion_matrix
from sklearn.model_selection import GroupKFold
from sklearn.pipeline import make_pipeline


ROOT = Path(__file__).resolve().parent
SOURCE_DIR = ROOT / "source_csv"
FEATURE_DIR = ROOT / "features"
MODEL_DIR = ROOT / "models"
REPORT_DIR = ROOT / "reports"

DESKTOP_INPUTS = [
    SOURCE_DIR / "P03_fatigue_clean_features.csv",
    SOURCE_DIR / "P05_fatigue_clean_features.csv",
]

EXISTING_INPUT = FEATURE_DIR / "features_lg0_binary_1s_hop0p5.csv"

COMMON_METRICS = ["rms", "mav", "wl", "mdf", "mpf"]
CHANNELS = [f"ch{i:02d}" for i in range(1, 14)]
META_COLS = [
    "dataset",
    "source_file",
    "subject",
    "state",
    "fatigue_label",
    "condition",
    "speed",
    "trial",
    "window_index",
    "start_sample",
    "end_sample",
    "window_sec",
    "hop_sec",
    "fs_hz",
    "n_channels",
    "sample_count",
    "group_id",
]


def ensure_dirs() -> None:
    for path in (SOURCE_DIR, FEATURE_DIR, MODEL_DIR, REPORT_DIR):
        path.mkdir(parents=True, exist_ok=True)


def copy_inputs() -> list[Path]:
    copied = []
    for src in DESKTOP_INPUTS:
        if not src.exists():
            raise FileNotFoundError(src)
        dst = SOURCE_DIR / src.name.replace("(1)", "")
        if not dst.exists():
            shutil.copy2(src, dst)
        copied.append(dst)
    return copied


def get_col(df: pd.DataFrame, *names: str, default=np.nan):
    for name in names:
        if name in df.columns:
            return df[name]
    return pd.Series([default] * len(df), index=df.index)


def channel_metric(df: pd.DataFrame, ch: str, metric: str) -> pd.Series:
    # Two known source schemas:
    #   clean CSV: ch01_rms
    #   generated CSV: rms_ch01
    a = f"{ch}_{metric}"
    b = f"{metric}_{ch}"
    if a in df.columns:
        return df[a]
    if b in df.columns:
        return df[b]
    return pd.Series([np.nan] * len(df), index=df.index)


def canonicalize_clean_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    out = pd.DataFrame(index=df.index)
    out["dataset"] = path.stem
    out["source_file"] = path.name
    out["subject"] = get_col(df, "subject", default=path.stem.split("_")[0])
    out["state"] = get_col(df, "condition")
    out["fatigue_label"] = get_col(df, "label").astype(int)
    out["condition"] = get_col(df, "ambulation", default="LG_0")
    out["speed"] = get_col(df, "speed", default="S_1")
    out["trial"] = get_col(df, "trial")
    out["window_index"] = get_col(df, "window_idx", default=0).astype(int)
    out["start_sample"] = get_col(df, "start_sample", default=0).astype(int)
    out["end_sample"] = get_col(df, "end_sample", default=0).astype(int)
    out["window_sec"] = get_col(df, "window_sec", default=2.0).astype(float)
    out["hop_sec"] = get_col(df, "step_sec", default=1.0).astype(float)
    out["fs_hz"] = (
        get_col(df, "window_samples", default=4000).astype(float)
        / out["window_sec"].replace(0, np.nan)
    ).round().astype(int)
    out["n_channels"] = get_col(df, "channel_count", default=13).astype(int)
    out["sample_count"] = get_col(df, "sample_count", default=np.nan)
    add_canonical_features(df, out)
    finish_features(out)
    return out


def canonicalize_generated_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    out = pd.DataFrame(index=df.index)
    out["dataset"] = path.stem
    out["source_file"] = get_col(df, "source_file", default=path.name)
    out["subject"] = get_col(df, "subject", default="P03")
    out["state"] = get_col(df, "state")
    out["fatigue_label"] = get_col(df, "fatigue_label").astype(int)
    out["condition"] = get_col(df, "condition", default="LG_0")
    out["speed"] = "S_1"
    out["trial"] = get_col(df, "trial")
    out["window_index"] = get_col(df, "window_index", default=0).astype(int)
    out["start_sample"] = get_col(df, "start_sample", default=0).astype(int)
    out["end_sample"] = get_col(df, "end_sample", default=0).astype(int)
    out["window_sec"] = get_col(df, "end_s", default=1.0) - get_col(df, "start_s", default=0.0)
    out["hop_sec"] = np.nan
    out["fs_hz"] = get_col(df, "fs_hz", default=2000).astype(int)
    out["n_channels"] = get_col(df, "n_channels", default=13).astype(int)
    out["sample_count"] = np.nan
    add_canonical_features(df, out)
    finish_features(out)
    return out


def add_canonical_features(src: pd.DataFrame, out: pd.DataFrame) -> None:
    for ch in CHANNELS:
        for metric in COMMON_METRICS:
            out[f"{ch}_{metric}"] = channel_metric(src, ch, metric)


def finish_features(out: pd.DataFrame) -> None:
    rms_cols = [f"{ch}_rms" for ch in CHANNELS]
    rms_values = out[rms_cols].to_numpy(dtype=float)
    best_idx = np.nanargmax(np.where(np.isnan(rms_values), -np.inf, rms_values), axis=1)

    for metric in COMMON_METRICS:
        cols = [f"{ch}_{metric}" for ch in CHANNELS]
        values = out[cols].astype(float)
        out[f"{metric}_mean"] = values.mean(axis=1)
        out[f"{metric}_std"] = values.std(axis=1)
        out[f"{metric}_median"] = values.median(axis=1)
        out[f"{metric}_min"] = values.min(axis=1)
        out[f"{metric}_max"] = values.max(axis=1)
        single = []
        arr = values.to_numpy(dtype=float)
        for row_i, ch_i in enumerate(best_idx):
            single.append(arr[row_i, ch_i])
        out[f"single_{metric}"] = single

    out["single_channel"] = [CHANNELS[i] for i in best_idx]
    out["group_id"] = (
        out["dataset"].astype(str)
        + "|"
        + out["subject"].astype(str)
        + "|"
        + out["state"].astype(str)
        + "|"
        + out["condition"].astype(str)
        + "|"
        + out["trial"].astype(str)
    )


def evaluate_grouped(df: pd.DataFrame, feature_cols: list[str], title: str) -> tuple[str, object]:
    X = df[feature_cols]
    y = df["fatigue_label"].astype(int)
    groups = df["group_id"]
    n_splits = min(5, groups.nunique())

    model = make_pipeline(
        SimpleImputer(strategy="median"),
        RandomForestClassifier(
            n_estimators=500,
            max_depth=10,
            min_samples_leaf=4,
            class_weight="balanced",
            random_state=42,
            n_jobs=-1,
        ),
    )

    y_true_all: list[int] = []
    y_pred_all: list[int] = []
    lines = [f"=== {title} ==="]
    lines.append(f"rows={len(df)} features={len(feature_cols)} groups={groups.nunique()}")
    lines.append(f"label_counts={y.value_counts().to_dict()}")

    if n_splits >= 2:
        cv = GroupKFold(n_splits=n_splits)
        for fold, (train_idx, test_idx) in enumerate(cv.split(X, y, groups), start=1):
            model.fit(X.iloc[train_idx], y.iloc[train_idx])
            pred = model.predict(X.iloc[test_idx])
            y_true = y.iloc[test_idx].to_numpy()
            acc = float((pred == y_true).mean())
            y_true_all.extend(y_true.tolist())
            y_pred_all.extend(pred.tolist())
            lines.append(f"fold {fold}: acc={acc:.3f}, test_groups={len(set(groups.iloc[test_idx]))}")

        lines.append("confusion matrix rows=true [Ideal=0, Fatigue=1], cols=pred")
        lines.append(str(confusion_matrix(y_true_all, y_pred_all, labels=[0, 1])))
        lines.append(classification_report(
            y_true_all,
            y_pred_all,
            labels=[0, 1],
            target_names=["Ideal", "Fatigue"],
            digits=3,
            zero_division=0,
        ))
    else:
        lines.append("not enough groups for grouped CV")

    model.fit(X, y)
    return "\n".join(lines), model


def main() -> None:
    ensure_dirs()
    clean_paths = copy_inputs()

    clean_parts = [canonicalize_clean_csv(path) for path in clean_paths]
    clean_df = pd.concat(clean_parts, ignore_index=True)
    clean_out = FEATURE_DIR / "combined_clean_p03_p05_2s.csv"
    clean_df.to_csv(clean_out, index=False)

    all_parts = clean_parts[:]
    if EXISTING_INPUT.exists():
        all_parts.append(canonicalize_generated_csv(EXISTING_INPUT))
    all_df = pd.concat(all_parts, ignore_index=True)
    all_out = FEATURE_DIR / "combined_all_common_features.csv"
    all_df.to_csv(all_out, index=False)

    single_feature_cols = [f"single_{metric}" for metric in COMMON_METRICS]
    aggregate_feature_cols = [
        f"{metric}_{stat}"
        for metric in COMMON_METRICS
        for stat in ("mean", "std", "median", "min", "max")
    ]
    model_feature_cols = single_feature_cols + aggregate_feature_cols

    clean_report, clean_model = evaluate_grouped(
        clean_df, model_feature_cols, "P03/P05 clean 2s fatigue model"
    )
    all_report, all_model = evaluate_grouped(
        all_df, model_feature_cols, "All common-feature fatigue model"
    )

    clean_model_path = MODEL_DIR / "fatigue_rf_p03_p05_clean_2s.joblib"
    all_model_path = MODEL_DIR / "fatigue_rf_all_common.joblib"

    dump(
        {
            "model": clean_model,
            "feature_cols": model_feature_cols,
            "source_csv": str(clean_out),
            "label_map": {"Ideal": 0, "Fatigue": 1},
            "note": "Primary PC-side fatigue model trained on clean P03/P05 2s windows.",
        },
        clean_model_path,
    )
    dump(
        {
            "model": all_model,
            "feature_cols": model_feature_cols,
            "source_csv": str(all_out),
            "label_map": {"Ideal": 0, "Fatigue": 1},
            "note": "Exploratory model trained on clean P03/P05 plus generated P03 common features.",
        },
        all_model_path,
    )

    report = "\n\n".join([clean_report, all_report])
    report_path = REPORT_DIR / "fatigue_training_report.txt"
    report_path.write_text(report, encoding="utf-8")

    summary = {
        "clean_csv": str(clean_out),
        "all_csv": str(all_out),
        "primary_model": str(clean_model_path),
        "all_model": str(all_model_path),
        "feature_cols": model_feature_cols,
        "clean_rows": int(len(clean_df)),
        "all_rows": int(len(all_df)),
    }
    (REPORT_DIR / "fatigue_training_summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    print(report)
    print("\nSaved:")
    print(f"  {clean_out}")
    print(f"  {all_out}")
    print(f"  {clean_model_path}")
    print(f"  {all_model_path}")
    print(f"  {report_path}")


if __name__ == "__main__":
    main()
