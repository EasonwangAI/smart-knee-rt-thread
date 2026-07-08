"""Off-line replay of the on-device EMG / motion model against the captured CSV.

This script:
  1) parses ads1292_capture.csv,
  2) replays the firmware's MAV/RMS/ZC and fatigue scoring on the recorded EMG,
  3) compares replay vs. firmware-reported values to flag inconsistencies,
  4) summarises behavioural soundness (active flag, force, phase, status).
"""

import csv
import math
import re
import statistics
import sys
from pathlib import Path

CSV_PATH = Path(__file__).with_name("ads1292_capture.csv")

# Firmware constants (must match main.c).
SAMPLE_RATE_HZ = 500
OUTPUT_DIV = 5                      # firmware prints every 5th sample
EMG_WINDOW_SAMPLES = 64
EMG_HOP_SAMPLES = 16
EMG_BASELINE_WINDOWS = 20           # rest
EMG_ACTIVE_BASELINE_WINDOWS = 20    # active
EMG_ACTIVE_RATIO_PERCENT = 180
EMG_ACTIVE_MIN_MAV = 3000
EMG_ZC_THRESHOLD = 2000
EMG_ZC_VALID_MIN = 3
EMG_FATIGUE_SCORE_THRESHOLD = 70
EMG_FATIGUE_HOLD_WINDOWS = 3

STATUS_RE = re.compile(
    r"^(?P<phase>[A-Z]+)/(?P<action>[A-Z_]+)/#(?P<rep>\d+)/"
    r"(?P<force>[A-Z_]+)/(?P<fatigue>[A-Z]+)/act=(?P<act>\d+)/"
    r"mav=(?P<mav>\d+)/base=(?P<base>\d+)/th=(?P<th>\d+)$"
)


def isqrt64(x):
    return int(math.isqrt(int(x)))


