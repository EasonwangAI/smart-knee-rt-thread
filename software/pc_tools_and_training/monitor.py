"""mpu_wave monitor: real-time visualisation + logging for the FIT-monitor
firmware running on the STM32F407 + ADS1292R + MPU6050 single-leg knee brace.

Connects to the board's UART, parses LIVE / CAL / REP lines, draws four live
plots, and writes the raw serial stream to a timestamped log file in this
directory. The log can later be re-analysed offline (e.g. with the
session_replay.py script in the test_pro3 project).

Usage:
    python monitor.py COM3                       # connect to COM3 @ 115200
    python monitor.py COM3 --baud 115200
    python monitor.py --list-ports               # show available ports
    python monitor.py replay path\\to\\log.log   # replay a saved log file

Close the plot window to exit. The log keeps streaming until then.
"""
from __future__ import annotations

import argparse
import csv
import queue
import sys
import threading
import time
import warnings
from collections import deque
from datetime import datetime
from pathlib import Path

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.animation as animation

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not installed. Run: pip install pyserial",
          file=sys.stderr)
    sys.exit(1)

# --- Constants -----------------------------------------------------------
DEFAULT_BAUD       = 115200
DEFAULT_PLOT_WIN_S = 30.0
RAW_WIN_S          = 5.0
RAW_FEATURE_FS_HZ  = 500.0
RAW_FEATURE_WINDOW_S = 2.0
RAW_FEATURE_HOP_S    = 1.0
RAW_FEATURE_BAND_HZ  = (20.0, 150.0)
RAW_FEATURE_MIN_COVERAGE = 0.8
RAW_FEATURE_MAX_GAP_MS = 10

ACTION_MODEL_PATH = Path(r"D:\mpu_wave\rt_thread_project\test_pro3\python\kneepad_action_rf.joblib")
FATIGUE_MODEL_PATH = Path(r"D:\mpu_wave\training_curated\fatigue_emg_balanced_sessions.joblib")
INFER_WINDOW_S = 3.0
INFER_HOP_S = 1.0
LIVE_FS_HZ = 20.0
EMG_WINDOW_SAMPLES = 256

# ADS1292R config in firmware: CH2 gain 6, CH1 gain 2, VREF 2.42 V.
ADS_LSB_V_GAIN6 = 2.0 * 2.42 / (6.0 * (1 << 24))  # ~48 nV / LSB
ADS_LSB_V_GAIN2 = 2.0 * 2.42 / (2.0 * (1 << 24))  # ~144 nV / LSB
ADS_LSB_V    = ADS_LSB_V_GAIN6
ACC_LSB_G    = 1.0 / 16384.0                       # MPU6050 +/-2g
GYRO_LSB_DPS = 1.0 / 131.0                         # MPU6050 +/-250 dps

LOG_DIR = Path(__file__).resolve().parent

# Keyboard label map. Press the digit to toggle that label on/off; pressing
# the same digit again or '0' clears the active label (writes a REST mark).
# The mapping follows the data-collection plan in our chat history:
#   1: Squat                2: Walking           3: Running
#   4: Deadlift             5: FreshSquat        6: FatiguedSquat
#   0: Rest (no activity)
LABEL_KEYS = {
    "1": "Squat",
    "2": "Walking",
    "3": "Running",
    "4": "Deadlift",
    "5": "FreshSquat",
    "6": "FatiguedSquat",
    "0": "Rest",
}

ACTION_FEATURE_COLS = [
    "mav", "rms", "wl", "iemg", "var", "zc", "ssc",
    "swing", "tilt_std", "tilt_p95", "tilt_zc",
    "gyro_mean", "gyro_max", "gyro_std", "acc_std",
]

FATIGUE_FEATURE_COLS = [
    "mav", "rms", "wl", "iemg", "var", "zc", "ssc",
    "zc_per_mav", "ssc_per_rms", "rms_mav_ratio",
    "wl_per_iemg", "var_per_rms2",
]

ACTION_OUTPUT_MAP = {
    "Squat": "Squat",
    "Deadlift": "Deadlift",
    "Walking": "Walking",
    # Legacy external KneE-PAD model class. Keep it explicit so we do not
    # silently pretend LegExt is the same as Deadlift.
    "LegExt": "LegExt",
}


# --- Parsing -------------------------------------------------------------
def parse_live(line: str):
    """Parse a `LIVE,...` row into a dict, returning None on mismatch."""
    if not line.startswith("LIVE,"):
        return None
    parts = line.split(",")
    if len(parts) < 16:
        return None
    try:
        i = lambda x: int(float(x))
        d = dict(
            t_ms       = i(parts[1]),
            ac         = i(parts[2]),
            rms        = i(parts[3]),
            mav        = i(parts[4]),
            wl         = i(parts[5]),
            zc         = i(parts[6]),
            ssc        = i(parts[7]),
            zcr_x1k    = i(parts[8]),
            fatigue    = i(parts[9]),
            alert      = i(parts[10]),
            emg_phase  = parts[11].strip(),
            emg_status = parts[12].strip(),
            mot_phase  = parts[13].strip(),
            angle_x10  = i(parts[14]),
            active     = i(parts[15]),
        )
        if len(parts) >= 22:
            d.update(
                ax=i(parts[16]), ay=i(parts[17]), az=i(parts[18]),
                gx=i(parts[19]), gy=i(parts[20]), gz=i(parts[21]),
            )
        return d
    except Exception:
        return None


def parse_raw(line: str):
    """Parse a `RAW,t_ms,emg` row from the 500 Hz ADS1292R stream."""
    if not line.startswith("RAW,"):
        return None
    parts = line.split(",")
    if len(parts) < 3:
        return None
    try:
        return int(float(parts[1])), int(float(parts[2]))
    except Exception:
        return None


