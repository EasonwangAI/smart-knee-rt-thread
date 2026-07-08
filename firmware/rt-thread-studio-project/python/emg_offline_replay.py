"""Replay the new emg_pipeline algorithms offline against ads1292_capture.csv.

This script mirrors the C pipeline in emg_pipeline.c exactly enough to validate
algorithm correctness and parameter choices before flashing the board.

Important caveat: the CSV was produced by the *old* firmware which only printed
every 5th sample at an effective ~250 SPS (instead of the configured 500 SPS).
So we cannot perfectly reconstruct the input to the new pipeline. Two modes:

  1. "as-is"   -- treat the CSV stream as 500 SPS directly. Will under-sample
                  features but otherwise faithfully shows what the new pipeline
                  produces on the available data.
  2. "upsample"-- repeat each printed sample 5x to fake the lost samples and
                  approximate the original 500 SPS cadence. Better for sanity
                  checking the filter cutoffs.

Run: python python/emg_offline_replay.py [--mode upsample|as-is]
"""
import argparse
import csv
import math
import statistics
from pathlib import Path
from collections import Counter

# --- Biquad coefficients (must match emg_pipeline.c) -----------------------

BIQUAD_COEFS = [
    # HPF 20 Hz, Q=0.7071
    dict(b0=0.8370891906, b1=-1.6741783811, b2=0.8370891906,
         a1=-1.6474599811, a2=0.7008967812),
    # Notch 50 Hz, Q=30
    dict(b0=0.9902986180, b1=-1.6023368229, b2=0.9902986180,
         a1=-1.6023368229, a2=0.9805972359),
    # LPF 150 Hz, Q=0.7071
    dict(b0=0.3913357725, b1=0.7826715450, b2=0.3913357725,
         a1=0.3695273774, a2=0.1958157127),
]

SAMPLE_RATE         = 500
WINDOW              = 256
HOP                 = 64
REST_CAL_WINDOWS    = 80
ACTIVE_CAL_WINDOWS  = 40
ACTIVE_RATIO        = 2.5
ACTIVE_FLOOR        = 3000
ZC_FLOOR, ZC_CEIL   = 200, 20000
SSC_FLOOR, SSC_CEIL = 150, 20000

FAT_THRESHOLD = 70
FAT_HOLD      = 3

CSV_PATH = Path(__file__).resolve().parent.parent / "ads1292_capture.csv"


def biquad_apply(state, c, x):
    y = c["b0"] * x + state[0]
    state[0] = c["b1"] * x - c["a1"] * y + state[1]
    state[1] = c["b2"] * x - c["a2"] * y
    return y


def cascade(states, coefs, x):
    y = x
    for s, c in zip(states, coefs):
        y = biquad_apply(s, c, y)
    return y


def compute_ssc(ring, threshold):
    n = len(ring)
    if n < 3:
        return 0
    count = 0
    for i in range(1, n - 1):
        d1 = ring[i]   - ring[i - 1]
        d2 = ring[i + 1] - ring[i]
        if (d1 > 0 and d2 < 0) or (d1 < 0 and d2 > 0):
            if abs(d1) > threshold or abs(d2) > threshold:
                count += 1
    return count


