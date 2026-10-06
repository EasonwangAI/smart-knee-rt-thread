from __future__ import annotations

import csv
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FEATURES = [
    "log_rms",
    "log_mav",
    "log_wl_per_sample",
    "rms_mav_ratio",
    "zc_density",
    "ssc_density",
    "angle_range_common",
    "active_ratio",
]
WEIGHTS = {
    "log_rms": 1.4,
    "log_mav": 1.2,
    "log_wl_per_sample": 1.0,
    "rms_mav_ratio": 0.8,
    "zc_density": 0.5,
    "ssc_density": 0.5,
    "angle_range_common": 0.6,
    "active_ratio": 0.3,
}


def f(row, key, default=0.0):
    try:
        v = row.get(key, default)
        if v in ("", None):
            return default
        return float(v)
    except Exception:
        return default


def safe_log(x):
    return math.log10(max(abs(x), 1e-12))


def safe_div(a, b):
    return 0.0 if abs(b) < 1e-12 else a / b


def derive(query):
    row_count = f(query, "row_count", 1.0) or 1.0
    rms = f(query, "rms_mean")
    mav = f(query, "mav_mean")
    wl = f(query, "wl_mean")
    zc = f(query, "zc_mean")
    ssc = f(query, "ssc_mean")
    angle = f(query, "angle_range_common")
    if not angle:
        angle = f(query, "angle_x10_range") / 10.0
    return {
        "log_rms": safe_log(rms),
        "log_mav": safe_log(mav),
        "log_wl_per_sample": safe_log(safe_div(wl, row_count)),
        "rms_mav_ratio": safe_div(rms, mav),
        "zc_density": safe_div(zc, row_count),
        "ssc_density": safe_div(ssc, row_count),
        "angle_range_common": angle,
        "active_ratio": f(query, "active_ratio"),
    }


def load_rows():
    with (ROOT / "runtime_units_normalized.csv").open("r", encoding="utf-8-sig", newline="") as fp:
        return list(csv.DictReader(fp))


def load_stats():
    return json.loads((ROOT / "normalization_stats.json").read_text(encoding="utf-8"))


def normalize_query(query_features, stats, schema="legacy_rtthread_features"):
    schema_stats = stats[schema]
    return {
        feat: (query_features[feat] - schema_stats[feat]["mean"]) / (schema_stats[feat]["std"] or 1.0)
        for feat in FEATURES
    }


def match(query, mode="device_core", top_k=5):
    rows = load_rows()
    stats = load_stats()
    q = normalize_query(derive(query), stats)

    if mode == "device_core":
        rows = [r for r in rows if r["source_family"] == "local_logged"]
    elif mode == "external_only":
        rows = [r for r in rows if r["source_family"] == "external_public"]
    elif mode == "mixed_shape":
        pass
    else:
        raise ValueError("mode must be device_core, mixed_shape, or external_only")

    scored = []
    for row in rows:
        dist = 0.0
        used = 0.0
        for feat in FEATURES:
            w = WEIGHTS[feat]
            rv = f(row, "z_" + feat)
            dist += w * (q[feat] - rv) ** 2
            used += w
        dist = math.sqrt(dist / used) if used else 999.0
        scored.append((dist, row))
    scored.sort(key=lambda x: x[0])
    return scored[:top_k]


def main():
    if len(sys.argv) < 2:
        print("Usage: python match_query.py query_example.json [device_core|mixed_shape|external_only] [top_k]")
        sys.exit(1)
    query_path = Path(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) >= 3 else "device_core"
    top_k = int(sys.argv[3]) if len(sys.argv) >= 4 else 5
    query = json.loads(query_path.read_text(encoding="utf-8"))
    for rank, (dist, row) in enumerate(match(query, mode, top_k), start=1):
        print(
            f"{rank}. distance={dist:.4f} "
            f"source={row['source_family']} person={row['person_id']} "
            f"unit={row['unit_id']} label={row['label']} fatigue={row['fatigue_level']}"
        )


if __name__ == "__main__":
    main()
