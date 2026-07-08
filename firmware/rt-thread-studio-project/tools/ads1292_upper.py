#!/usr/bin/env python3
"""
ADS1292 serial monitor.

Install:
    pip install pyserial matplotlib

Run:
    python tools/ads1292_upper.py COM3 --baud 115200
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
import time
from collections import deque
from pathlib import Path

import matplotlib.animation as animation
import matplotlib.pyplot as plt
import serial


TEXT_RE = re.compile(r"breath=(-?\d+),\s*ecg=(-?\d+)")


def parse_sample(line: str, fallback_seq: int) -> tuple[int, int, int, int, int, int, str, int] | None:
    line = line.strip()
    if not line:
        return None

    if line.startswith("ADS1292,"):
        parts = line.split(",")
        if len(parts) >= 4:
            fatigue = int(parts[4]) if len(parts) > 4 else 0
            rms = int(parts[5]) if len(parts) > 5 else 0
            zc = int(parts[6]) if len(parts) > 6 else 0
            status = parts[7].strip() if len(parts) > 7 else "NA"
            return int(parts[1]), int(parts[2]), int(parts[3]), fatigue, rms, zc, status, 0

    if line.startswith("FIT,"):
        parts = line.split(",")
        if len(parts) >= 18:
            seq = int(parts[1])
            emg = int(parts[2])
            angle = int(parts[3])
            fatigue = int(parts[10])
            rms = int(parts[11])
            zc = int(parts[12])
            phase = parts[13].strip()
            action = parts[14].strip()
            count = parts[15].strip()
            force = parts[16].strip()
            status = parts[17].strip()
            mav = int(parts[19]) if len(parts) > 19 else 0
            if len(parts) >= 25:
                active = parts[18].strip()
                base_mav = parts[21].strip()
                active_th = parts[24].strip()
                status = f"{phase}/{action}/#{count}/{force}/{status}/act={active}/mav={mav}/base={base_mav}/th={active_th}"
            else:
                status = f"{phase}/{action}/#{count}/{force}/{status}/mav={mav}"
            return seq, angle, emg, fatigue, rms, zc, status, mav

    match = TEXT_RE.search(line)
    if match:
        return fallback_seq, int(match.group(1)), int(match.group(2)), 0, 0, 0, "NA", 0

    return None


def build_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ADS1292 ECG/breath serial plotter")
    parser.add_argument("port", help="Serial port, for example COM3 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--window", type=int, default=500, help="Number of samples shown")
    parser.add_argument("--csv", default="ads1292_capture.csv", help="CSV output path")
    return parser


def main() -> int:
    args = build_argparser().parse_args()
    csv_path = Path(args.csv)

    seq_data: deque[int] = deque(maxlen=args.window)
    breath_data: deque[int] = deque(maxlen=args.window)
    ecg_data: deque[int] = deque(maxlen=args.window)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05)
    except serial.SerialException as exc:
        print(f"open serial failed: {exc}", file=sys.stderr)
        return 1
    ser.write(b"\r\nfit_start\r\n")

    csv_file = csv_path.open("w", newline="", encoding="utf-8")
    writer = csv.writer(csv_file)
    writer.writerow(["host_time", "seq", "angle_or_ch1", "raw_emg", "fatigue", "rms", "zc", "mav", "status"])

    fig, (ax_breath, ax_ecg) = plt.subplots(2, 1, sharex=True, figsize=(11, 7))
    fig.canvas.manager.set_window_title("ADS1292 Monitor")

    breath_line, = ax_breath.plot([], [], color="#1f77b4", linewidth=1.2)
    ecg_line, = ax_ecg.plot([], [], color="#d62728", linewidth=1.2)

    ax_breath.set_ylabel("Angle/Ch1")
    ax_ecg.set_ylabel("EMG MAV")
    ax_ecg.set_xlabel("Sample")
    ax_breath.grid(True, alpha=0.3)
    ax_ecg.grid(True, alpha=0.3)

    status = fig.text(0.01, 0.01, "Waiting for data...", fontsize=9)
    next_seq = 0
    last_sample_time = 0.0
    last_raw_emg = 0

    def update(_frame: int):
        nonlocal next_seq, last_sample_time

        while ser.in_waiting:
            raw = ser.readline()
            try:
                line = raw.decode("utf-8", errors="ignore")
            except UnicodeDecodeError:
                continue

            parsed = parse_sample(line, next_seq)
            if parsed is None:
                continue

            seq, breath, ecg, fatigue, rms, zc, fatigue_status, mav = parsed
            next_seq = seq + 1
            last_sample_time = time.time()
            last_raw_emg = ecg

            seq_data.append(seq)
            breath_data.append(breath)
            ecg_data.append(mav if mav else ecg)
            writer.writerow([f"{last_sample_time:.3f}", seq, breath, ecg, fatigue, rms, zc, mav, fatigue_status])

        if seq_data:
            x = list(seq_data)
            breath_line.set_data(x, list(breath_data))
            ecg_line.set_data(x, list(ecg_data))

            ax_breath.set_xlim(x[0], x[-1] if x[-1] > x[0] else x[0] + 1)
            ax_breath.relim()
            ax_breath.autoscale_view(scalex=False, scaley=True)
            ax_ecg.relim()
            ax_ecg.autoscale_view(scalex=False, scaley=True)
            status.set_text(
                f"port={args.port} baud={args.baud} samples={len(seq_data)} "
                f"last: angle={breath_data[-1]} raw={last_raw_emg} mav={ecg_data[-1]} "
                f"fatigue={fatigue} {fatigue_status} csv={csv_path}"
            )
        elif last_sample_time == 0.0:
            status.set_text(f"Waiting for ADS1292 data on {args.port} @ {args.baud}...")

        csv_file.flush()
        return breath_line, ecg_line, status

    try:
        animation.FuncAnimation(fig, update, interval=50, blit=False, cache_frame_data=False)
        plt.tight_layout(rect=(0, 0.03, 1, 1))
        plt.show()
    finally:
        try:
            ser.write(b"\r\nfit_stop\r\n")
        except serial.SerialException:
            pass
        csv_file.close()
        ser.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