def parse_mark(line: str):
    """Parse a GUI label mark: `MARK,t_ms,label`."""
    if not line.startswith("MARK,"):
        return None
    parts = line.split(",", 2)
    if len(parts) < 3:
        return None
    try:
        return int(float(parts[1])), parts[2].strip()
    except Exception:
        return None


def parse_cal(line: str):
    """Pull rest_mav / active_th out of a CAL,REST_READY,... line."""
    if not line.startswith("CAL,"):
        return None
    out = {}
    for tok in line.split(","):
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k.strip()] = float(v.strip())
            except ValueError:
                out[k.strip()] = v.strip()
    return out


def fatigue_label_from_text(label: str):
    label_l = (label or "").strip().lower()
    if "fatig" in label_l:
        return 1
    if "fresh" in label_l or "ideal" in label_l or "rest" in label_l:
        return 0
    return ""


def label_at_ms(t_ms: int, marks):
    label = "Rest"
    for mark_t, mark_label in marks:
        if mark_t <= t_ms:
            label = mark_label
        else:
            break
    return label


def compute_raw_window_features(values, fs_hz: float):
    x = np.asarray(values, dtype=np.float64)
    if x.size == 0:
        return None

    raw_mean = float(np.mean(x))
    raw_std = float(np.std(x))
    x0 = x - raw_mean

    freqs = None
    band = None
    power = None
    if x.size > 4:
        freqs = np.fft.rfftfreq(x.size, d=1.0 / fs_hz)
        spec = np.fft.rfft(x0 * np.hanning(x.size))
        lo, hi = RAW_FEATURE_BAND_HZ
        band = (freqs >= lo) & (freqs <= min(hi, fs_hz / 2.0))

        filt_spec = np.fft.rfft(x0)
        filt_spec[~band] = 0
        xc = np.fft.irfft(filt_spec, n=x.size)
        power = np.abs(spec) ** 2
    else:
        xc = x0

    abs_x = np.abs(xc)
    std = float(np.std(xc))
    th = max(1.0, std * 0.05)

    rms = float(np.sqrt(np.mean(xc * xc)))
    mav = float(np.mean(abs_x))
    wl = float(np.sum(np.abs(np.diff(xc)))) if x.size > 1 else 0.0

    if x.size > 1:
        zc_mask = ((xc[:-1] > 0) & (xc[1:] < 0)) | ((xc[:-1] < 0) & (xc[1:] > 0))
        zc_mask &= (np.abs(xc[:-1]) > th) | (np.abs(xc[1:]) > th)
        zc = int(np.sum(zc_mask))
    else:
        zc = 0

    if x.size > 2:
        left = xc[1:-1] - xc[:-2]
        right = xc[1:-1] - xc[2:]
        ssc_mask = (left * right > 0) & ((np.abs(left) > th) | (np.abs(right) > th))
        ssc = int(np.sum(ssc_mask))
    else:
        ssc = 0

    mdf = 0.0
    mpf = 0.0
    if power is not None and band is not None and freqs is not None:
        band_power = power[band]
        band_freqs = freqs[band]
        total = float(np.sum(band_power))
        if total > 0.0 and band_freqs.size:
            mpf = float(np.sum(band_freqs * band_power) / total)
            half_idx = int(np.searchsorted(np.cumsum(band_power), total * 0.5))
            half_idx = min(half_idx, band_freqs.size - 1)
            mdf = float(band_freqs[half_idx])

    return dict(
        rms=rms,
        mav=mav,
        wl=wl,
        zc=zc,
        ssc=ssc,
        mdf=mdf,
        mpf=mpf,
        mean=float(np.mean(xc)),
        std=std,
        min=float(np.min(xc)),
        max=float(np.max(xc)),
        raw_mean=raw_mean,
        raw_std=raw_std,
        zc_threshold=th,
        ssc_threshold=th,
    )


