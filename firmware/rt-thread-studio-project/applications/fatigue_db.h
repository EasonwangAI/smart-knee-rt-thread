#ifndef __FATIGUE_DB_H__
#define __FATIGUE_DB_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t t_ms;
    int32_t  ac;
    uint32_t rms;
    uint32_t mav;
    uint32_t wl;
    uint16_t zc;
    uint16_t ssc;
    uint16_t zcr_x1000;
    uint16_t fatigue_score;
    uint8_t  fatigue_alert;
    uint8_t  active;
    const char *emg_phase;
    const char *emg_status;
    const char *motion_phase;
    int32_t  angle_x10;
    int16_t  ax;
    int16_t  ay;
    int16_t  az;
    int16_t  gx;
    int16_t  gy;
    int16_t  gz;
} fatigue_db_live_record_t;

typedef struct
{
    uint32_t rep_n;
    const char *action;
    uint32_t depth_x10;
    uint32_t descend_ms;
    uint32_t bottom_ms;
    uint32_t ascend_ms;
    uint64_t iemg;
    uint64_t mean_mav;
    uint64_t mean_rms;
    uint64_t mean_wl;
    uint32_t peak_rms;
    uint32_t peak_fatigue;
    uint32_t quality_score;
    const char *quality_label;
    uint32_t fail_mask;
    uint32_t base_mav;
} fatigue_db_rep_record_t;

typedef struct
{
    rt_bool_t matched;
    char person_id[24];
    char action[16];
    uint32_t score;
    uint32_t base_rms;
    uint32_t base_mav;
    uint32_t base_wl;
    uint16_t base_zc;
    uint16_t base_ssc;
    uint16_t fatigue_light;
    uint16_t fatigue_mid;
    uint16_t fatigue_heavy;
} fatigue_db_match_t;

typedef struct
{
    rt_bool_t matched;
    char mode[16];
    char source_family[20];
    char person_id[24];
    char unit_id[48];
    char label[20];
    char fatigue_level[12];
    uint32_t distance_x1000;
    uint32_t reference_distance_x1000;
    uint32_t rows_scanned;
    uint32_t rows_eligible;
    uint16_t top_k;
    uint16_t known_votes;
    uint16_t database_score;
    uint16_t confidence;
} fatigue_db_runtime_match_t;

void fatigue_db_init(void);
rt_bool_t fatigue_db_ready(void);
void fatigue_db_set_user(const char *user_id);
const char *fatigue_db_user(void);
void fatigue_db_start_session(const char *action_hint);
void fatigue_db_stop_session(void);
void fatigue_db_log_live(const fatigue_db_live_record_t *rec);
void fatigue_db_log_rep(const fatigue_db_rep_record_t *rec);
void fatigue_db_flush(void);
rt_err_t fatigue_db_match_current(const char *action_hint, fatigue_db_match_t *out);
void fatigue_db_get_match(fatigue_db_match_t *out);
void fatigue_db_get_runtime_match(fatigue_db_runtime_match_t *out);
uint16_t fatigue_db_fuse_score(uint16_t model_score, rt_bool_t *used);

#ifdef __cplusplus
}
#endif

#endif /* __FATIGUE_DB_H__ */