def replay(samples):
    states = [[0.0, 0.0] for _ in BIQUAD_COEFS]

    ac_ring   = [0] * WINDOW
    abs_ring  = [0] * WINDOW
    diff_ring = [0] * WINDOW
    zc_ring   = [0] * WINDOW
    pos = 0
    valid = 0
    hop_count = 0
    prev_ac = 0

    sum_sq = sum_abs = sum_diff = sum_zc = 0

    phase = "REST_CAL"
    status = "CAL"
    alert_hold = 0

    rest_buf = dict(rms=[], mav=[], wl=[], zc=[], ssc=[], var_sum=0, var_n=0)
    rest_done = None  # filled with median dict when ready

    active_buf = dict(rms=[], mav=[], wl=[], zc=[], ssc=[])
    base_done = None

    zc_threshold = ZC_FLOOR
    ssc_threshold = SSC_FLOOR
    active_threshold = ACTIVE_FLOOR

    out_rows = []  # one per window evaluation
    out_filt = []  # one per filtered sample (filtered ac)

    for x in samples:
        y = cascade(states, BIQUAD_COEFS, float(x))
        ac = int(y)
        out_filt.append(ac)

        abs_ac = abs(ac)
        cur_diff = ac - prev_ac
        abs_diff = abs(cur_diff)

        this_zc = 0
        if valid > 0:
            if (abs_ac > zc_threshold and abs(prev_ac) > zc_threshold and
                ((ac > 0 and prev_ac < 0) or (ac < 0 and prev_ac > 0))):
                this_zc = 1

        if valid == WINDOW:
            o_ac, o_abs, o_dif, o_zc = ac_ring[pos], abs_ring[pos], diff_ring[pos], zc_ring[pos]
            sum_sq   -= o_ac * o_ac
            sum_abs  -= o_abs
            sum_diff -= o_dif
            sum_zc   -= o_zc
        else:
            valid += 1

        ac_ring[pos] = ac
        abs_ring[pos] = abs_ac
        diff_ring[pos] = abs_diff
        zc_ring[pos] = this_zc

        sum_sq   += ac * ac
        sum_abs  += abs_ac
        sum_diff += abs_diff
        sum_zc   += this_zc

        pos = (pos + 1) % WINDOW
        prev_ac = ac

        hop_count += 1
        if hop_count < HOP or valid < WINDOW:
            continue
        hop_count = 0

        rms = int(math.sqrt(sum_sq / WINDOW)) if sum_sq > 0 else 0
        mav = sum_abs // WINDOW
        wl  = sum_diff
        zc  = sum_zc

        # Reconstruct chronological ring for SSC.
        chrono = ac_ring[pos:] + ac_ring[:pos]
        ssc = compute_ssc(chrono, ssc_threshold)

        active = 0
        score = 0
        alert = 0

        if phase == "REST_CAL":
            rest_buf["rms"].append(rms)
            rest_buf["mav"].append(mav)
            rest_buf["wl"].append(wl)
            rest_buf["zc"].append(zc)
            rest_buf["ssc"].append(ssc)
            rest_buf["var_sum"] += sum_sq
            rest_buf["var_n"]   += WINDOW
            if len(rest_buf["rms"]) >= REST_CAL_WINDOWS:
                rest_done = {
                    "rms": statistics.median(rest_buf["rms"]),
                    "mav": statistics.median(rest_buf["mav"]),
                    "wl":  statistics.median(rest_buf["wl"]),
                    "zc":  statistics.median(rest_buf["zc"]),
                    "ssc": statistics.median(rest_buf["ssc"]),
                }
                sigma = math.sqrt(rest_buf["var_sum"] / max(1, rest_buf["var_n"]))
                zc_threshold = int(max(ZC_FLOOR, min(ZC_CEIL, sigma * 3)))
                ssc_threshold = int(max(SSC_FLOOR, min(SSC_CEIL, sigma * 4)))
                active_threshold = int(max(ACTIVE_FLOOR, rest_done["mav"] * ACTIVE_RATIO))
                phase = "ACTIVE_CAL"
                print(f"[REPLAY] REST_CAL done: rest_mav={rest_done['mav']:.0f} "
                      f"rest_rms={rest_done['rms']:.0f} sigma={sigma:.0f} "
                      f"zc_th={zc_threshold} ssc_th={ssc_threshold} active_th={active_threshold}")
        elif phase == "ACTIVE_CAL":
            active = 1 if mav >= active_threshold else 0
            if active:
                active_buf["rms"].append(rms)
                active_buf["mav"].append(mav)
                active_buf["wl"].append(wl)
                active_buf["zc"].append(zc)
                active_buf["ssc"].append(ssc)
                if len(active_buf["rms"]) >= ACTIVE_CAL_WINDOWS:
                    base_done = {
                        "rms": max(1, statistics.median(active_buf["rms"])),
                        "mav": max(1, statistics.median(active_buf["mav"])),
                        "wl":  max(1, statistics.median(active_buf["wl"])),
                        "zc":  max(1, statistics.median(active_buf["zc"])),
                        "ssc": max(1, statistics.median(active_buf["ssc"])),
                    }
                    phase = "RUNNING"
                    print(f"[REPLAY] ACTIVE_CAL done: base_mav={base_done['mav']:.0f} "
                          f"base_rms={base_done['rms']:.0f} base_wl={base_done['wl']:.0f}")
        else:  # RUNNING
            active = 1 if mav >= active_threshold else 0
            if active and base_done is not None:
                rms_pct = rms * 100.0 / base_done["rms"]
                mav_pct = mav * 100.0 / base_done["mav"]
                wl_pct  = wl  * 100.0 / base_done["wl"]
                zc_pct  = zc  * 100.0 / base_done["zc"]

                rms_score  = max(0, min(30, (rms_pct - 110) * 30 / 50))
                mav_score  = max(0, min(20, (mav_pct - 110) * 20 / 50))
                wl_score   = max(0, min(20, (wl_pct  - 110) * 20 / 50))
                freq_score = max(0, min(30, (100 - zc_pct)  * 30 / 50))
                score = int(rms_score + mav_score + wl_score + freq_score)
                if score >= FAT_THRESHOLD:
                    alert_hold = min(FAT_HOLD, alert_hold + 1)
                else:
                    alert_hold = 0
            else:
                alert_hold = 0
            alert = 1 if alert_hold >= FAT_HOLD else 0

        out_rows.append(dict(
            sample_idx=len(out_filt),
            rms=rms, mav=mav, wl=wl, zc=zc, ssc=ssc,
            active=active, score=score, alert=alert,
            phase=phase,
            zc_th=zc_threshold,
            active_th=active_threshold,
        ))

    return dict(
        rows=out_rows,
        filt=out_filt,
        rest=rest_done,
        base=base_done,
        zc_th=zc_threshold,
        ssc_th=ssc_threshold,
        active_th=active_threshold,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["upsample", "as-is"], default="upsample",
                    help="upsample: repeat each printed sample 5x to fake 500 SPS")
    ap.add_argument("--csv", default=str(CSV_PATH))
    args = ap.parse_args()

    raw = []
    with open(args.csv, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for r in reader:
            try:
                raw.append(int(r["emg"]))
            except Exception:
                pass

    if args.mode == "upsample":
        samples = []
        for v in raw:
            samples.extend([v] * 5)
        print(f"[REPLAY] loaded {len(raw)} printed rows; upsampled to {len(samples)} fake-500SPS samples")
    else:
        samples = raw
        print(f"[REPLAY] loaded {len(raw)} samples (treated as 500 SPS directly)")

    res = replay(samples)

    rows = res["rows"]
    if not rows:
        print("No window evaluations produced -- not enough samples?")
        return

    print()
    print(f"window evaluations: {len(rows)}")
    print(f"calibration: rest={res['rest']}")
    print(f"             base={res['base']}")
    print(f"             zc_th={res['zc_th']} ssc_th={res['ssc_th']} active_th={res['active_th']}")

    running_rows = [r for r in rows if r["phase"] == "RUNNING"]
    print()
    print(f"RUNNING-phase rows: {len(running_rows)}")
    if running_rows:
        scores = [r["score"] for r in running_rows]
        print(f"  fatigue score: min={min(scores)}  max={max(scores)}  median={statistics.median(scores):.0f}")
        alerts = sum(1 for r in running_rows if r["alert"])
        print(f"  alert rows: {alerts} ({100*alerts/len(running_rows):.1f}%)")
        actives = sum(1 for r in running_rows if r["active"])
        print(f"  active rows: {actives} ({100*actives/len(running_rows):.1f}%)")
        rms_vals = [r["rms"] for r in running_rows]
        mav_vals = [r["mav"] for r in running_rows]
        wl_vals  = [r["wl"]  for r in running_rows]
        zc_vals  = [r["zc"]  for r in running_rows]
        print(f"  rms: min={min(rms_vals)} max={max(rms_vals)} median={statistics.median(rms_vals)}")
        print(f"  mav: min={min(mav_vals)} max={max(mav_vals)} median={statistics.median(mav_vals)}")
        print(f"  wl:  min={min(wl_vals)}  max={max(wl_vals)}  median={statistics.median(wl_vals)}")
        print(f"  zc:  min={min(zc_vals)}  max={max(zc_vals)}  median={statistics.median(zc_vals)}")

    # Phase distribution.
    phase_count = Counter(r["phase"] for r in rows)
    print()
    print("phase distribution (windows):")
    for k, v in phase_count.most_common():
        print(f"  {k:12s} {v}  ({100*v/len(rows):.1f}%)")


if __name__ == "__main__":
    main()
