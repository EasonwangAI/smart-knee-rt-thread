"""Dual-function prediction system: parallel action + fatigue detection.

Function 1: Action classification
            - Target project classes: Squat / Deadlift / Walking
            - The current external KneE-PAD model has Squat / LegExt / Walking;
              collect Deadlift data and train action_squat_deadlift_walking_rf.joblib
              before using Deadlift as a reliable output.

Function 2: Fatigue detection using your custom EMG-only model
            - Fresh / Fatigued
            - Always runs in parallel (independent of action)

Both functions run on the same EMG features but produce independent results.

Usage:
    python dual_predict.py
"""
from __future__ import annotations

import warnings
import numpy as np
from joblib import load
from pathlib import Path

warnings.filterwarnings('ignore', category=UserWarning)


# Keep legacy external labels explicit; do not silently map LegExt to Deadlift.
ACTION_OUTPUT_MAP = {
    'Squat': 'Squat',
    'Deadlift': 'Deadlift',
    'Walking': 'Walking',
    'LegExt': 'LegExt',
}


class DualPredictor:
    """Parallel dual-function predictor: Action + Fatigue."""

    def __init__(self, action_model_path, fatigue_model_path):
        """Load both models.

        Args:
            action_model_path: Path to KneE-PAD action classifier
            fatigue_model_path: Path to your EMG-only fatigue model
        """
        self.action_model = load(action_model_path)
        self.fatigue_model = load(fatigue_model_path)

        print(f"[OK] Action model loaded")
        print(f"     Original classes: {list(self.action_model.classes_)}")
        if 'Deadlift' not in [str(c) for c in self.action_model.classes_]:
            print(f"     WARNING: no Deadlift class in this model")
        print()
        print(f"[OK] Fatigue model loaded")
        print(f"     Classes: ['Fresh', 'Fatigued']")
        print()

    @staticmethod
    def _compute_derived_features(mav, rms, wl, iemg, var, zc, ssc):
        """Compute 5 derived EMG features for fatigue model."""
        return [
            zc / (mav + 1e-9),
            ssc / (rms + 1e-9),
            rms / (mav + 1e-9),
            wl / (iemg + 1e-9),
            var / ((rms ** 2) + 1e-9),
        ]

    def predict_action(self, mav, rms, wl, iemg, var, zc, ssc,
                       swing, tilt_std, tilt_p95, tilt_zc,
                       gyro_mean, gyro_max, gyro_std, acc_std):
        """Function 1: Action classification with merged labels.

        Returns:
            dict: {
                'raw_action': Model prediction,
                'action': Display action label,
                'confidence': Prediction confidence (0.0 to 1.0),
                'probs': Probabilities per merged class
            }
        """
        features = np.array([[
            mav, rms, wl, iemg, var, zc, ssc,
            swing, tilt_std, tilt_p95, tilt_zc,
            gyro_mean, gyro_max, gyro_std, acc_std
        ]])

        raw_action = self.action_model.predict(features)[0]
        raw_probs = self.action_model.predict_proba(features)[0]

        class_probs = {str(cls): float(p) for cls, p in zip(self.action_model.classes_, raw_probs)}
        raw_action = str(raw_action)
        action = ACTION_OUTPUT_MAP.get(raw_action, raw_action)
        confidence = class_probs.get(raw_action, float(np.max(raw_probs)))

        return {
            'raw_action': raw_action,
            'action': action,
            'confidence': float(confidence),
            'probs': class_probs,
        }

    def predict_fatigue(self, mav, rms, wl, iemg, var, zc, ssc):
        """Function 2: Fatigue detection (always runs).

        Returns:
            dict: {
                'fatigue': 'Fresh' or 'Fatigued',
                'probability': Probability of being fatigued (0.0 to 1.0)
            }
        """
        derived = self._compute_derived_features(mav, rms, wl, iemg, var, zc, ssc)

        features = np.array([[
            mav, rms, wl, iemg, var, zc, ssc,
            derived[0], derived[1], derived[2], derived[3], derived[4]
        ]])

        prediction = self.fatigue_model.predict(features)[0]
        probability = self.fatigue_model.predict_proba(features)[0, 1]

        return {
            'fatigue': 'Fatigued' if prediction == 1 else 'Fresh',
            'probability': float(probability),
        }

    def predict(self, mav, rms, wl, iemg, var, zc, ssc,
                swing, tilt_std, tilt_p95, tilt_zc,
                gyro_mean, gyro_max, gyro_std, acc_std):
        """Run both functions in parallel.

        Returns:
            dict: {
                'action': {...},   # Function 1 result
                'fatigue': {...},  # Function 2 result
            }
        """
        action_result = self.predict_action(
            mav, rms, wl, iemg, var, zc, ssc,
            swing, tilt_std, tilt_p95, tilt_zc,
            gyro_mean, gyro_max, gyro_std, acc_std
        )

        fatigue_result = self.predict_fatigue(
            mav, rms, wl, iemg, var, zc, ssc
        )

        return {
            'action': action_result,
            'fatigue': fatigue_result,
        }


