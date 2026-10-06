#ifndef __EMG_PIPELINE_H__
#define __EMG_PIPELINE_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sampling and windowing -- all derived from the on-board ADS1292R config. */
#define EMG_SAMPLE_RATE         500
#define EMG_WINDOW_SAMPLES      256
#define EMG_HOP_SAMPLES         64

#define EMG_REST_CAL_WINDOWS        20
#define EMG_ACTIVE_CAL_WINDOWS      20

#define EMG_FATIGUE_SCORE_THRESHOLD 55
#define EMG_FATIGUE_HOLD_WINDOWS    2

typedef enum
{
    EMG_PHASE_REST_CAL = 0,
    EMG_PHASE_ACTIVE_CAL,
    EMG_PHASE_RUNNING,
} emg_phase_t;

typedef enum
{
    EMG_STATUS_CAL = 0,
    EMG_STATUS_REST,
    EMG_STATUS_OK,
    EMG_STATUS_ALERT,
} emg_status_t;

typedef struct
{
    uint32_t window_count;
    uint32_t sample_count;

    int32_t  last_ac;
    uint32_t rms;
    uint32_t mav;
    uint32_t wl;
    uint16_t zc;
    uint16_t ssc;
    uint16_t zcr_x1000;

    uint16_t fatigue_score;
    uint8_t  fatigue_alert;
    uint8_t  active;

    emg_phase_t  phase;
    emg_status_t status;

    uint32_t rest_rms;
    uint32_t rest_mav;
    uint32_t rest_wl;
    uint16_t rest_zc;
    uint16_t rest_ssc;

    uint32_t base_rms;
    uint32_t base_mav;
    uint32_t base_wl;
    uint16_t base_zc;
    uint16_t base_ssc;

    uint32_t zc_threshold;
    uint32_t ssc_threshold;
    uint32_t active_threshold;
} emg_snapshot_t;

typedef struct
{
    uint64_t iemg;
    uint64_t sum_window_rms;
    uint64_t sum_window_mav;
    uint64_t sum_window_wl;
    uint32_t window_count;
    uint32_t peak_abs_ac;
    uint32_t peak_rms;
    uint32_t peak_mav;
    uint32_t peak_wl;
    uint32_t peak_fatigue;
    uint32_t sample_count;
} emg_rep_features_t;

void emg_pipeline_init(void);
void emg_pipeline_get_snapshot(emg_snapshot_t *out);

void emg_pipeline_rep_start(void);
void emg_pipeline_rep_stop(emg_rep_features_t *out);
void emg_pipeline_rep_cancel(void);

void emg_pipeline_cal_reset(void);
void emg_pipeline_active_cal_force(void);

void emg_pipeline_raw_output_set(rt_bool_t enable);
rt_bool_t emg_pipeline_raw_output_get(void);
uint32_t emg_pipeline_raw_drop_count(void);
void emg_pipeline_raw_channel_set(uint8_t channel);
uint8_t emg_pipeline_raw_channel_get(void);

#ifdef __cplusplus
}
#endif

#endif /* __EMG_PIPELINE_H__ */
