
"""
ADS1292 GUI upper computer.

Install:
    pip install -r tools/requirements.txt

Run:
    python tools/ads1292_gui.py
"""

from __future__ import annotations

import csv
import queue
import re
import threading
import time
import tkinter as tk
from collections import deque
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import matplotlib.animation as animation
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure
import serial
from serial.tools import list_ports


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


class SerialReader(threading.Thread):
    def __init__(
        self,
        port: str,
        baud: int,
        out_queue: queue.Queue[tuple[int, int, int, int, int, int, str, int]],
        status_queue: queue.Queue[str],
    ) -> None:
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.out_queue = out_queue
        self.status_queue = status_queue
        self._stop_event = threading.Event()
        self._serial: serial.Serial | None = None
        self._fallback_seq = 0

    def stop(self) -> None:
        self._stop_event.set()
        if self._serial is not None:
            self._serial.close()

    def run(self) -> None:
        try:
            self._serial = serial.Serial(self.port, self.baud, timeout=0.1)
            self.status_queue.put(f"Connected: {self.port} @ {self.baud}")
            self._serial.write(b"\r\nfit_start\r\n")
        except serial.SerialException as exc:
            self.status_queue.put(f"Open serial failed: {exc}")
            return

        raw_line_count = 0
        while not self._stop_event.is_set():
            try:
                raw = self._serial.readline()
            except serial.SerialException as exc:
                self.status_queue.put(f"Serial read failed: {exc}")
                break

            if not raw:
                continue

            line = raw.decode("utf-8", errors="ignore")
            raw_line_count += 1
            try:
                parsed = parse_sample(line, self._fallback_seq)
            except ValueError:
                if raw_line_count <= 10:
                    self.status_queue.put(f"Unparsed: {line.strip()}")
                continue

            if parsed is None:
                if raw_line_count <= 10:
                    self.status_queue.put(f"Unparsed: {line.strip()}")
                continue

            seq, breath, ecg, fatigue, rms, zc, fatigue_status, mav = parsed
            self._fallback_seq = seq + 1
            self.out_queue.put((seq, breath, ecg, fatigue, rms, zc, fatigue_status, mav))

        if self._serial is not None:
            try:
                self._serial.write(b"\r\nfit_stop\r\n")
            except serial.SerialException:
                pass
        self.status_queue.put("Disconnected")