def replay(emg_samples):
    """Replay the per-sample + windowed pipeline. Returns list of window features."""
    dc = 0
    prev_ac = 0
    ring_ac = [0] * EMG_WINDOW_SAMPLES
    ring_abs = [0] * EMG_WINDOW_SAMPLES
    ring_zc = [0] * EMG_WINDOW_SAMPLES
    sum_sq = 0
    sum_abs = 0
    zc_count = 0
    pos = 0
    valid = 0
    sample_idx = 0
    hop = 0

    rest_windows = 0
    rest_rms_sum = rest_mav_sum = rest_zc_sum = 0
    rest_mav = rest_rms = rest_zc = 0
    zc_threshold = EMG_ZC_THRESHOLD
    active_threshold = EMG_ACTIVE_MIN_MAV

    base_windows = 0
    base_rms_sum = base_mav_sum = base_zc_sum = 0
    base_rms = base_mav = base_zc = 0

    alert_hold = 0
    out = []
    for raw in emg_samples:
        # DC tracker.
        dc += (raw - dc) >> 6
        ac = raw - dc
        abs_ac = abs(ac)

        zc = 0
        if (
            sample_idx > 0
            and abs_ac > zc_threshold
            and abs(prev_ac) > zc_threshold
            and ((ac > 0 and prev_ac < 0) or (ac < 0 and prev_ac > 0))
        ):
            zc = 1

        if valid == EMG_WINDOW_SAMPLES:
            sum_sq -= ring_ac[pos] * ring_ac[pos]
            sum_abs -= ring_abs[pos]
            zc_count -= ring_zc[pos]
        else:
            valid += 1

        ring_ac[pos] = ac
        ring_abs[pos] = abs_ac
        ring_zc[pos] = zc
        sum_sq += ac * ac
        sum_abs += abs_ac
        zc_count += zc

        pos = (pos + 1) % EMG_WINDOW_SAMPLES
        prev_ac = ac
        sample_idx += 1

        if valid < EMG_WINDOW_SAMPLES:
            continue

        hop += 1
        if hop < EMG_HOP_SAMPLES:
            continue
        hop = 0

        rms = isqrt64(sum_sq // EMG_WINDOW_SAMPLES)
        mav = sum_abs // EMG_WINDOW_SAMPLES
        zc_val = zc_count

        # Rest calibration phase.
        if rest_windows < EMG_BASELINE_WINDOWS:
            rest_rms_sum += rms
            rest_mav_sum += mav
            rest_zc_sum += zc_val
            rest_windows += 1
            if rest_windows == EMG_BASELINE_WINDOWS:
                rest_rms = rest_rms_sum // EMG_BASELINE_WINDOWS
                rest_mav = rest_mav_sum // EMG_BASELINE_WINDOWS
                rest_zc = rest_zc_sum // EMG_BASELINE_WINDOWS
                zc_threshold = max(EMG_ZC_THRESHOLD, rest_mav // 2)
                active_threshold = max(
                    EMG_ACTIVE_MIN_MAV,
                    rest_mav * EMG_ACTIVE_RATIO_PERCENT // 100,
                )
            out.append(
                dict(idx=sample_idx, rms=rms, mav=mav, zc=zc_val,
                     active=False, score=0, phase="REST_CAL",
                     base_rms=0, base_mav=0, base_zc=0,
                     rest_mav=rest_mav, active_th=active_threshold)
            )
            continue

        active = mav >= active_threshold

        if base_windows < EMG_ACTIVE_BASELINE_WINDOWS:
            phase_tag = "ACT_CAL"
            score = 0
            if active:
                base_rms_sum += rms
                base_mav_sum += mav
                base_zc_sum += zc_val
                base_windows += 1
                if base_windows == EMG_ACTIVE_BASELINE_WINDOWS:
                    base_rms = base_rms_sum // EMG_ACTIVE_BASELINE_WINDOWS
                    base_mav = base_mav_sum // EMG_ACTIVE_BASELINE_WINDOWS
                    base_zc = max(1, base_zc_sum // EMG_ACTIVE_BASELINE_WINDOWS)
            out.append(
                dict(idx=sample_idx, rms=rms, mav=mav, zc=zc_val,
                     active=active, score=0, phase=phase_tag,
                     base_rms=base_rms, base_mav=base_mav, base_zc=base_zc,
                     rest_mav=rest_mav, active_th=active_threshold)
            )
            continue

        if not active:
            alert_hold = 0
            phase_tag = "REST"
            score = 0
        else:
            rms_pct = rms * 100 // max(1, base_rms)
            mav_pct = mav * 100 // max(1, base_mav)
            rms_score = (rms_pct - 110) * 2
            mav_score = mav_pct - 110
            if base_zc >= EMG_ZC_VALID_MIN:
                zc_pct = zc_val * 100 // base_zc
                zc_score = (85 - zc_pct) * 2
            else:
                zc_score = 0

            def clamp(x, lo, hi):
                return max(lo, min(hi, x))

            score = clamp(rms_score, 0, 45) + clamp(mav_score, 0, 25) + clamp(zc_score, 0, 30)

            if score >= EMG_FATIGUE_SCORE_THRESHOLD:
                alert_hold = min(EMG_FATIGUE_HOLD_WINDOWS, alert_hold + 1)
            else:
                alert_hold = 0
            phase_tag = "ALERT" if alert_hold >= EMG_FATIGUE_HOLD_WINDOWS else "OK"

        out.append(
            dict(idx=sample_idx, rms=rms, mav=mav, zc=zc_val,
                 active=active, score=score, phase=phase_tag,
                 base_rms=base_rms, base_mav=base_mav, base_zc=base_zc,
                 rest_mav=rest_mav, active_th=active_threshold)
        )

    return out, dict(rest_mav=rest_mav, rest_rms=rest_rms, rest_zc=rest_zc,
                     zc_threshold=zc_threshold, active_threshold=active_threshold,
                     base_mav=base_mav, base_rms=base_rms, base_zc=base_zc)


def main():
    rows = []
    with CSV_PATH.open("r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for r in reader:
            try:
                r["seq"] = int(r["seq"])
                r["angle"] = int(r["angle_or_ch1"])
                r["emg"] = int(r["emg"])
                r["fatigue"] = int(r["fatigue"])
                r["rms"] = int(r["rms"])
                r["zc"] = int(r["zc"])
                r["host_time"] = float(r["host_time"])
            except Exception:
                continue
            m = STATUS_RE.match(r["status"])
            if not m:
                continue
            r.update({k: m.group(k) for k in ("phase", "action", "force", "fatigue_label")
                      if k != "fatigue_label"})
            r["fw_phase"] = m.group("phase")
            r["fw_action"] = m.group("action")
            r["fw_rep"] = int(m.group("rep"))
            r["fw_force"] = m.group("force")
            r["fw_fatigue_label"] = m.group("fatigue")
            r["fw_act"] = int(m.group("act"))
            r["fw_mav"] = int(m.group("mav"))
            r["fw_base"] = int(m.group("base"))
            r["fw_th"] = int(m.group("th"))
            rows.append(r)

    print(f"=== CSV ===")
    print(f"rows parsed: {len(rows)}")
    print(f"duration: {rows[-1]['host_time'] - rows[0]['host_time']:.2f} s host-time")
    print(f"seq range: {rows[0]['seq']} -> {rows[-1]['seq']} (delta {rows[-1]['seq']-rows[0]['seq']})")

    # The firmware prints every 5th sample. The CSV's row order corresponds to seq order.
    # Reconstruct the original 500 Hz EMG stream by repeating each printed sample 5 times.
    # This is an approximation since we don't have the intermediate 4 raw samples, but the
    # DC tracker + ring buffer should give us *similar* MAV/RMS/ZC trends.
    #
    # Better: the firmware-reported rms/mav/zc give us the GROUND TRUTH of what the on-device
    # algorithm produced. We compare its behaviour against the angle/phase/emg context.

    # 1) Angle range.
    angles = [r["angle"] for r in rows]
    print(f"\n=== angle (0.1 deg, signed) ===")
    print(f"min/max/mean: {min(angles)} / {max(angles)} / {statistics.mean(angles):.1f}")
    print(f"abs max: {max(abs(a) for a in angles)} (i.e. {max(abs(a) for a in angles)/10:.1f} deg-equiv)")

    # 2) EMG raw range -- this is the raw uV-ish value before DC removal.
    emgs = [r["emg"] for r in rows]
    print(f"\n=== emg raw ===")
    print(f"min/max/mean/stdev: {min(emgs)} / {max(emgs)} / {statistics.mean(emgs):.0f} / {statistics.pstdev(emgs):.0f}")

    # 3) Reported MAV/RMS/ZC distribution.
    print(f"\n=== firmware-reported features ===")
    print(f"rms min/max/median: {min(r['rms'] for r in rows)} / {max(r['rms'] for r in rows)} / {statistics.median(r['rms'] for r in rows)}")
    print(f"mav min/max/median: {min(r['fw_mav'] for r in rows)} / {max(r['fw_mav'] for r in rows)} / {statistics.median(r['fw_mav'] for r in rows)}")
    print(f"zc  min/max/median: {min(r['zc'] for r in rows)} / {max(r['zc'] for r in rows)} / {statistics.median(r['zc'] for r in rows)}")
    print(f"fatigue score min/max/median: {min(r['fatigue'] for r in rows)} / {max(r['fatigue'] for r in rows)} / {statistics.median(r['fatigue'] for r in rows)}")

    # 4) Phases and actions reached.
    phase_counts = {}
    for r in rows:
        phase_counts[r["fw_phase"]] = phase_counts.get(r["fw_phase"], 0) + 1
    print(f"\n=== motion phase distribution ===")
    for k, v in sorted(phase_counts.items(), key=lambda x: -x[1]):
        print(f"  {k:10s} {v}  ({100*v/len(rows):.1f}%)")

    rep_counts = {}
    for r in rows:
        rep_counts[r["fw_action"]] = rep_counts.get(r["fw_action"], 0) + 1
    print(f"\n=== action distribution ===")
    for k, v in sorted(rep_counts.items(), key=lambda x: -x[1]):
        print(f"  {k:10s} {v}  ({100*v/len(rows):.1f}%)")
    print(f"max rep_count reached: {max(r['fw_rep'] for r in rows)}")

    force_counts = {}
    for r in rows:
        force_counts[r["fw_force"]] = force_counts.get(r["fw_force"], 0) + 1
    print(f"\n=== force flag distribution ===")
    for k, v in force_counts.items():
        print(f"  {k:10s} {v}  ({100*v/len(rows):.1f}%)")

    fatigue_lbl_counts = {}
    for r in rows:
        fatigue_lbl_counts[r["fw_fatigue_label"]] = fatigue_lbl_counts.get(r["fw_fatigue_label"], 0) + 1
    print(f"\n=== fatigue label distribution ===")
    for k, v in fatigue_lbl_counts.items():
        print(f"  {k:10s} {v}  ({100*v/len(rows):.1f}%)")

    # 5) active flag consistency.
    act_counts = {0: 0, 1: 0}
    for r in rows:
        act_counts[r["fw_act"]] = act_counts.get(r["fw_act"], 0) + 1
    print(f"\n=== active flag ===")
    for k, v in act_counts.items():
        print(f"  active={k}: {v}  ({100*v/len(rows):.1f}%)")

    # 6) Baseline ('base') and threshold ('th') evolution -- are they stable?
    bases = [r["fw_base"] for r in rows]
    ths = [r["fw_th"] for r in rows]
    print(f"\n=== base_mav (active baseline) ===")
    print(f"unique values: {sorted(set(bases))[:10]}{'...' if len(set(bases))>10 else ''}")
    print(f"min/max: {min(bases)} / {max(bases)}")
    print(f"\n=== active threshold ===")
    print(f"unique values: {sorted(set(ths))}")

    # 7) Spot-check active flag against motion phase: should we really be active during STAND?
    stand_active = sum(1 for r in rows if r["fw_phase"] == "STAND" and r["fw_act"] == 1)
    stand_total  = sum(1 for r in rows if r["fw_phase"] == "STAND")
    print(f"\n=== STAND-phase activity (suspicious if active mostly =1) ===")
    if stand_total:
        print(f"  STAND rows: {stand_total}, of which active=1: {stand_active} ({100*stand_active/stand_total:.1f}%)")

    # 8) Score during STAND.
    scores_stand = [r["fatigue"] for r in rows if r["fw_phase"] == "STAND"]
    scores_motion = [r["fatigue"] for r in rows if r["fw_phase"] not in ("STAND", "CAL")]
    print(f"\n=== fatigue score by phase ===")
    if scores_stand:
        print(f"  STAND: mean={statistics.mean(scores_stand):.1f}, max={max(scores_stand)}")
    if scores_motion:
        print(f"  in-motion (DESCEND/BOTTOM/ASCEND): mean={statistics.mean(scores_motion):.1f}, max={max(scores_motion)}")

    # 9) Reconstruct an approximate 500 Hz EMG stream by 5x repeat and replay.
    print("\n=== replaying model on reconstructed EMG (5x repeat of every printed sample) ===")
    upsampled = []
    for r in rows:
        # repeat each printed sample 5 times to recover original 500Hz cadence.
        upsampled.extend([r["emg"]] * OUTPUT_DIV)
    replay_out, replay_summary = replay(upsampled)
    print(f"replay yielded {len(replay_out)} window decisions; replay calibration:")
    for k, v in replay_summary.items():
        print(f"  {k:18s} = {v}")

    # 10) Show first/last few rows where active=1 and compare context.
    print("\n=== sample rows where firmware reports active=0 (rest) ===")
    rest_rows = [r for r in rows if r["fw_act"] == 0][:5]
    for r in rest_rows:
        print(f"  seq={r['seq']} angle={r['angle']} emg={r['emg']:>9d} mav={r['fw_mav']:>7d} th={r['fw_th']:>7d} phase={r['fw_phase']} fatigue={r['fatigue']}")

    print("\n=== sample rows where firmware reports active=1 during STAND ===")
    sus = [r for r in rows if r["fw_act"] == 1 and r["fw_phase"] == "STAND"][:5]
    for r in sus:
        print(f"  seq={r['seq']} angle={r['angle']} emg={r['emg']:>9d} mav={r['fw_mav']:>7d} th={r['fw_th']:>7d} fatigue={r['fatigue']}")

    print("\n=== sample rows during DESCEND/ASCEND ===")
    motion_rows = [r for r in rows if r["fw_phase"] not in ("STAND", "CAL")][:10]
    for r in motion_rows:
        print(f"  seq={r['seq']} phase={r['fw_phase']:8s} angle={r['angle']:>5d} emg={r['emg']:>9d} mav={r['fw_mav']:>7d} fatigue={r['fatigue']:>3d} force={r['fw_force']}")


if __name__ == "__main__":
    main()
