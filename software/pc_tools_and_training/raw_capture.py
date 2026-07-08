"""One-shot RAW capture: connect at 115200, send raw_fast_start, switch
local serial to 460800, record N seconds of RAW lines, then send raw_stop
to restore the board. Use this to validate the DC-step / overflow fixes.

Usage:
    python raw_capture.py COM12
    python raw_capture.py COM12 --seconds 60 --out test_raw.log
"""
from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    print("pip install pyserial", file=sys.stderr)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--fast-baud", type=int, default=460800)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    out_path = Path(args.out) if args.out else (
        Path(__file__).resolve().parent /
        f"raw_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log")

    # ---- Phase 1: command at 115200 -------------------------------------
    print(f"[1] open {args.port} @ 115200")
    ser = serial.Serial(args.port, 115200, timeout=0.2)
    time.sleep(0.5)
    # Drain any pending output (boot banner, etc).
    ser.reset_input_buffer()

    print(f"[2] send: raw_fast_start")
    ser.write(b"raw_fast_start\r\n")
    ser.flush()

    # The firmware prints RAW_BAUD,460800 then begins streaming.
    # Wait briefly so it has time to actually switch hardware baud.
    deadline = time.time() + 1.0
    saw_ack = False
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            try:
                txt = chunk.decode(errors="replace")
            except Exception:
                txt = ""
            if "RAW_BAUD" in txt or "RAW_BEGIN" in txt:
                saw_ack = True
            sys.stdout.write(txt)
            sys.stdout.flush()
    ser.close()
    if not saw_ack:
        print("\n[!] no RAW_BAUD ack seen -- continuing anyway, the board "
              "may have already switched on a previous run.", flush=True)

    # ---- Phase 2: re-open at fast baud, record RAW ----------------------
    print(f"\n[3] re-open {args.port} @ {args.fast_baud}, record "
          f"{args.seconds:.0f}s -> {out_path}")
    time.sleep(0.3)
    ser = serial.Serial(args.port, args.fast_baud, timeout=0.2)

    n_lines = 0
    n_overflow = 0
    t_start = time.time()
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(f"# raw capture {datetime.now().isoformat()} "
                f"port={args.port} baud={args.fast_baud}\n")
        last_print = t_start
        while time.time() - t_start < args.seconds:
            line = ser.readline()
            if not line:
                continue
            try:
                s = line.decode("utf-8", errors="replace").rstrip("\r\n")
            except Exception:
                continue
            if not s:
                continue
            f.write(s + "\n")
            n_lines += 1
            if "Overflow" in s or "DAPLink" in s:
                n_overflow += 1
            if time.time() - last_print > 1.0:
                last_print = time.time()
                elapsed = time.time() - t_start
                print(f"   t={elapsed:5.1f}s  lines={n_lines:>6}  "
                      f"overflow={n_overflow}", flush=True)

    # ---- Phase 3: stop RAW, restore baud --------------------------------
    print(f"\n[4] send: raw_stop  (board returns to 115200)")
    ser.write(b"raw_stop\r\n")
    ser.flush()
    time.sleep(0.3)
    ser.close()

    # ---- Phase 4: brief analysis ----------------------------------------
    print(f"\n[5] capture done -> {out_path}")
    print(f"    total lines:     {n_lines}")
    print(f"    overflow lines:  {n_overflow}  "
          + ("[FAIL]" if n_overflow else "[OK]"))

    raw_count = 0
    vals = []
    with open(out_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("RAW,"):
                continue
            p = line.split(",")
            if len(p) < 3:
                continue
            try:
                vals.append((int(p[1]), int(p[2])))
                raw_count += 1
            except Exception:
                pass

    if not vals:
        print("    [!] no RAW samples parsed -- did the board switch baud?")
        return

    dur = (vals[-1][0] - vals[0][0]) / 1000.0
    rate = raw_count / dur if dur > 0 else 0
    LSB_V = 2.0 * 2.42 / (6.0 * (1 << 24)) * 1000.0  # mV/LSB

    print(f"    RAW samples:     {raw_count}")
    print(f"    duration:        {dur:.1f} s")
    print(f"    sample rate:     {rate:.1f} Hz   "
          + ("[OK]" if rate > 480 else "[LOW -- expected ~500 Hz]"))

    # 5-second bins for DC drift.
    t0 = vals[0][0]
    bins = {}
    for t, v in vals:
        b = int((t - t0) / 5000)
        bins.setdefault(b, []).append(v)

    print()
    print("    DC drift per 5-second bin (raw counts -> mV):")
    means = []
    for b in sorted(bins.keys()):
        a = bins[b]
        m = sum(a) / len(a)
        means.append(m)
        print(f"      {b*5:>4}-{(b+1)*5:>4}s  n={len(a):>4}  "
              f"mean={int(m):>10}  ({m * LSB_V:>+7.2f} mV)")

    if len(means) > 1:
        max_jump = max(abs(means[i] - means[i-1]) for i in range(1, len(means)))
        print(f"\n    largest 5s-to-5s mean jump: {int(max_jump)} counts "
              f"({max_jump * LSB_V:+.2f} mV)")
        verdict = "[OK]" if max_jump * LSB_V < 5 else "[FAIL -- electrode contact issue]"
        print(f"    DC stability:    {verdict}")


if __name__ == "__main__":
    main()