def export_raw_features_from_log(log_path: Path,
                                 fs_hz: float = RAW_FEATURE_FS_HZ,
                                 window_s: float = RAW_FEATURE_WINDOW_S,
                                 hop_s: float = RAW_FEATURE_HOP_S):
    raw_rows = []
    marks = []
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            raw = parse_raw(line)
            if raw is not None:
                raw_rows.append(raw)
                continue
            mark = parse_mark(line)
            if mark is not None:
                marks.append(mark)

    if not raw_rows:
        return None

    raw_rows.sort(key=lambda item: item[0])
    marks.sort(key=lambda item: item[0])

    raw_csv = log_path.with_name(log_path.stem + "_raw.csv")
    with open(raw_csv, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_ms", "emg"])
        w.writerows(raw_rows)

    t = np.asarray([item[0] for item in raw_rows], dtype=np.int64)
    x = np.asarray([item[1] for item in raw_rows], dtype=np.float64)

    win_ms = int(round(window_s * 1000.0))
    hop_ms = int(round(hop_s * 1000.0))
    min_samples = int(round(window_s * fs_hz * RAW_FEATURE_MIN_COVERAGE))

    rows = []
    start_ms = int(t[0])
    last_ms = int(t[-1])
    window_idx = 0
    while start_ms + win_ms <= last_ms + 1:
        end_ms = start_ms + win_ms
        lo = int(np.searchsorted(t, start_ms, side="left"))
        hi = int(np.searchsorted(t, end_ms, side="left"))
        if hi - lo >= min_samples:
            gaps = np.diff(t[lo:hi])
            max_gap_ms = int(gaps.max()) if gaps.size else 0
            if max_gap_ms > RAW_FEATURE_MAX_GAP_MS:
                start_ms += hop_ms
                window_idx += 1
                continue
            label_start = label_at_ms(start_ms, marks)
            label_end = label_at_ms(end_ms - 1, marks)
            if label_start == label_end:
                feats = compute_raw_window_features(x[lo:hi], fs_hz)
                if feats is not None:
                    row = dict(
                        source_log=str(log_path),
                        window_idx=window_idx,
                        start_ms=start_ms,
                        end_ms=end_ms,
                        window_s=window_s,
                        hop_s=hop_s,
                        fs_hz=fs_hz,
                        sample_count=hi - lo,
                        max_gap_ms=max_gap_ms,
                        label=label_start,
                        fatigue_label=fatigue_label_from_text(label_start),
                    )
                    row.update(feats)
                    rows.append(row)
        start_ms += hop_ms
        window_idx += 1

    feature_csv = log_path.with_name(log_path.stem + "_raw_features.csv")
    fields = [
        "source_log", "window_idx", "start_ms", "end_ms", "window_s",
        "hop_s", "fs_hz", "sample_count", "max_gap_ms", "label", "fatigue_label",
        "rms", "mav", "wl", "zc", "ssc", "mdf", "mpf",
        "mean", "std", "min", "max", "raw_mean", "raw_std",
        "zc_threshold", "ssc_threshold",
    ]
    with open(feature_csv, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)

    return dict(raw_csv=raw_csv, feature_csv=feature_csv,
                raw_count=len(raw_rows), feature_count=len(rows))


def _zero_crossings(values: np.ndarray) -> int:
    if values.size < 2:
        return 0
    a = values[:-1]
    b = values[1:]
    return int(np.sum(((a > 0) & (b < 0)) | ((a < 0) & (b > 0))))


def compute_live_model_features(rows):
    """Build the 15 action features from a short LIVE-row window.

    LIVE EMG values are rolling 256-sample firmware features in ADS counts.
    We convert them to mV and expand rolling totals to the full window so the
    feature scale matches the Python training data as closely as possible.
    """
    if len(rows) < 3:
        return None

    t_ms = np.asarray([r["t_ms"] for r in rows], dtype=np.float64)
    duration_s = max(0.001, (t_ms[-1] - t_ms[0]) / 1000.0)
    emg_samples = max(1.0, duration_s * RAW_FEATURE_FS_HZ)
    lsb_mv = ADS_LSB_V * 1000.0

    rms_win = np.asarray([r["rms"] for r in rows], dtype=np.float64) * lsb_mv
    mav_win = np.asarray([r["mav"] for r in rows], dtype=np.float64) * lsb_mv
    wl_win = np.asarray([r["wl"] for r in rows], dtype=np.float64) * lsb_mv

    mav = float(np.mean(mav_win))
    rms = float(np.sqrt(np.mean(rms_win * rms_win)))
    wl = float(np.mean(wl_win) * emg_samples / EMG_WINDOW_SAMPLES)
    iemg = float(mav * emg_samples)
    var = float(np.mean(rms_win * rms_win))
    zc = float(np.mean([r["zc"] for r in rows]) * emg_samples / EMG_WINDOW_SAMPLES)
    ssc = float(np.mean([r["ssc"] for r in rows]) * emg_samples / EMG_WINDOW_SAMPLES)

    tilt = np.asarray([r["angle_x10"] for r in rows], dtype=np.float64) / 10.0
    tilt_rel = tilt - tilt[0]
    swing = float(np.max(tilt_rel) - np.min(tilt_rel))
    tilt_std = float(np.std(tilt_rel))
    tilt_p95 = float(np.percentile(np.abs(tilt_rel), 95))
    tilt_zc = float(_zero_crossings(tilt_rel))

    gx = np.asarray([r["gx"] for r in rows], dtype=np.float64) * GYRO_LSB_DPS
    gy = np.asarray([r["gy"] for r in rows], dtype=np.float64) * GYRO_LSB_DPS
    gz = np.asarray([r["gz"] for r in rows], dtype=np.float64) * GYRO_LSB_DPS
    gyro_mag = np.sqrt(gx * gx + gy * gy + gz * gz)
    gyro_mean = float(np.mean(gyro_mag))
    gyro_max = float(np.max(gyro_mag))
    gyro_std = float(np.std(gyro_mag))

    ax = np.asarray([r["ax"] for r in rows], dtype=np.float64) * ACC_LSB_G
    ay = np.asarray([r["ay"] for r in rows], dtype=np.float64) * ACC_LSB_G
    az = np.asarray([r["az"] for r in rows], dtype=np.float64) * ACC_LSB_G
    acc_mag = np.sqrt(ax * ax + ay * ay + az * az)
    acc_std = float(np.std(acc_mag))

    return dict(
        mav=mav, rms=rms, wl=wl, iemg=iemg, var=var, zc=zc, ssc=ssc,
        swing=swing, tilt_std=tilt_std, tilt_p95=tilt_p95,
        tilt_zc=tilt_zc, gyro_mean=gyro_mean, gyro_max=gyro_max,
        gyro_std=gyro_std, acc_std=acc_std,
        duration_s=duration_s, live_rows=len(rows),
    )


def add_fatigue_ratios(feats):
    out = dict(feats)
    mav = out["mav"]
    rms = out["rms"]
    wl = out["wl"]
    iemg = out["iemg"]
    var = out["var"]
    zc = out["zc"]
    ssc = out["ssc"]
    out["zc_per_mav"] = zc / (mav + 1e-9)
    out["ssc_per_rms"] = ssc / (rms + 1e-9)
    out["rms_mav_ratio"] = rms / (mav + 1e-9)
    out["wl_per_iemg"] = wl / (iemg + 1e-9)
    out["var_per_rms2"] = var / ((rms * rms) + 1e-9)
    return out


def load_pc_models(action_path: Path, fatigue_path: Path):
    from joblib import load

    warnings.filterwarnings("ignore", category=UserWarning)
    action_model = load(action_path)
    fatigue_model = load(fatigue_path)

    action_n = getattr(action_model, "n_features_in_", None)
    fatigue_n = getattr(fatigue_model, "n_features_in_", None)
    if action_n != len(ACTION_FEATURE_COLS):
        raise ValueError(f"action model expects {action_n} features, expected {len(ACTION_FEATURE_COLS)}")
    if fatigue_n != len(FATIGUE_FEATURE_COLS):
        raise ValueError(f"fatigue model expects {fatigue_n} features, expected {len(FATIGUE_FEATURE_COLS)}")
    action_classes = {str(c) for c in getattr(action_model, "classes_", [])}
    if "Deadlift" not in action_classes:
        print("[infer] WARNING: action model has no Deadlift class; train a Squat/Deadlift/Walking model before judging Deadlift.")
    return action_model, fatigue_model


def predict_pc_models(features, action_model, fatigue_model):
    action_x = np.asarray([[features[c] for c in ACTION_FEATURE_COLS]], dtype=np.float64)
    raw_action = action_model.predict(action_x)[0]
    action_probs = action_model.predict_proba(action_x)[0]
    class_probs = {str(cls): float(prob) for cls, prob in zip(action_model.classes_, action_probs)}

    raw_action = str(raw_action)
    action_label = ACTION_OUTPUT_MAP.get(raw_action, raw_action)
    action_confidence = float(class_probs.get(raw_action, np.max(action_probs)))

    fatigue_features = add_fatigue_ratios(features)
    fatigue_x = np.asarray([[fatigue_features[c] for c in FATIGUE_FEATURE_COLS]], dtype=np.float64)
    fatigue_pred = fatigue_model.predict(fatigue_x)[0]
    fatigue_probs = fatigue_model.predict_proba(fatigue_x)[0]
    classes = list(fatigue_model.classes_)
    if 1 in classes:
        fatigued_prob = float(fatigue_probs[classes.index(1)])
    else:
        fatigued_prob = float(np.max(fatigue_probs))

    fatigue_label = "Fatigued" if int(fatigue_pred) == 1 else "Fresh"
    return dict(
        raw_action=raw_action,
        action_label=action_label,
        action_confidence=action_confidence,
        squat_prob=float(class_probs.get("Squat", 0.0)),
        deadlift_prob=float(class_probs.get("Deadlift", 0.0)),
        walking_prob=float(class_probs.get("Walking", 0.0)),
        legext_prob=float(class_probs.get("LegExt", 0.0)),
        fatigue_label=fatigue_label,
        fatigued_prob=fatigued_prob,
    )


def send_msh_command(ser, command: str):
    ser.write((command.strip() + "\r\n").encode("ascii"))
    ser.flush()
    time.sleep(0.15)


# --- Serial reader thread ------------------------------------------------
class LogWriter:
    """Thread-safe append-only log writer. Owned by the reader thread but
    also written to by the GUI thread when the user presses a label key."""
    def __init__(self, path: Path):
        self.path = path
        self._lock = threading.Lock()
        self._f = None
        self._pending = 0
        self._last_flush = 0.0

    def open(self):
        self._f = open(self.path, "w", encoding="utf-8")
        self._f.write(f"# mpu_wave session start {datetime.now().isoformat()}\n")
        self._f.flush()
        self._pending = 0
        self._last_flush = time.time()

    def write(self, line: str):
        if self._f is None:
            return
        with self._lock:
            self._f.write(line + "\n")
            self._pending += 1
            now = time.time()
            if line.startswith("MARK,") or self._pending >= 100 or now - self._last_flush >= 0.5:
                self._f.flush()
                self._pending = 0
                self._last_flush = now

    def close(self):
        if self._f is not None:
            with self._lock:
                self._f.flush()
                self._f.close()
                self._f = None


def serial_reader(ser, q_out, log_writer, stop_evt):
    print(f"[reader] logging raw serial to {log_writer.path}")
    log_writer.open()
    try:
        while not stop_evt.is_set():
            try:
                raw = ser.readline()
            except serial.SerialException as e:
                print(f"[reader] serial error: {e}")
                stop_evt.set()
                break
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if not line:
                continue
            log_writer.write(line)
            try:
                q_out.put_nowait(line)
            except queue.Full:
                pass
    finally:
        log_writer.close()


def file_replayer(path: Path, q_out, stop_evt, speed: float):
    """Re-feed an existing log file into the queue at approx wall-clock speed.
    Used when invoked as: python monitor.py replay path\\to\\log.log
    """
    print(f"[replay] streaming {path} at {speed}x")
    last_t = None
    t_offset = None
    start_wall = time.time()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if stop_evt.is_set():
                break
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            d = parse_live(line)
            if d is not None:
                if t_offset is None:
                    t_offset = d["t_ms"]
                t_live = (d["t_ms"] - t_offset) / 1000.0
                elapsed = (time.time() - start_wall) * speed
                if t_live > elapsed:
                    time.sleep(min(0.5, (t_live - elapsed) / speed))
            try:
                q_out.put_nowait(line)
            except queue.Full:
                pass
    print("[replay] done")


# --- Rolling buffer ------------------------------------------------------
class Roll:
    def __init__(self, max_s: float, sample_hz: float):
        n = int(max_s * sample_hz * 1.5)
        self.t = deque(maxlen=n)
        self.v = deque(maxlen=n)

    def append(self, t, v):
        self.t.append(t)
        self.v.append(v)

    def arrays(self):
        return np.asarray(self.t), np.asarray(self.v)


# --- Main ----------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    p_live = sub.add_parser("live", help="connect to a serial port (default)")
    p_live.add_argument("port", help="serial port, e.g. COM3")
    p_live.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    p_live.add_argument("--plot-window", type=float, default=DEFAULT_PLOT_WIN_S)
    p_live.add_argument("--raw-start", action="store_true",
                        help="send raw_start after connecting")
    p_live.add_argument("--raw-only", action="store_true",
                        help="send fit_stop, then raw_start after connecting")
    p_live.add_argument("--raw-channel", type=int, choices=(1, 2),
                        help="select ADS1292R CH1 or CH2 before raw_start")
    p_live.add_argument("--raw-fast", action="store_true",
                        help="switch board UART to 921600, then start RAW")
    p_live.add_argument("--fast-baud", type=int, default=921600,
                        help="baud used after raw_fast_start")
    p_live.add_argument("--fit-start", action="store_true",
                        help="send fit_start after connecting for the legacy LIVE stream")
    p_live.add_argument("--infer", action="store_true",
                        help="run full PC-side action and fatigue models on LIVE windows")
    p_live.add_argument("--action-model", default=str(ACTION_MODEL_PATH),
                        help="path to kneepad action RandomForest .joblib")
    p_live.add_argument("--fatigue-model", default=str(FATIGUE_MODEL_PATH),
                        help="path to fatigue RandomForest .joblib")
    p_live.add_argument("--infer-window", type=float, default=INFER_WINDOW_S,
                        help="PC inference window length in seconds")
    p_live.add_argument("--infer-hop", type=float, default=INFER_HOP_S,
                        help="PC inference update interval in seconds")

    p_replay = sub.add_parser("replay", help="play back a saved log file")
    p_replay.add_argument("log_path")
    p_replay.add_argument("--speed", type=float, default=2.0,
                          help="replay speed multiplier")
    p_replay.add_argument("--plot-window", type=float, default=DEFAULT_PLOT_WIN_S)
    p_replay.add_argument("--infer", action="store_true",
                          help="run full PC-side action and fatigue models on LIVE windows")
    p_replay.add_argument("--action-model", default=str(ACTION_MODEL_PATH),
                          help="path to kneepad action RandomForest .joblib")
    p_replay.add_argument("--fatigue-model", default=str(FATIGUE_MODEL_PATH),
                          help="path to fatigue RandomForest .joblib")
    p_replay.add_argument("--infer-window", type=float, default=INFER_WINDOW_S,
                          help="PC inference window length in seconds")
    p_replay.add_argument("--infer-hop", type=float, default=INFER_HOP_S,
                          help="PC inference update interval in seconds")

    p_list = sub.add_parser("list-ports", help="list serial ports and exit")

    # back-compat: if first positional looks like a COM port, treat as live.
    if len(sys.argv) >= 2 and sys.argv[1].upper().startswith(("COM", "/DEV/")):
        sys.argv.insert(1, "live")
    elif len(sys.argv) == 1:
        sys.argv.extend(["--help"])

    args = ap.parse_args()

    if args.cmd == "list-ports":
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device:<10}  {p.description}")
        return

    LOG_DIR.mkdir(parents=True, exist_ok=True)

    q = queue.Queue(maxsize=10000)
    stop_evt = threading.Event()
    ser = None
    log_writer = None
    log_path = None

    if args.cmd == "live":
        try:
            ser = serial.Serial(args.port, args.baud, timeout=0.1)
        except serial.SerialException as e:
            print(f"ERROR opening {args.port}: {e}", file=sys.stderr)
            sys.exit(1)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_path = LOG_DIR / f"session_{ts}.log"
        log_writer = LogWriter(log_path)
        title_src = f"{args.port} @ {args.baud}"
        t = threading.Thread(target=serial_reader,
                             args=(ser, q, log_writer, stop_evt), daemon=True)
        t.start()
        if args.raw_only or args.raw_fast:
            send_msh_command(ser, "fit_stop")
        elif args.fit_start:
            send_msh_command(ser, "fit_start")
        if args.raw_channel == 1:
            send_msh_command(ser, "raw_ch1")
        elif args.raw_channel == 2:
            send_msh_command(ser, "raw_ch2")
        if args.raw_fast:
            send_msh_command(ser, "raw_fast_start")
            ser.baudrate = args.fast_baud
            time.sleep(0.4)
        elif args.raw_start or args.raw_only:
            send_msh_command(ser, "raw_start")
    elif args.cmd == "replay":
        path = Path(args.log_path)
        if not path.exists():
            print(f"ERROR: {path} not found", file=sys.stderr)
            sys.exit(1)
        title_src = f"REPLAY {path.name} @ {args.speed}x"
        t = threading.Thread(target=file_replayer,
                             args=(path, q, stop_evt, args.speed),
                             daemon=True)
        t.start()
    else:
        ap.print_help()
        return

    plot_window = args.plot_window

    pc_action_model = None
    pc_fatigue_model = None
    if getattr(args, "infer", False):
        try:
            pc_action_model, pc_fatigue_model = load_pc_models(
                Path(args.action_model), Path(args.fatigue_model)
            )
            print(f"[infer] action model:  {args.action_model}")
            print(f"[infer] fatigue model: {args.fatigue_model}")
            print(f"[infer] window={args.infer_window:.1f}s hop={args.infer_hop:.1f}s")
        except Exception as e:
            print(f"ERROR loading PC inference models: {e}", file=sys.stderr)
            sys.exit(1)

    # --- Plot layout -----------------------------------------------------
    plt.rcParams["toolbar"] = "toolbar2"
    fig, axes = plt.subplots(4, 1, figsize=(13, 9))
    fig.subplots_adjust(left=0.08, right=0.74, top=0.94, bottom=0.06,
                        hspace=0.45)

    title = "FIT monitor live  -  " + title_src
    if log_path is not None:
        title += f"\nlog: {log_path}"
    fig.suptitle(title, fontsize=10, fontweight="bold")

    ax_mav, ax_ac, ax_angle, ax_imu = axes

    ax_mav.set_title("EMG envelope  (MAV, mV)")
    ax_mav.set_ylabel("mV")
    ax_mav.grid(True, alpha=0.3)
    line_mav,        = ax_mav.plot([], [], "b-",  lw=1.2, label="mav")
    line_active_th,  = ax_mav.plot([], [], "r--", lw=1.0, alpha=0.7,
                                   label="active_th")
    line_rest_mav,   = ax_mav.plot([], [], "g--", lw=1.0, alpha=0.7,
                                   label="rest_mav")
    ax_mav.legend(loc="upper right", fontsize=8)

    ax_ac.set_title(f"EMG raw waveform  (RAW AC/LIVE, last {RAW_WIN_S}s, mV)")
    ax_ac.set_ylabel("mV")
    ax_ac.grid(True, alpha=0.3)
    ax_ac.axhline(0, color="gray", lw=0.5)
    line_ac, = ax_ac.plot([], [], "g-", lw=0.8)

    ax_angle.set_title("Knee tilt angle  (deg)")
    ax_angle.set_ylabel("deg")
    ax_angle.grid(True, alpha=0.3)
    line_angle, = ax_angle.plot([], [], "r-", lw=1.5)

    ax_imu.set_title("IMU magnitude")
    ax_imu.set_ylabel("|a|(g),  |g|(dps/100)")
    ax_imu.set_xlabel("t (s)")
    ax_imu.grid(True, alpha=0.3)
    line_acc,  = ax_imu.plot([], [], "b-",      lw=1.0, label="|accel| g")
    line_gyro, = ax_imu.plot([], [], "orange",  lw=1.0,
                             label="|gyro|/100 dps")
    ax_imu.legend(loc="upper right", fontsize=8)

    # Status panel on the right side of figure
    status_txt = fig.text(0.755, 0.92, "", ha="left", va="top",
                          fontsize=9, family="monospace",
                          bbox=dict(facecolor="#f6f6f6", edgecolor="gray",
                                    alpha=0.95))

    cal_txt = fig.text(0.755, 0.58, "", ha="left", va="top",
                       fontsize=8.5, family="monospace",
                       bbox=dict(facecolor="#fff8e1", edgecolor="gray",
                                 alpha=0.95))

    rep_txt = fig.text(0.755, 0.31, "", ha="left", va="top",
                       fontsize=8.5, family="monospace",
                       bbox=dict(facecolor="#e8f5e9", edgecolor="gray",
                                 alpha=0.95))

    msg_txt = fig.text(0.755, 0.10, "", ha="left", va="top",
                       fontsize=8.5, family="monospace",
                       bbox=dict(facecolor="#fce4ec", edgecolor="gray",
                                 alpha=0.95))

    # --- Buffers ---------------------------------------------------------
    buf_mav   = Roll(plot_window, RAW_FEATURE_FS_HZ)
    buf_ac    = Roll(RAW_WIN_S,  RAW_FEATURE_FS_HZ)
    buf_angle = Roll(plot_window, 20)
    buf_acc   = Roll(plot_window, 20)
    buf_gyro  = Roll(plot_window, 20)
    raw_mav_win = deque(maxlen=int(0.2 * RAW_FEATURE_FS_HZ))
    raw_dc_win = deque(maxlen=max(4, int(0.05 * RAW_FEATURE_FS_HZ)))
    live_history = deque(maxlen=int(max(plot_window, args.infer_window + 5.0) * LIVE_FS_HZ * 2.0))

    state = dict(
        t0=None,
        n_live=0,
        n_raw=0,
        n_other=0,
        last_phase="-",
        rest_mav_mv=None,
        active_th_mv=None,
        last_cal="(none)",
        last_rep="(none)",
        last_raw="(none)",
        last_msg="(none)",
        raw_channel="CH2",
        raw_lsb_v=ADS_LSB_V_GAIN6,
        rep_count=0,
        start_wall=time.time(),
        # Latest board t_ms from LIVE or RAW -- used as the time anchor for MARKs.
        last_t_ms=None,
        current_label="Rest",
        last_mark="(none yet -- press 1/2/3/4/5/6/0)",
        marks=[],   # list of (t_ms, label) for in-memory display
        last_infer_ms=None,
        infer_status="OFF" if pc_action_model is None else "warming up",
        infer_last_line="",
    )

    def maybe_run_pc_inference(latest_t_ms: int):
        if pc_action_model is None or pc_fatigue_model is None:
            return
        last_ms = state["last_infer_ms"]
        hop_ms = int(round(args.infer_hop * 1000.0))
        if last_ms is not None and latest_t_ms - last_ms < hop_ms:
            return

        win_ms = int(round(args.infer_window * 1000.0))
        start_ms = latest_t_ms - win_ms
        rows = [r for r in live_history if r["t_ms"] >= start_ms]
        min_rows = max(3, int(round(args.infer_window * LIVE_FS_HZ * 0.6)))
        if len(rows) < min_rows:
            state["infer_status"] = f"warming up {len(rows)}/{min_rows}"
            return

        features = compute_live_model_features(rows)
        if features is None:
            state["infer_status"] = "feature error"
            return
        try:
            pred = predict_pc_models(features, pc_action_model, pc_fatigue_model)
        except Exception as e:
            state["infer_status"] = f"predict error: {e}"
            return

        state["last_infer_ms"] = latest_t_ms
        action_note = ""
        if pred["raw_action"] == "LegExt":
            action_note = " (no DL model)"
        state["infer_status"] = (
            f"{pred['action_label']} {pred['action_confidence']:.0%}{action_note} | "
            f"{pred['fatigue_label']} Pfat={pred['fatigued_prob']:.0%}"
        )
        state["infer_last_line"] = (
            f"PRED,{latest_t_ms},{pred['action_label']},{pred['raw_action']},"
            f"{pred['action_confidence']:.4f},{pred['squat_prob']:.4f},"
            f"{pred['deadlift_prob']:.4f},{pred['walking_prob']:.4f},"
            f"{pred['legext_prob']:.4f},{pred['fatigue_label']},"
            f"{pred['fatigued_prob']:.4f},{features['duration_s']:.3f},"
            f"{features['live_rows']}"
        )
        if log_writer is not None:
            log_writer.write(state["infer_last_line"])

    def handle_line(line: str):
        d = parse_live(line)
        if d is None:
            raw = parse_raw(line)
            if raw is not None:
                raw_t_ms, raw_emg = raw
                state["n_raw"] += 1
                state["last_t_ms"] = raw_t_ms
                if state["t0"] is None:
                    state["t0"] = raw_t_ms
                t = (raw_t_ms - state["t0"]) / 1000.0
                raw_mv = raw_emg * state["raw_lsb_v"] * 1000
                raw_dc_win.append(raw_mv)
                raw_ac_mv = raw_mv - float(np.mean(raw_dc_win))
                buf_ac.append(t, raw_ac_mv)
                raw_mav_win.append(raw_ac_mv)
                if len(raw_mav_win) >= 4:
                    raw_arr = np.asarray(raw_mav_win)
                    raw_mav_mv = float(np.mean(np.abs(raw_arr)))
                    buf_mav.append(t, raw_mav_mv)
                state["last_raw"] = f"RAW,{raw_t_ms},{raw_emg}"
                return
            if line.startswith("RAW_BEGIN") or line.startswith("RAW_CHANNEL") or line.startswith("RAW_STATUS"):
                if "CH1" in line:
                    state["raw_channel"] = "CH1"
                    state["raw_lsb_v"] = ADS_LSB_V_GAIN2
                elif "CH2" in line:
                    state["raw_channel"] = "CH2"
                    state["raw_lsb_v"] = ADS_LSB_V_GAIN6
                state["last_msg"] = line
                state["n_other"] += 1
                return
            if line.startswith("CAL,"):
                state["last_cal"] = line
                cal = parse_cal(line)
                if cal:
                    if "rest_mav" in cal:
                        state["rest_mav_mv"] = cal["rest_mav"] * ADS_LSB_V * 1000
                    if "active_th" in cal:
                        state["active_th_mv"] = cal["active_th"] * ADS_LSB_V * 1000
            elif line.startswith("REP,"):
                state["last_rep"] = line
                state["rep_count"] += 1
            else:
                state["last_msg"] = line
                if line.strip():
                    print(f">> {line}")
            state["n_other"] += 1
            return

        state["n_live"] += 1
        if state["t0"] is None:
            state["t0"] = d["t_ms"]
        state["last_t_ms"] = d["t_ms"]
        t = (d["t_ms"] - state["t0"]) / 1000.0

        mav_mv = d["mav"] * ADS_LSB_V * 1000
        ac_mv  = d["ac"]  * ADS_LSB_V * 1000
        ang    = d["angle_x10"] / 10.0
        buf_mav.append(t, mav_mv)
        buf_ac.append(t, ac_mv)
        buf_angle.append(t, ang)
        if "ax" in d:
            acc_g  = np.sqrt(d["ax"]**2 + d["ay"]**2 + d["az"]**2) * ACC_LSB_G
            gyro_dps = np.sqrt(d["gx"]**2 + d["gy"]**2 + d["gz"]**2) * GYRO_LSB_DPS
            buf_acc.append(t, acc_g)
            buf_gyro.append(t, gyro_dps / 100.0)

        live_history.append(d)
        maybe_run_pc_inference(d["t_ms"])

        state["last_phase"] = (f"{d['emg_phase']:<8s} {d['emg_status']:<5s}  "
                               f"mot={d['mot_phase']}")

    def update(_):
        # Drain queue.
        drained = 0
        while drained < 500:
            try:
                line = q.get_nowait()
            except queue.Empty:
                break
            drained += 1
            handle_line(line)

        # Update line data.
        t_arr, v_arr = buf_mav.arrays()
        line_mav.set_data(t_arr, v_arr)
        if len(t_arr):
            x_hi = max(plot_window, t_arr[-1] + 0.5)
            x_lo = max(0, t_arr[-1] - plot_window)
            ax_mav.set_xlim(x_lo, x_hi)
            ymax = max(float(np.percentile(v_arr, 99)) * 1.4, 0.05)
            if state["active_th_mv"]:
                ymax = max(ymax, state["active_th_mv"] * 1.2)
            ax_mav.set_ylim(0, ymax)

            # Draw baseline / threshold horizontal lines
            if state["active_th_mv"]:
                line_active_th.set_data([x_lo, x_hi],
                                        [state["active_th_mv"]] * 2)
            if state["rest_mav_mv"]:
                line_rest_mav.set_data([x_lo, x_hi],
                                       [state["rest_mav_mv"]] * 2)

        t_arr, v_arr = buf_ac.arrays()
        line_ac.set_data(t_arr, v_arr)
        if len(t_arr):
            ax_ac.set_xlim(max(0, t_arr[-1] - RAW_WIN_S),
                           max(RAW_WIN_S, t_arr[-1] + 0.5))
            amax = max(float(np.percentile(np.abs(v_arr), 99)) * 1.5, 0.05)
            ax_ac.set_ylim(-amax, amax)

        t_arr, v_arr = buf_angle.arrays()
        line_angle.set_data(t_arr, v_arr)
        if len(t_arr):
            ax_angle.set_xlim(max(0, t_arr[-1] - plot_window),
                              max(plot_window, t_arr[-1] + 0.5))
            vmin, vmax = float(v_arr.min()), float(v_arr.max())
            pad = max(5.0, (vmax - vmin) * 0.1)
            ax_angle.set_ylim(vmin - pad, vmax + pad)

        t_a, v_a = buf_acc.arrays()
        t_g, v_g = buf_gyro.arrays()
        line_acc.set_data(t_a, v_a)
        line_gyro.set_data(t_g, v_g)
        if len(t_a):
            ax_imu.set_xlim(max(0, t_a[-1] - plot_window),
                            max(plot_window, t_a[-1] + 0.5))
            vmax = 1.5
            if len(v_a): vmax = max(vmax, float(v_a.max()))
            if len(v_g): vmax = max(vmax, float(v_g.max()))
            ax_imu.set_ylim(0, vmax * 1.1)

        # Status text panels.
        elapsed = time.time() - state["start_wall"]
        rest_str   = f"{state['rest_mav_mv']:.3f} mV" if state["rest_mav_mv"] else "-"
        active_str = f"{state['active_th_mv']:.3f} mV" if state["active_th_mv"] else "-"
        status_txt.set_text(
            f"phase     : {state['last_phase']}\n"
            f"wall time : {elapsed:6.1f} s\n"
            f"LIVE rows : {state['n_live']:>6}\n"
            f"RAW rows  : {state['n_raw']:>6}\n"
            f"RAW ch    : {state['raw_channel']}\n"
            f"other     : {state['n_other']:>6}\n"
            f"reps seen : {state['rep_count']:>6}\n"
            f"\n"
            f"rest_mav  : {rest_str}\n"
            f"active_th : {active_str}\n"
            f"\n"
            f"PC INFER  : {state['infer_status']}\n"
            f"\n"
            f"LABEL     : {state['current_label']:<14}\n"
            f"keys: 1=Squat 2=Walking 3=Running\n"
            f"      4=Deadlift 5=Fresh 6=Fatigued\n"
            f"      0=Rest\n"
        )

        cal_txt.set_text("LAST CAL:\n" + _wrap(state["last_cal"], 38))
        rep_txt.set_text("LAST REP:\n" + _wrap(state["last_rep"], 38))
        msg_txt.set_text(f"LAST MARK ({len(state['marks'])} total):\n"
                         + _wrap(state["last_mark"], 38)
                         + "\n\nLAST RAW:\n"
                         + _wrap(state["last_raw"], 38)
                         + "\n\nLAST MSG:\n"
                         + _wrap(state["last_msg"], 38)
                         + "\n\nLAST PRED:\n"
                         + _wrap(state["infer_last_line"], 38))

        return (line_mav, line_active_th, line_rest_mav, line_ac,
                line_angle, line_acc, line_gyro)

    def _wrap(s, width):
        if not s: return ""
        out = []
        for chunk_start in range(0, len(s), width):
            out.append(s[chunk_start:chunk_start + width])
        return "\n".join(out[:6])     # cap to 6 lines

    def on_key(event):
        """Keyboard handler: digit -> emit a MARK row into the log with the
        latest known board t_ms. Only meaningful in live mode (replay mode
        skips writing because we have no log_writer)."""
        key = (event.key or "").strip()
        if key not in LABEL_KEYS:
            return
        label = LABEL_KEYS[key]
        # Use the most recent board timestamp as the time anchor. If no LIVE
        # row has been seen yet, anchor at 0 and let later analysis ignore it.
        t_ms = state["last_t_ms"] if state["last_t_ms"] is not None else 0
        mark_line = f"MARK,{t_ms},{label}"
        if log_writer is not None:
            log_writer.write(mark_line)
        state["current_label"] = label
        state["last_mark"] = mark_line
        state["marks"].append((t_ms, label))
        # Print to console as immediate feedback so the user knows the press
        # registered (matplotlib's window does not always have visible focus
        # cues).
        print(f"[mark] {mark_line}")

    fig.canvas.mpl_connect("key_press_event", on_key)

    ani = animation.FuncAnimation(fig, update, interval=100, blit=False,
                                  cache_frame_data=False)

    try:
        plt.show()
    finally:
        if ser is not None and args.cmd == "live" and args.raw_fast:
            try:
                send_msh_command(ser, "raw_stop")
                send_msh_command(ser, "raw_baud_115200")
                ser.baudrate = args.baud
            except Exception as e:
                print(f"[monitor] raw fast stop failed: {e}")
        elif ser is not None and args.cmd == "live" and (args.raw_start or args.raw_only):
            try:
                send_msh_command(ser, "raw_stop")
            except Exception as e:
                print(f"[monitor] raw_stop failed: {e}")
        stop_evt.set()
        try:
            t.join(timeout=1.0)
        except Exception:
            pass
        if ser is not None:
            ser.close()
        if log_path is not None:
            print(f"\n[monitor] log saved: {log_path}")
            print(f"[monitor] marks recorded: {len(state['marks'])}")
            try:
                exported = export_raw_features_from_log(log_path)
            except Exception as e:
                exported = None
                print(f"[monitor] RAW feature export failed: {e}")
            if exported is not None:
                print(f"[monitor] RAW samples exported: {exported['raw_csv']}")
                print(f"[monitor] RAW features exported: {exported['feature_csv']}")
                print(f"[monitor] RAW rows={exported['raw_count']}, feature windows={exported['feature_count']}")


if __name__ == "__main__":
    main()
