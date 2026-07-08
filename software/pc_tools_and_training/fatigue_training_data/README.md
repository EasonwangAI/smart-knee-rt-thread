# fatigue_training_data

This folder was generated from `D:\mpu_wave\P03.mat`.

Recommended first training file:

```text
features\features_lg0_binary_1s_hop0p5.csv
```

It contains only the matched `LG_0` condition:

- `fatigue_label = 0`: `OriginalData/Ideal/LG_0`
- `fatigue_label = 1`: `OriginalData/Fatigue/LG_0`

The broader file below includes all usable original EMG trials from P03:

```text
features\features_all_conditions_1s_hop0p5.csv
```

Cycle-level features exported from `NormalizedData` are stored in:

```text
features\features_normalized_cycles.csv
```

Use these for gait-cycle experiments rather than as the first on-device
fatigue-training target.

Raw trial-level EMG is stored as compressed NumPy files in:

```text
raw_trials_npz\
```

Each `.npz` contains:

- `emg`: 13 x N raw EMG matrix
- `fs_hz`: 2000
- `fatigue_label`: 0 for Ideal, 1 for Fatigue
- `state`, `condition`, `subject`, `trial`

Feature extraction settings:

- window: 1.0 s
- hop: 0.5 s
- spectral band for MDF/MPF: 20-450 Hz

Use the `metadata\trial_manifest.csv` file to split by trial. For model
evaluation, split by trial or condition; do not randomly split windows from
the same trial into both train and test.