class Ads1292App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("ADS1292 Upper Computer")
        self.geometry("1100x720")
        self.minsize(900, 620)

        self.sample_queue: queue.Queue[tuple[int, int, int, int, int, int, str, int]] = queue.Queue()
        self.status_queue: queue.Queue[str] = queue.Queue()
        self.reader: SerialReader | None = None
        self.csv_file = None
        self.csv_writer: csv.writer | None = None

        self.window_size = tk.IntVar(value=500)
        self.baud = tk.IntVar(value=115200)
        self.port = tk.StringVar()
        self.csv_path = tk.StringVar(value=str(Path.cwd() / "ads1292_capture.csv"))
        self.status = tk.StringVar(value="Ready")
        self.latest = tk.StringVar(value="angle: --    raw: --    mav: --    fatigue: --")

        self.seq_data: deque[int] = deque(maxlen=self.window_size.get())
        self.breath_data: deque[int] = deque(maxlen=self.window_size.get())
        self.ecg_data: deque[int] = deque(maxlen=self.window_size.get())
        self.total_samples = 0

        self._build_ui()
        self._refresh_ports()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self) -> None:
        top = ttk.Frame(self, padding=(10, 8))
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(top, text="Port").pack(side=tk.LEFT)
        self.port_combo = ttk.Combobox(top, textvariable=self.port, width=14, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=(6, 10))

        ttk.Button(top, text="Refresh", command=self._refresh_ports).pack(side=tk.LEFT, padx=(0, 10))

        ttk.Label(top, text="Baud").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.baud, width=9).pack(side=tk.LEFT, padx=(6, 10))

        ttk.Label(top, text="Window").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.window_size, width=7).pack(side=tk.LEFT, padx=(6, 10))

        ttk.Button(top, text="CSV...", command=self._choose_csv).pack(side=tk.LEFT, padx=(0, 10))
        ttk.Button(top, text="Start", command=self._start).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(top, text="Stop", command=self._stop).pack(side=tk.LEFT)

        ttk.Label(top, textvariable=self.latest).pack(side=tk.RIGHT)

        fig = Figure(figsize=(10, 6), dpi=100)
        self.ax_breath = fig.add_subplot(211)
        self.ax_ecg = fig.add_subplot(212, sharex=self.ax_breath)

        self.breath_line, = self.ax_breath.plot([], [], color="#1677c9", linewidth=1.2)
        self.ecg_line, = self.ax_ecg.plot([], [], color="#c93232", linewidth=1.2)

        self.ax_breath.set_ylabel("Angle/Ch1")
        self.ax_ecg.set_ylabel("EMG MAV")
        self.ax_ecg.set_xlabel("Sample")
        self.ax_breath.grid(True, alpha=0.28)
        self.ax_ecg.grid(True, alpha=0.28)
        fig.tight_layout()

        self.canvas = FigureCanvasTkAgg(fig, master=self)
        self.canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        toolbar = NavigationToolbar2Tk(self.canvas, self, pack_toolbar=False)
        toolbar.update()
        toolbar.pack(side=tk.TOP, fill=tk.X)

        bottom = ttk.Frame(self, padding=(10, 6))
        bottom.pack(side=tk.BOTTOM, fill=tk.X)
        ttk.Label(bottom, textvariable=self.status).pack(side=tk.LEFT)
        ttk.Label(bottom, textvariable=self.csv_path).pack(side=tk.RIGHT)

        self.anim = animation.FuncAnimation(
            fig,
            self._update_plot,
            interval=50,
            blit=False,
            cache_frame_data=False,
        )

    def _refresh_ports(self) -> None:
        ports = [port.device for port in list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port.get():
            self.port.set(ports[0])
        self.status.set(f"Found {len(ports)} serial port(s)")

    def _choose_csv(self) -> None:
        path = filedialog.asksaveasfilename(
            title="Save CSV",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
            initialfile=Path(self.csv_path.get()).name,
        )
        if path:
            self.csv_path.set(path)

    def _start(self) -> None:
        if self.reader is not None:
            self.status.set("Already running")
            return

        if not self.port.get():
            messagebox.showwarning("ADS1292", "Please select a serial port.")
            return

        self._reset_data()
        self._open_csv()

        self.reader = SerialReader(
            self.port.get(),
            self.baud.get(),
            self.sample_queue,
            self.status_queue,
        )
        self.reader.start()
        self.status.set("Connecting...")

    def _stop(self) -> None:
        if self.reader is not None:
            self.reader.stop()
            self.reader = None
        self._close_csv()
        self.status.set("Stopped")

    def _reset_data(self) -> None:
        maxlen = max(10, self.window_size.get())
        self.seq_data = deque(maxlen=maxlen)
        self.breath_data = deque(maxlen=maxlen)
        self.ecg_data = deque(maxlen=maxlen)
        self.total_samples = 0

    def _open_csv(self) -> None:
        self._close_csv()
        path = Path(self.csv_path.get())
        path.parent.mkdir(parents=True, exist_ok=True)
        self.csv_file = path.open("w", newline="", encoding="utf-8")
        self.csv_writer = csv.writer(self.csv_file)
        self.csv_writer.writerow(["host_time", "seq", "angle_or_ch1", "raw_emg", "fatigue", "rms", "zc", "mav", "status"])

    def _close_csv(self) -> None:
        if self.csv_file is not None:
            self.csv_file.close()
        self.csv_file = None
        self.csv_writer = None

    def _update_plot(self, _frame: int):
        self._drain_status()
        self._drain_samples()

        if not self.seq_data:
            return self.breath_line, self.ecg_line

        x = list(self.seq_data)
        self.breath_line.set_data(x, list(self.breath_data))
        self.ecg_line.set_data(x, list(self.ecg_data))

        left = x[0]
        right = x[-1] if x[-1] > x[0] else x[0] + 1
        self.ax_breath.set_xlim(left, right)
        self.ax_breath.relim()
        self.ax_breath.autoscale_view(scalex=False, scaley=True)
        self.ax_ecg.relim()
        self.ax_ecg.autoscale_view(scalex=False, scaley=True)

        self.canvas.draw_idle()
        return self.breath_line, self.ecg_line

    def _drain_status(self) -> None:
        while True:
            try:
                self.status.set(self.status_queue.get_nowait())
            except queue.Empty:
                break

    def _drain_samples(self) -> None:
        wrote = False

        while True:
            try:
                seq, breath, ecg, fatigue, rms, zc, fatigue_status, mav = self.sample_queue.get_nowait()
            except queue.Empty:
                break

            now = time.time()
            self.seq_data.append(seq)
            self.breath_data.append(breath)
            self.ecg_data.append(mav if mav else ecg)
            self.total_samples += 1
            self.latest.set(
                f"angle: {breath}    raw: {ecg}    mav: {mav if mav else ecg}    fatigue: {fatigue} "
                f"{fatigue_status}    samples: {self.total_samples}"
            )

            if self.csv_writer is not None:
                self.csv_writer.writerow([f"{now:.3f}", seq, breath, ecg, fatigue, rms, zc, mav, fatigue_status])
                wrote = True

        if wrote and self.csv_file is not None:
            self.csv_file.flush()

    def _on_close(self) -> None:
        self._stop()
        self.destroy()


def main() -> int:
    app = Ads1292App()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
