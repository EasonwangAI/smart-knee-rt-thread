# Model Deployment Notes

## Fatigue Model Burned Into Firmware

Current board-side fatigue model:

- Source PC model: `python/fatigue_emg_selected_pair_sessions.joblib`
- Training windows: `python/fatigue_selected_pair_sessions.csv`
- Training summary: `python/selected_pair_sessions_training_summary.json`
- Generated firmware model:
  - `applications/fatigue_rf_model.c`
  - `applications/fatigue_rf_model.h`

Model structure:

- RandomForest
- 150 trees
- 15492 total nodes
- 12 EMG fatigue features
- Output: fatigue probability, 0-100

The firmware does not load the `.joblib` file. The `.joblib` is kept only as the
source model record. The actual burned model is the compact C array model in
`applications/fatigue_rf_model.c`.

## Current Alarm Threshold

The board uses:

- `EMG_FATIGUE_SCORE_THRESHOLD = 50`
- `EMG_FATIGUE_HOLD_WINDOWS = 3`

This keeps the model decision threshold at the standard 50% probability, so the
accuracy is not intentionally lowered. The hold window is short, so the warning
is still not hard to trigger once the model probability crosses the threshold.

## Feature Window

The board aggregates about 3 seconds of active EMG feature windows before
running the RF model. This matches the training and monitor-side feature window
more closely than the older single-window heuristic.

Feature order in the generated model:

1. `mav`
2. `rms`
3. `wl`
4. `iemg`
5. `var`
6. `zc`
7. `ssc`
8. `zc_per_mav`
9. `ssc_per_rms`
10. `rms_mav_ratio`
11. `wl_per_iemg`
12. `var_per_rms2`

## SD Card Role

The SD card should be used as a database for:

- raw/feature logs from the user's own hardware
- per-user calibration records
- model/version metadata
- future threshold override values

The real-time model should stay compiled into Flash. Loading the full RF model
from SD card into STM32F407 RAM is not recommended because RAM is only 192 KB.

Current project config does not yet enable SDIO/SPI-MSD filesystem support:

- `CONFIG_RT_USING_SDIO` is off
- `CONFIG_RT_USING_SPI_MSD` is off

So SD database support is the next firmware step after confirming the burned
model builds and runs.
