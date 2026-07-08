"""Real-time fatigue prediction using the trained EMG-only model.

Usage:
    python predict_fatigue.py

This script loads the trained model and demonstrates how to predict
fatigue state from EMG features.
"""
from __future__ import annotations

import numpy as np
from joblib import load
from pathlib import Path


def compute_derived_features(mav, rms, wl, iemg, var, zc, ssc):
    """Compute the 5 derived features from the 7 base EMG features."""
    zc_per_mav = zc / (mav + 1e-9)
    ssc_per_rms = ssc / (rms + 1e-9)
    rms_mav_ratio = rms / (mav + 1e-9)
    wl_per_iemg = wl / (iemg + 1e-9)
    var_per_rms2 = var / ((rms ** 2) + 1e-9)

    return zc_per_mav, ssc_per_rms, rms_mav_ratio, wl_per_iemg, var_per_rms2


def predict_fatigue(model, mav, rms, wl, iemg, var, zc, ssc):
    """Predict fatigue state from EMG features.

    Args:
        model: Trained sklearn model
        mav, rms, wl, iemg, var, zc, ssc: Base EMG features from firmware

    Returns:
        prediction: 0 = Fresh, 1 = Fatigued
        probability: Probability of being fatigued (0.0 to 1.0)
    """
    # Compute derived features
    derived = compute_derived_features(mav, rms, wl, iemg, var, zc, ssc)

    # Combine all 12 features in the correct order
    features = np.array([[
        mav, rms, wl, iemg, var, zc, ssc,
        derived[0], derived[1], derived[2], derived[3], derived[4]
    ]])

    # Predict
    prediction = model.predict(features)[0]
    probability = model.predict_proba(features)[0, 1]

    return prediction, probability


def main():
    # Load the trained model
    model_path = Path(__file__).parent / "training_curated" / "fatigue_emg_balanced_sessions.joblib"
    if not model_path.exists():
        print(f"ERROR: Model not found at {model_path}")
        print("Please run the curated fatigue training first.")
        return

    model = load(model_path)
    print(f"Model loaded: {model_path}")
    print(f"Model type: {type(model).__name__}")
    print()

    # Example 1: Fresh state (typical values from your data)
    print("=" * 60)
    print("Example 1: Fresh State")
    print("=" * 60)
    mav_fresh = 2500
    rms_fresh = 2000
    wl_fresh = 400000
    iemg_fresh = 1200000
    var_fresh = 3000000
    zc_fresh = 5
    ssc_fresh = 3

    pred, prob = predict_fatigue(model, mav_fresh, rms_fresh, wl_fresh,
                                 iemg_fresh, var_fresh, zc_fresh, ssc_fresh)

    print(f"EMG Features:")
    print(f"  MAV={mav_fresh}, RMS={rms_fresh}, WL={wl_fresh}")
    print(f"  IEMG={iemg_fresh}, VAR={var_fresh}")
    print(f"  ZC={zc_fresh}, SSC={ssc_fresh}")
    print()
    print(f"Prediction: {'Fatigued' if pred == 1 else 'Fresh'}")
    print(f"Fatigue Probability: {prob:.1%}")
    print()

    # Example 2: Fatigued state (higher amplitude, lower frequency)
    print("=" * 60)
    print("Example 2: Fatigued State")
    print("=" * 60)
    mav_fatigued = 3500
    rms_fatigued = 3000
    wl_fatigued = 550000
    iemg_fatigued = 1800000
    var_fatigued = 6000000
    zc_fatigued = 3
    ssc_fatigued = 2

    pred, prob = predict_fatigue(model, mav_fatigued, rms_fatigued, wl_fatigued,
                                 iemg_fatigued, var_fatigued, zc_fatigued, ssc_fatigued)

    print(f"EMG Features:")
    print(f"  MAV={mav_fatigued}, RMS={rms_fatigued}, WL={wl_fatigued}")
    print(f"  IEMG={iemg_fatigued}, VAR={var_fatigued}")
    print(f"  ZC={zc_fatigued}, SSC={ssc_fatigued}")
    print()
    print(f"Prediction: {'Fatigued' if pred == 1 else 'Fresh'}")
    print(f"Fatigue Probability: {prob:.1%}")
    print()

    # Show feature importance
    print("=" * 60)
    print("Feature Importance (Top 5)")
    print("=" * 60)
    feature_names = [
        'mav', 'rms', 'wl', 'iemg', 'var', 'zc', 'ssc',
        'zc_per_mav', 'ssc_per_rms', 'rms_mav_ratio',
        'wl_per_iemg', 'var_per_rms2'
    ]
    importances = model.feature_importances_
    sorted_idx = np.argsort(importances)[::-1]

    for i in range(5):
        idx = sorted_idx[i]
        print(f"  {i+1}. {feature_names[idx]:<16} {importances[idx]:.4f}")


if __name__ == "__main__":
    main()
