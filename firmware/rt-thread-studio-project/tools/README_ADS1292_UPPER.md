# ADS1292 Upper Computer

## Board Output

The board firmware prints one CSV-style line per sampled point:

```text
ADS1292,seq,breath,ecg
```

The fusion firmware prints MPU6050 + ADS1292R + fatigue/action data:

```text
FIT,seq,emg,angle_x10,ax,ay,az,gx,gy,gz,fatigue,rms,zc,phase,action,count,force,status
```

Example:

```text
ADS1292,0,8388607,649000
ADS1292,1,8388607,649711
```

The Python tool also accepts the old debug format:

```text
breath=8388607, ecg=649000
```

## Install

```powershell
pip install -r tools\requirements.txt
```

## Run

Replace `COM3` with the serial port used by RT-Thread Studio:

GUI version:

```powershell
python tools\ads1292_gui.py
```

Command-line plotter:

```powershell
python tools\ads1292_upper.py COM3 --baud 115200
```

Optional:

```powershell
python tools\ads1292_upper.py COM3 --baud 115200 --window 1000 --csv ads1292_capture.csv
```

## Notes

- Close RT-Thread Studio's serial terminal before opening the Python tool, because only one program can usually occupy the same COM port.
- The default board output divider is `ADS1292_OUTPUT_DIV = 5` in `applications/main.c`, which reduces serial bandwidth usage.
- CSV data is saved continuously while the plot window is open.
