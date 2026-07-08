"""Replay test_pro3's emg_pipeline + posture quality scoring against KneE-PAD
to estimate how the firmware's per-rep classifier would perform on real squats.

Each KneE-PAD trial is already segmented to ~3 s = one rep. We treat the whole
trial as the descent->bottom->ascent of one squat. The IMU axis with the
largest swing during the trial is taken as the "thigh tilt" proxy; the EMG
channel with the highest variance is taken as the "single-channel" EMG that
mirrors the on-board ADS1292R input.

Output:
  - Confusion matrix:  true KneE-PAD label  vs  firmware quality label
  - Per-label feature distributions
  - Counts of each failure mode the firmware would have raised

Caveat: KneE-PAD's Squat_WT (weight transfer) and Squat_FL (injured leg in
front) are *bilateral* errors that are hard to detect from a single EMG / IMU.
The on-board firmware will most likely score these as "OK"-ish on depth and
timing alone. Use this evaluation to set expectations -- the firmware can
catch depth/bounce/asymmetric-tempo errors but probably won't catch postural
asymmetry errors.

Usage:
    python python/evaluate_on_kneepad.py --root D:\\datasets\\dataset
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

try:
    from scipy.signal import sosfilt
    HAS_SCIPY = True
except ImportError:
    HAS_SCIPY = False

# --- Sampling rates (Delsys Trigno standard) ------------------------------
EMG_FS = 1259.0   # Hz
IMU_FS = 148.0    # Hz, per-axis (8 sensors * 6 axes = 48 ch)

# Target rates matching the firmware.
TARGET_EMG_FS = 500.0
TARGET_IMU_FS = 50.0

# --- emg_pipeline biquads (must match emg_pipeline.c) ---------------------
BIQUAD_COEFS_500 = [
    dict(b0=0.8370891906,  b1=-1.6741783811, b2=0.8370891906,
         a1=-1.6474599811, a2=0.7008967812),  # HPF 20 Hz
    dict(b0=0.9902986180,  b1=-1.6023368229, b2=0.9902986180,
         a1=-1.6023368229, a2=0.9805972359),  # Notch 50 Hz
    dict(b0=0.3913357725,  b1=0.7826715450,  b2=0.3913357725,
         a1=0.3695273774,  a2=0.1958157127),  # LPF 150 Hz
]

# --- Quality thresholds (kept in sync with main.c) ------------------------
DESCENT_MIN_MS = 500
DESCENT_MAX_MS = 4000
ASCENT_MIN_MS  = 500
ASCENT_MAX_MS  = 3000
BOTTOM_MIN_MS  = 0
BOTTOM_MAX_MS  = 2000
SYMMETRY_MIN   = 0.40
BOTTOM_ANGLE_DEG = 20.0
START_ANGLE_DEG  = 5.0
FORCE_RATIO    = 0.70


def biquad_cascade(x, coefs):
    """Apply biquad cascade. Uses scipy.signal.sosfilt when available (10x+
    faster), falls back to a pure-Python DF2T loop otherwise."""
    if HAS_SCIPY:
        # Each row in sos is [b0, b1, b2, 1.0, a1, a2].
        sos = np.array([[c["b0"], c["b1"], c["b2"], 1.0, c["a1"], c["a2"]]
                        for c in coefs], dtype=np.float64)
        return sosfilt(sos, x.astype(np.float64))

    y = np.empty_like(x, dtype=np.float64)
    states = [(0.0, 0.0) for _ in coefs]
    for n in range(len(x)):
        v = float(x[n])
        for i, c in enumerate(coefs):
            s1, s2 = states[i]
            out = c["b0"] * v + s1
            s1 = c["b1"] * v - c["a1"] * out + s2
            s2 = c["b2"] * v - c["a2"] * out
            states[i] = (s1, s2)
            v = out
        y[n] = v
    return y


def resample_linear(x, src_fs, dst_fs):
    """Linear-interpolation resampler. Simple, no anti-aliasing -- adequate
    here because Delsys EMG/IMU outputs are already bandlimited well below
    the target Nyquist."""
    n_in = len(x)
    dur = (n_in - 1) / src_fs
    n_out = int(round(dur * dst_fs)) + 1
    t_in  = np.linspace(0, dur, n_in)
    t_out = np.linspace(0, dur, n_out)
    return np.interp(t_out, t_in, x)


def pick_active_emg_channel(emg):
    """Channel with highest std after a coarse DC removal -- most likely the
    quadriceps or the active muscle for this exercise."""
    detrended = emg - emg.mean(axis=1, keepdims=True)
    return int(detrended.std(axis=1).argmax())


def pick_thigh_imu(imu):
    """Return the IMU sensor index (0..7) whose accel tilt swing is largest
    during the trial. Accel triplets occupy channels [s*6 .. s*6+2]; gyro
    triplets occupy [s*6+3 .. s*6+5]."""
    best_idx = 0
    best_swing = -1.0
    for s in range(8):
        ax = imu[s * 6 + 0]
        ay = imu[s * 6 + 1]
        az = imu[s * 6 + 2]
        # |g| is ~1 because Delsys accel is in g.
        tilt = np.degrees(np.arctan2(ax, np.sqrt(ay * ay + az * az)))
        swing = tilt.max() - tilt.min()
        if swing > best_swing:
            best_swing = swing
            best_idx = s
    return best_idx, best_swing


def compute_angle_track(imu, sensor_idx):
    ax = imu[sensor_idx * 6 + 0]
    ay = imu[sensor_idx * 6 + 1]
    az = imu[sensor_idx * 6 + 2]
    tilt = np.degrees(np.arctan2(ax, np.sqrt(ay * ay + az * az)))
    # Express as |tilt - tilt[0]| so a standing-baseline trial starts at 0.
    return tilt - tilt[0]


def detect_rep_phases(angle_deg, fs):
    """Decompose a one-rep trial into descend/bottom/ascend phases.

    Strategy:
      - Find the time index of |peak angle| (~bottom of the squat).
      - Walk back from peak while angle is rising (descent phase).
      - Walk forward while angle is dropping (ascent phase).
      - Anything in between (within 10% of peak) is "bottom dwell".

    Returns (descent_ms, bottom_ms, ascent_ms, peak_angle_deg).
    """
    abs_ang = np.abs(angle_deg)
    if len(abs_ang) < 5:
        return 0, 0, 0, 0.0
    peak_idx = int(abs_ang.argmax())
    peak = float(abs_ang[peak_idx])

    # Bottom region: contiguous span around peak where |ang| >= 0.9 * peak.
    threshold = 0.9 * peak
    left = peak_idx
    while left > 0 and abs_ang[left - 1] >= threshold:
        left -= 1
    right = peak_idx
    while right < len(abs_ang) - 1 and abs_ang[right + 1] >= threshold:
        right += 1

    descent_n = left            # 0..left is descent
    bottom_n  = right - left + 1
    ascent_n  = len(abs_ang) - 1 - right

    ms = lambda n: int(round(1000.0 * n / fs))
    return ms(descent_n), ms(bottom_n), ms(ascent_n), peak


def emg_window_features(filt):
    abs_x = np.abs(filt)
    mav = float(abs_x.mean())
    rms = float(np.sqrt((filt.astype(np.float64) ** 2).mean()))
    wl  = float(np.abs(np.diff(filt)).sum())
    return mav, rms, wl


def zc_ssc_count(filt, zc_th, ssc_th):
    """Vectorized ZC + SSC count."""
    a = filt[:-1]
    b = filt[1:]
    zc_mask = (np.abs(a) > zc_th) & (np.abs(b) > zc_th) & ((a > 0) != (b > 0)) & (a != 0) & (b != 0)
    zc = int(zc_mask.sum())

    if len(filt) >= 3:
        d = np.diff(filt)
        d1 = d[:-1]
        d2 = d[1:]
        sign_change = ((d1 > 0) & (d2 < 0)) | ((d1 < 0) & (d2 > 0))
        ssc_mask = sign_change & ((np.abs(d1) > ssc_th) | (np.abs(d2) > ssc_th))
        ssc = int(ssc_mask.sum())
    else:
        ssc = 0
    return zc, ssc


def score_quality(peak_deg, dn_ms, bot_ms, up_ms, mean_mav, mean_wl, base_mav):
    """Mirror motion_score_quality() in main.c. Returns (score, fail_mask, label)."""
    score = 0
    mask = 0

    # depth
    if peak_deg >= BOTTOM_ANGLE_DEG: score += 1
    else:                            mask |= 0x01  # DEPTH_LOW

    # descent time
    if DESCENT_MIN_MS <= dn_ms <= DESCENT_MAX_MS:
        score += 1
    elif dn_ms < DESCENT_MIN_MS:
        mask |= 0x02  # BOUNCE
    else:
        mask |= 0x10  # SLOW

    # ascent time
    if ASCENT_MIN_MS <= up_ms <= ASCENT_MAX_MS:
        score += 1
    elif up_ms < ASCENT_MIN_MS:
        mask |= 0x02
    else:
        mask |= 0x10

    # bottom dwell
    if BOTTOM_MIN_MS <= bot_ms <= BOTTOM_MAX_MS:
        score += 1
    elif bot_ms < BOTTOM_MIN_MS:
        mask |= 0x02

    # symmetry
    lo = min(dn_ms, up_ms); hi = max(dn_ms, up_ms)
    if hi > 0 and (lo / hi) >= SYMMETRY_MIN:
        score += 1
    else:
        mask |= 0x04  # ASYMMETRIC

    # force
    if base_mav > 0 and mean_mav >= base_mav * FORCE_RATIO:
        score += 1
    elif base_mav > 0:
        mask |= 0x08  # WEAK
    else:
        score += 1   # no baseline: give benefit of doubt (matches firmware)

    # mean WL positive
    if mean_wl > 0:
        score += 1

    if score == 7: label = "GOOD"
    elif score >= 5: label = "OK"
    elif score >= 3: label = "WEAK"
    else: label = "WRONG"

    return score, mask, label


# --- Walk dataset ----------------------------------------------------------

def trials(root: Path, labels=("0", "1", "2")):
    for sub in sorted(root.glob("Subject_*"), key=lambda p: int(p.name.split("_")[1])):
        for lab in labels:
            ldir = sub / lab
            if not ldir.is_dir():
                continue
            for tdir in sorted(ldir.glob("Trial_*"), key=lambda p: int(p.name.split("_")[1])):
                emg_p = tdir / "emg.npy"
                imu_p = tdir / "imu.npy"
                if emg_p.exists() and imu_p.exists():
                    yield sub.name, lab, tdir.name, emg_p, imu_p


def per_trial(emg_path, imu_path, base_mav=None):
    emg = np.load(emg_path)   # (8, N_emg)
    imu = np.load(imu_path)   # (48, N_imu)

    ch = pick_active_emg_channel(emg)
    raw = emg[ch].astype(np.float64)

    # Resample EMG 1259 -> 500 Hz.
    emg500 = resample_linear(raw, EMG_FS, TARGET_EMG_FS)

    # Filter through the same biquad cascade as the firmware.
    filt = biquad_cascade(emg500, BIQUAD_COEFS_500)

    mav, rms, wl = emg_window_features(filt)

    # Derive sensible ZC/SSC thresholds from filt itself for the offline run.
    sigma = float(filt.std())
    zc_th = max(0.001, sigma * 0.5)
    ssc_th = max(0.001, sigma * 0.7)
    zc, ssc = zc_ssc_count(filt, zc_th, ssc_th)

    # Angle via best-tilt-swing IMU.
    sensor_idx, swing = pick_thigh_imu(imu)
    angle_full = compute_angle_track(imu, sensor_idx)
    # Resample IMU 148 -> 50 Hz (informational only; phase detection works
    # at either rate, but matches firmware view).
    angle_ds = resample_linear(angle_full, IMU_FS, TARGET_IMU_FS)
    dn_ms, bot_ms, up_ms, peak_deg = detect_rep_phases(angle_ds, TARGET_IMU_FS)

    score, mask, label = score_quality(
        peak_deg, dn_ms, bot_ms, up_ms,
        mean_mav=mav, mean_wl=wl,
        base_mav=base_mav if base_mav else 0.0,
    )

    return {
        "ch": ch, "imu": sensor_idx, "swing": float(swing),
        "peak_deg": float(peak_deg),
        "descent_ms": dn_ms, "bottom_ms": bot_ms, "ascent_ms": up_ms,
        "mav": mav, "rms": rms, "wl": wl, "zc": zc, "ssc": ssc,
        "score": score, "mask": mask, "label": label,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=r"D:\datasets\dataset",
                    help="Path to extracted KneE-PAD root (the folder containing Subject_1..)")
    ap.add_argument("--subjects", type=int, default=0,
                    help="Limit to first N subjects (0 = all)")
    args = ap.parse_args()

    root = Path(args.root)
    if not root.exists():
        raise SystemExit(f"dataset root not found: {root}")

    # Pass 1: per-subject baseline MAV from label-0 trials (acts as the
    # firmware's active-baseline estimate).
    print("Pass 1: subject baselines from label-0 squats...")
    base_by_subject = {}
    seen_subjects = set()
    by_subject_label0 = defaultdict(list)
    for sub, lab, trial, emg_p, imu_p in trials(root, labels=("0",)):
        if args.subjects and len(seen_subjects) >= args.subjects and sub not in seen_subjects:
            continue
        seen_subjects.add(sub)
        f = per_trial(emg_p, imu_p, base_mav=None)
        by_subject_label0[sub].append(f["mav"])

    for sub, mavs in by_subject_label0.items():
        base_by_subject[sub] = statistics.median(mavs) if mavs else 0.0
    print(f"  built baselines for {len(base_by_subject)} subjects")

    # Pass 2: score all label 0/1/2 trials.
    print("Pass 2: scoring squat trials (labels 0/1/2)...")
    rows = []
    n = 0
    for sub, lab, trial, emg_p, imu_p in trials(root, labels=("0", "1", "2")):
        if args.subjects and sub not in seen_subjects:
            continue
        base = base_by_subject.get(sub, 0.0)
        f = per_trial(emg_p, imu_p, base_mav=base)
        f["subject"] = sub
        f["trial"]   = trial
        f["true"]    = {"0": "CORRECT", "1": "Squat_WT", "2": "Squat_FL"}[lab]
        rows.append(f)
        n += 1
        if n % 200 == 0:
            print(f"  ...{n}")
    print(f"total trials scored: {n}")

    # Aggregate.
    conf = defaultdict(Counter)
    fail_by_true = defaultdict(Counter)
    for r in rows:
        conf[r["true"]][r["label"]] += 1
        for bit, name in [(1,"DEPTH_LOW"), (2,"BOUNCE"), (4,"ASYMMETRIC"),
                          (8,"WEAK"), (16,"SLOW")]:
            if r["mask"] & bit:
                fail_by_true[r["true"]][name] += 1

    print()
    print("=== Confusion matrix (rows = true KneE-PAD label, cols = firmware verdict) ===")
    labels_pred = ["GOOD", "OK", "WEAK", "WRONG"]
    print(f"{'true \\ pred':<14} | " + "  ".join(f"{l:>6}" for l in labels_pred) + " |   N")
    for t in ("CORRECT", "Squat_WT", "Squat_FL"):
        total = sum(conf[t].values())
        if total == 0:
            continue
        cells = "  ".join(f"{conf[t][p]:>6}" for p in labels_pred)
        print(f"{t:<14} | {cells} | {total:>4}")

    print()
    print("=== Failure modes raised (per true label) ===")
    for t in ("CORRECT", "Squat_WT", "Squat_FL"):
        total = sum(conf[t].values())
        if total == 0: continue
        print(f"  {t}:")
        for name in ("DEPTH_LOW", "BOUNCE", "ASYMMETRIC", "WEAK", "SLOW"):
            cnt = fail_by_true[t][name]
            pct = 100.0 * cnt / total if total else 0.0
            print(f"    {name:<11} {cnt:>5} ({pct:5.1f}%)")

    print()
    print("=== Feature distribution by true label ===")
    for t in ("CORRECT", "Squat_WT", "Squat_FL"):
        subset = [r for r in rows if r["true"] == t]
        if not subset:
            continue
        def stats(field):
            xs = [r[field] for r in subset]
            return (statistics.median(xs),
                    (sorted(xs)[max(0, len(xs)*5//100)]),
                    (sorted(xs)[min(len(xs)-1, len(xs)*95//100)]))
        med_d,  p5_d,  p95_d  = stats("descent_ms")
        med_b,  p5_b,  p95_b  = stats("bottom_ms")
        med_u,  p5_u,  p95_u  = stats("ascent_ms")
        med_p,  p5_p,  p95_p  = stats("peak_deg")
        med_m,  p5_m,  p95_m  = stats("mav")
        print(f"  {t} (n={len(subset)})")
        print(f"    descent_ms median/p5/p95 = {med_d:.0f} / {p5_d:.0f} / {p95_d:.0f}")
        print(f"    bottom_ms  median/p5/p95 = {med_b:.0f} / {p5_b:.0f} / {p95_b:.0f}")
        print(f"    ascent_ms  median/p5/p95 = {med_u:.0f} / {p5_u:.0f} / {p95_u:.0f}")
        print(f"    peak_deg   median/p5/p95 = {med_p:.1f} / {p5_p:.1f} / {p95_p:.1f}")
        print(f"    mav        median/p5/p95 = {med_m:.5f} / {p5_m:.5f} / {p95_m:.5f}")

    # Save per-trial CSV for later analysis.
    out = Path(__file__).resolve().parent / "kneepad_squat_scores.csv"
    with out.open("w", encoding="utf-8") as f:
        f.write("subject,trial,true,ch,imu,peak_deg,descent_ms,bottom_ms,ascent_ms,"
                "mav,rms,wl,zc,ssc,score,mask,pred\n")
        for r in rows:
            f.write(f"{r['subject']},{r['trial']},{r['true']},{r['ch']},{r['imu']},"
                    f"{r['peak_deg']:.2f},{r['descent_ms']},{r['bottom_ms']},{r['ascent_ms']},"
                    f"{r['mav']:.6f},{r['rms']:.6f},{r['wl']:.6f},{r['zc']},{r['ssc']},"
                    f"{r['score']},{r['mask']},{r['label']}\n")
    print()
    print(f"per-trial CSV written: {out}")


if __name__ == "__main__":
    main()