def format_result(result):
    """Pretty-print a dual prediction result."""
    action = result['action']
    fatigue = result['fatigue']

    lines = []
    lines.append(f"  Action:  {action['action']:<12} "
                 f"(confidence: {action['confidence']:.1%}, "
                 f"raw: {action['raw_action']})")
    lines.append(f"  Fatigue: {fatigue['fatigue']:<12} "
                 f"(probability: {fatigue['probability']:.1%})")
    return "\n".join(lines)


def main():
    action_model_path = Path(r"D:\mpu_wave\rt_thread_project\test_pro3\python\kneepad_action_rf.joblib")
    fatigue_model_path = Path(r"D:\mpu_wave\training_curated\fatigue_emg_balanced_sessions.joblib")

    if not action_model_path.exists():
        print(f"[ERROR] Action model not found: {action_model_path}")
        return
    if not fatigue_model_path.exists():
        print(f"[ERROR] Fatigue model not found: {fatigue_model_path}")
        return

    predictor = DualPredictor(action_model_path, fatigue_model_path)

    print("=" * 70)
    print("Dual-Function Prediction Examples")
    print("=" * 70)
    print()

    # Example 1: Fresh leg movement
    print("[Example 1] Fresh leg movement (low amplitude EMG)")
    print("-" * 70)
    result = predictor.predict(
        mav=2500, rms=2000, wl=400000, iemg=640000, var=3000000, zc=5, ssc=3,
        swing=45.0, tilt_std=12.0, tilt_p95=40.0, tilt_zc=3,
        gyro_mean=150.0, gyro_max=250.0, gyro_std=50.0, acc_std=0.3
    )
    print(format_result(result))
    print()

    # Example 2: Fatigued leg movement
    print("[Example 2] Fatigued leg movement (high amplitude EMG)")
    print("-" * 70)
    result = predictor.predict(
        mav=3500, rms=3000, wl=550000, iemg=896000, var=6000000, zc=3, ssc=2,
        swing=45.0, tilt_std=12.0, tilt_p95=40.0, tilt_zc=3,
        gyro_mean=150.0, gyro_max=250.0, gyro_std=50.0, acc_std=0.3
    )
    print(format_result(result))
    print()

    # Example 3: Walking
    print("[Example 3] Walking (high frequency, low amplitude)")
    print("-" * 70)
    result = predictor.predict(
        mav=1500, rms=1200, wl=250000, iemg=384000, var=1000000, zc=8, ssc=6,
        swing=20.0, tilt_std=8.0, tilt_p95=18.0, tilt_zc=12,
        gyro_mean=80.0, gyro_max=120.0, gyro_std=30.0, acc_std=0.5
    )
    print(format_result(result))
    print()

    print("=" * 70)
    print("System Description")
    print("=" * 70)
    print("Function 1: Action Classification")
    print("  - Target: Squat / Deadlift / Walking")
    print("  - Current external model may still output LegExt until Deadlift data is trained")
    print("  - Uses 15 features (EMG 7 + IMU 8)")
    print()
    print("Function 2: Fatigue Detection (your custom model)")
    print("  - Fresh / Fatigued")
    print("  - Always runs in parallel")
    print("  - Uses 12 features (EMG 7 + derived 5)")
    print("=" * 70)


if __name__ == "__main__":
    main()
