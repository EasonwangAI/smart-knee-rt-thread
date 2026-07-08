"""Generate a tiny mock LIVE log so we can sanity-check session_replay.py
without needing the actual board on bench. Not for real validation -- just
a smoke test that parsing + feature aggregation + prediction does not crash.
"""
import random
from pathlib import Path

OUT = Path(__file__).resolve().parent / "mock_live.log"

def gen():
    rng = random.Random(42)
    lines = [
        "FIT monitor v2 starting...",
        "# version=2 sample_rate=500 window=256 hop=64 hpf=20 notch=50 lpf=150",
        "# LIVE,t_ms,ac,rms,mav,wl,zc,ssc,zcr_x1k,fatigue,alert,emg_phase,emg_status,mot_phase,angle_x10,active,ax,ay,az,gx,gy,gz",
        "CAL,REST_BEGIN",
        "MPU,CALIBRATED,base_angle_x10=10,bias=(50,30,-20)",
        "CAL,REST_READY,rest_rms=120,rest_mav=98,rest_wl=18000,rest_zc=2,rest_ssc=4,zc_th=350,ssc_th=480,active_th=3000",
        "CAL,ACTIVE_READY,base_rms=820,base_mav=750,base_wl=160000,base_zc=18,base_ssc=42",
    ]

    # 10 seconds of "squat-like" data: angle rising to 300 then back to 0,
    # accel/gyro moving accordingly. 20 Hz print rate -> 200 rows.
    t = 0
    for i in range(200):
        # Angle profile: stand -> descend -> bottom -> ascend -> stand
        phase = i / 40.0  # rep takes 2 seconds
        rep_idx = int(phase)
        within = phase - rep_idx
        if within < 0.3:
            ang = within / 0.3 * 350  # descending 0->350
        elif within < 0.5:
            ang = 350                  # bottom
        elif within < 0.8:
            ang = 350 - (within - 0.5) / 0.3 * 350  # ascending
        else:
            ang = 0
        ang += rng.randint(-15, 15)

        # EMG is mostly low at rest, jumps up when active (during the rep).
        if 30 < ang < 350:
            mav = 1500 + rng.randint(-200, 400)
            rms = mav + rng.randint(0, 200)
            wl  = 80000 + rng.randint(-10000, 20000)
            zc  = rng.randint(6, 14)
            ssc = rng.randint(15, 30)
            active = 1
        else:
            mav = 90 + rng.randint(-15, 15)
            rms = 110 + rng.randint(-15, 15)
            wl  = 15000 + rng.randint(-3000, 3000)
            zc  = rng.randint(0, 3)
            ssc = rng.randint(0, 5)
            active = 0

        ac = rng.randint(-500, 500)

        # IMU raw: accel mostly along axis 1 (gravity ~ -16384 LSB = -1g).
        # During squat, tilt changes the x-component.
        tilt_rad = ang / 1800.0 * 3.14159
        import math
        ax = int(16384 * math.sin(tilt_rad)) + rng.randint(-200, 200)
        ay = int(-16384 * math.cos(tilt_rad)) + rng.randint(-200, 200)
        az = rng.randint(-500, 500)
        gx = rng.randint(-30, 30) * 131    # mostly small
        gy = rng.randint(-30, 30) * 131
        gz = rng.randint(-30, 30) * 131

        fatigue = 0
        alert   = 0
        emg_phase = "RUN"
        emg_status = "OK" if active else "REST"
        mot_phase = (
            "DESC" if 30 < ang < 340 and within < 0.5 else
            "BOT"  if ang > 340 else
            "ASC"  if 30 < ang < 340 and within >= 0.5 else
            "STAND"
        )

        lines.append(
            f"LIVE,{t},{ac},{rms},{mav},{wl},{zc},{ssc},"
            f"{int(zc*1000/256)},{fatigue},{alert},"
            f"{emg_phase},{emg_status},{mot_phase},{ang},{active},"
            f"{ax},{ay},{az},{gx},{gy},{gz}"
        )
        t += 50  # 20 Hz -> 50 ms per row

    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote mock log: {OUT}  ({len(lines)} lines)")

if __name__ == "__main__":
    gen()
