#ifndef __FATIGUE_RF_MODEL_H__
#define __FATIGUE_RF_MODEL_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FATIGUE_RF_FEATURE_COUNT 12
#define FATIGUE_RF_TREE_COUNT 150
#define FATIGUE_RF_NODE_COUNT 15492
#define FATIGUE_RF_ALERT_THRESHOLD_Q100 50
#define FATIGUE_RF_PREALERT_THRESHOLD_Q100 45

uint8_t fatigue_rf_predict_proba_q100(const float features[FATIGUE_RF_FEATURE_COUNT]);

#ifdef __cplusplus
}
#endif

#endif /* __FATIGUE_RF_MODEL_H__ */
