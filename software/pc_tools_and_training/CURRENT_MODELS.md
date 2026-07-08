# Current Model Selection

## Action model

Current default:

`D:\mpu_wave\rt_thread_project\test_pro3\python\kneepad_action_rf.joblib`

Classes:

- `LegExt`
- `Squat`
- `Walking`

Note: this external KneE-PAD model does not include `Deadlift`. It can be used
for rough `Squat` / `Walking` testing now. Train
`action_squat_deadlift_walking_rf.joblib` after collecting enough `Squat`,
`Deadlift`, and `Walking` windows.

## Fatigue model

Current default:

`D:\mpu_wave\training_curated\fatigue_emg_balanced_sessions.joblib`

Classes:

- `0` = Fresh
- `1` = Fatigued

Selection reason:

Among the currently available fatigue models, this model had the highest direct
accuracy on the curated balanced Fresh/Fatigued dataset:

- Accuracy: about 97.7%
- F1: about 97.6%
- AUC: about 99.8%

Important caveat: strict leave-one-session-out validation is still much lower,
so the model is usable for demo/recognition, but more standardized fatigue
collection is still needed for a robust final product.
