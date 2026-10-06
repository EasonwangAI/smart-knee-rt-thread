#include "emg_pipeline.h"
#include "ads1292.h"
#include "fatigue_rf_model.h"

#include <math.h>
#include <rtdevice.h>
#include <stdlib.h>
#include <string.h>

#define DBG_TAG "emg_pipe"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define EMG_BIQUAD_COUNT            3

#define EMG_FATIGUE_RMS_WEIGHT      30
#define EMG_FATIGUE_MAV_WEIGHT      20
#define EMG_FATIGUE_WL_WEIGHT       20
#define EMG_FATIGUE_FREQ_WEIGHT     30
#define EMG_FATIGUE_RISE_PCT        110

#define EMG_ZC_THRESHOLD_FLOOR      200
#define EMG_ZC_THRESHOLD_CEIL       20000
#define EMG_SSC_THRESHOLD_FLOOR     150
#define EMG_SSC_THRESHOLD_CEIL      20000
#define EMG_ACTIVE_FLOOR            2000
#define EMG_ACTIVE_RATIO_PERCENT    200

#define EMG_MODEL_AGG_FRAMES        24
#define EMG_MODEL_RESET_INACTIVE_WINDOWS 16
#define EMG_MODEL_WINDOW_SAMPLES    1500.0f
#define EMG_MODEL_WL_SCALE          (EMG_MODEL_WINDOW_SAMPLES / (float)EMG_WINDOW_SAMPLES)
#define EMG_MODEL_ADS_LSB_MV        ((2.0f * 2.42f * 1000.0f) / (6.0f * 16777216.0f))
#define EMG_MODEL_EPS               1.0e-6f

#ifndef EMG_RAW_OUTPUT_DEFAULT_ENABLE
#define EMG_RAW_OUTPUT_DEFAULT_ENABLE 0
#endif

#define EMG_RAW_RING_SIZE           4096
#define EMG_RAW_THREAD_STACK_SIZE   2048
#define EMG_RAW_THREAD_PRIORITY     23
#define EMG_RAW_THREAD_TICK         10
#define EMG_RAW_PRINT_BUF_SIZE      384
#define EMG_RAW_FAST_BAUD           BAUD_RATE_921600
#define EMG_RAW_SLOW_BAUD           BAUD_RATE_115200

typedef struct
{
    float b0, b1, b2;
    float a1, a2;
} emg_biquad_coef_t;

typedef struct
{
    float s1, s2;
} emg_biquad_state_t;

static const emg_biquad_coef_t emg_biquad_coefs[EMG_BIQUAD_COUNT] =
{
    { 0.8370891906f, -1.6741783811f, 0.8370891906f, -1.6474599811f, 0.7008967812f },
    { 0.9902986180f, -1.6023368229f, 0.9902986180f, -1.6023368229f, 0.9805972359f },
    { 0.3913357725f, 0.7826715450f,  0.3913357725f, 0.3695273774f,  0.1958157127f },
};

static inline float biquad_df2t(emg_biquad_state_t *st, const emg_biquad_coef_t *c, float x)
{
    float y = c->b0 * x + st->s1;
    st->s1 = c->b1 * x - c->a1 * y + st->s2;
    st->s2 = c->b2 * x - c->a2 * y;
    return y;
}

typedef struct
{
    int32_t  ac_ring[EMG_WINDOW_SAMPLES];
    uint32_t abs_ring[EMG_WINDOW_SAMPLES];
    uint32_t diff_ring[EMG_WINDOW_SAMPLES];
    uint8_t  zc_ring[EMG_WINDOW_SAMPLES];

    uint64_t sum_sq;
    uint64_t sum_abs;
    uint64_t sum_diff;
    uint32_t sum_zc;

    int32_t  prev_ac;
    uint32_t pos;
    uint32_t valid;

    uint32_t hop_count;
    uint32_t sample_count;
    uint32_t window_count;
} emg_window_state_t;

typedef struct
{
    uint32_t rms_buf[EMG_REST_CAL_WINDOWS];
    uint32_t mav_buf[EMG_REST_CAL_WINDOWS];
    uint32_t wl_buf[EMG_REST_CAL_WINDOWS];
    uint16_t zc_buf[EMG_REST_CAL_WINDOWS];
    uint16_t ssc_buf[EMG_REST_CAL_WINDOWS];
    uint64_t var_sum;
    uint64_t var_n;
    uint32_t count;
} emg_rest_cal_t;

typedef struct
{
    uint32_t rms_buf[EMG_ACTIVE_CAL_WINDOWS];
    uint32_t mav_buf[EMG_ACTIVE_CAL_WINDOWS];
    uint32_t wl_buf[EMG_ACTIVE_CAL_WINDOWS];
    uint16_t zc_buf[EMG_ACTIVE_CAL_WINDOWS];
    uint16_t ssc_buf[EMG_ACTIVE_CAL_WINDOWS];
    uint32_t count;
} emg_active_cal_t;

typedef struct
{
    uint32_t sample_index;
    int32_t  emg_raw;
} emg_raw_sample_t;

typedef struct
{
    uint32_t rms;
    uint32_t mav;
    uint32_t wl;
    uint16_t zc;
    uint16_t ssc;
} emg_model_frame_t;

static emg_biquad_state_t   biquad_state[EMG_BIQUAD_COUNT];
static emg_window_state_t   win;
static emg_rest_cal_t       rest_cal;
static emg_active_cal_t     active_cal;

static emg_phase_t          phase;
static emg_status_t         status;
static uint32_t             alert_hold;

static uint32_t             zc_threshold;
static uint32_t             ssc_threshold;
static uint32_t             active_threshold;

static uint32_t             rest_rms, rest_mav, rest_wl;
static uint16_t             rest_zc, rest_ssc;
static uint32_t             base_rms, base_mav, base_wl;
static uint16_t             base_zc, base_ssc;

static emg_snapshot_t       snap_pub;
static emg_rep_features_t   rep_live;
static volatile rt_bool_t   rep_active;

static emg_model_frame_t    model_frames[EMG_MODEL_AGG_FRAMES];
static uint32_t             model_frame_pos;
static uint32_t             model_frame_count;
static uint16_t             model_inactive_windows;
static uint16_t             fatigue_state_score;

static emg_raw_sample_t     raw_ring[EMG_RAW_RING_SIZE];
static uint32_t             raw_head;
static uint32_t             raw_tail;
static uint32_t             raw_dropped;
static struct rt_semaphore  raw_sem;
static rt_thread_t          raw_thread = RT_NULL;
static rt_bool_t            raw_sem_ready;
static volatile rt_bool_t   raw_output_enable =
    EMG_RAW_OUTPUT_DEFAULT_ENABLE ? RT_TRUE : RT_FALSE;
static volatile uint8_t     raw_output_channel = 2;

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int u16_cmp(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return (int)x - (int)y;
}

static uint32_t median_u32(uint32_t *buf, uint32_t n)
{
    if (n == 0) return 0;
    qsort(buf, n, sizeof(uint32_t), u32_cmp);
    return buf[n / 2];
}

static uint16_t median_u16(uint16_t *buf, uint32_t n)
{
    if (n == 0) return 0;
    qsort(buf, n, sizeof(uint16_t), u16_cmp);
    return buf[n / 2];
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t clamp_u32(int32_t v, uint32_t lo, uint32_t hi)
{
    if (v < (int32_t)lo) return lo;
    if (v > (int32_t)hi) return hi;
    return (uint32_t)v;
}

static uint32_t emg_isqrt64(uint64_t v)
{
    if (v == 0) return 0;
    return (uint32_t)sqrt((double)v);
}

static void emg_model_reset(void)
{
    model_frame_pos = 0;
    model_frame_count = 0;
    model_inactive_windows = 0;
    fatigue_state_score = 0;
    rt_memset(model_frames, 0, sizeof(model_frames));
}

static void emg_model_push(uint32_t rms, uint32_t mav, uint32_t wl,
                           uint16_t zc, uint16_t ssc)
{
    model_frames[model_frame_pos].rms = rms;
    model_frames[model_frame_pos].mav = mav;
    model_frames[model_frame_pos].wl = wl;
    model_frames[model_frame_pos].zc = zc;
    model_frames[model_frame_pos].ssc = ssc;

    model_frame_pos = (model_frame_pos + 1U) % EMG_MODEL_AGG_FRAMES;
    if (model_frame_count < EMG_MODEL_AGG_FRAMES) model_frame_count++;
}

static uint16_t emg_score_fatigue_rf(void)
{
    if (model_frame_count < EMG_MODEL_AGG_FRAMES) return 0;

    uint64_t sum_mav = 0;
    uint64_t sum_wl = 0;
    uint64_t sum_rms_sq = 0;
    uint32_t sum_zc = 0;
    uint32_t sum_ssc = 0;

    for (uint32_t i = 0; i < EMG_MODEL_AGG_FRAMES; i++)
    {
        const emg_model_frame_t *f = &model_frames[i];
        sum_mav += f->mav;
        sum_wl += f->wl;
        sum_rms_sq += (uint64_t)f->rms * (uint64_t)f->rms;
        sum_zc += f->zc;
        sum_ssc += f->ssc;
    }

    const float n = (float)EMG_MODEL_AGG_FRAMES;
    const float mean_mav_counts = (float)sum_mav / n;
    const float mean_wl_counts = (float)sum_wl / n;
    const float mean_rms_sq_counts = (float)((double)sum_rms_sq / (double)EMG_MODEL_AGG_FRAMES);

    const float mav = mean_mav_counts * EMG_MODEL_ADS_LSB_MV;
    const float rms = sqrtf(mean_rms_sq_counts) * EMG_MODEL_ADS_LSB_MV;
    const float wl = mean_wl_counts * EMG_MODEL_WL_SCALE * EMG_MODEL_ADS_LSB_MV;
    const float iemg = mav * EMG_MODEL_WINDOW_SAMPLES;
    const float mean_rms_sq_mv = mean_rms_sq_counts * EMG_MODEL_ADS_LSB_MV * EMG_MODEL_ADS_LSB_MV;
    float var = mean_rms_sq_mv - mav * mav;
    if (var < 0.0f) var = 0.0f;

    const float zc = ((float)sum_zc / n) * EMG_MODEL_WL_SCALE;
    const float ssc = ((float)sum_ssc / n) * EMG_MODEL_WL_SCALE;

    float features[FATIGUE_RF_FEATURE_COUNT];
    features[0] = mav;
    features[1] = rms;
    features[2] = wl;
    features[3] = iemg;
    features[4] = var;
    features[5] = zc;
    features[6] = ssc;
    features[7] = zc / (mav + EMG_MODEL_EPS);
    features[8] = ssc / (rms + EMG_MODEL_EPS);
    features[9] = rms / (mav + EMG_MODEL_EPS);
    features[10] = wl / (iemg + EMG_MODEL_EPS);
    features[11] = var / ((rms * rms) + EMG_MODEL_EPS);

    return (uint16_t)fatigue_rf_predict_proba_q100(features);
}

static uint16_t emg_compute_ssc(void)
{
    uint32_t count = 0;
    if (win.valid < 3) return 0;

    uint32_t start = (win.pos + EMG_WINDOW_SAMPLES - win.valid) % EMG_WINDOW_SAMPLES;
    uint32_t prev_pos = start;
    int32_t  prev_val = win.ac_ring[prev_pos];
    uint32_t cur_pos = (prev_pos + 1) % EMG_WINDOW_SAMPLES;
    int32_t  cur_val = win.ac_ring[cur_pos];
    int32_t  prev_diff = cur_val - prev_val;

    for (uint32_t i = 2; i < win.valid; i++)
    {
        uint32_t next_pos = (cur_pos + 1) % EMG_WINDOW_SAMPLES;
        int32_t  next_val = win.ac_ring[next_pos];
        int32_t  cur_diff = next_val - cur_val;

        if ((prev_diff > 0 && cur_diff < 0) || (prev_diff < 0 && cur_diff > 0))
        {
            uint32_t abs_prev = (uint32_t)(prev_diff < 0 ? -prev_diff : prev_diff);
            uint32_t abs_cur  = (uint32_t)(cur_diff  < 0 ? -cur_diff  : cur_diff);
            if (abs_prev > ssc_threshold || abs_cur > ssc_threshold)
            {
                count++;
            }
        }
        prev_diff = cur_diff;
        cur_val = next_val;
        cur_pos = next_pos;
    }
    return (uint16_t)(count > 0xFFFFu ? 0xFFFFu : count);
}

static void emg_publish_snapshot(uint32_t rms, uint32_t mav, uint32_t wl,
                                 uint16_t zc, uint16_t ssc,
                                 uint16_t score, uint8_t alert, uint8_t active)
{
    emg_snapshot_t s;

    s.window_count    = win.window_count;
    s.sample_count    = win.sample_count;
    s.last_ac         = win.prev_ac;
    s.rms             = rms;
    s.mav             = mav;
    s.wl              = wl;
    s.zc              = zc;
    s.ssc             = ssc;
    s.zcr_x1000       = (uint16_t)(((uint32_t)zc * 1000U) / EMG_WINDOW_SAMPLES);
    s.fatigue_score   = score;
    s.fatigue_alert   = alert;
    s.active          = active;
    s.phase           = phase;
    s.status          = status;
    s.rest_rms        = rest_rms;
    s.rest_mav        = rest_mav;
    s.rest_wl         = rest_wl;
    s.rest_zc         = rest_zc;
    s.rest_ssc        = rest_ssc;
    s.base_rms        = base_rms;
    s.base_mav        = base_mav;
    s.base_wl         = base_wl;
    s.base_zc         = base_zc;
    s.base_ssc        = base_ssc;
    s.zc_threshold    = zc_threshold;
    s.ssc_threshold   = ssc_threshold;
    s.active_threshold = active_threshold;

    rt_base_t level = rt_hw_interrupt_disable();
    snap_pub = s;
    rt_hw_interrupt_enable(level);
}

static rt_bool_t emg_raw_dequeue(emg_raw_sample_t *out)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (raw_head == raw_tail)
    {
        rt_hw_interrupt_enable(level);
        return RT_FALSE;
    }

    *out = raw_ring[raw_tail];
    raw_tail = (raw_tail + 1U) % EMG_RAW_RING_SIZE;
    rt_hw_interrupt_enable(level);
    return RT_TRUE;
}

static void emg_raw_enqueue(uint32_t sample_index, int32_t emg_raw)
{
    if (!raw_output_enable || !raw_sem_ready) return;

    rt_base_t level = rt_hw_interrupt_disable();
    rt_bool_t wake = (raw_head == raw_tail) ? RT_TRUE : RT_FALSE;
    uint32_t next = (raw_head + 1U) % EMG_RAW_RING_SIZE;
    if (next == raw_tail)
    {
        raw_tail = (raw_tail + 1U) % EMG_RAW_RING_SIZE;
        raw_dropped++;
    }

    raw_ring[raw_head].sample_index = sample_index;
    raw_ring[raw_head].emg_raw = emg_raw;
    raw_head = next;
    rt_hw_interrupt_enable(level);

    if (wake) rt_sem_release(&raw_sem);
}

static void emg_raw_flush_print_buf(char *buf, uint32_t *used)
{
    if (*used == 0U) return;
    buf[*used] = '\0';
    rt_kprintf("%s", buf);
    *used = 0U;
}

static void emg_raw_thread_entry(void *parameter)
{
    (void)parameter;
    char out_buf[EMG_RAW_PRINT_BUF_SIZE + 1U];
    uint32_t used = 0;

    while (1)
    {
        emg_raw_sample_t s;
        if (!emg_raw_dequeue(&s))
        {
            emg_raw_flush_print_buf(out_buf, &used);
            rt_sem_take(&raw_sem, RT_WAITING_FOREVER);
            continue;
        }

        uint32_t t_ms = (uint32_t)(((uint64_t)s.sample_index * 1000ULL) /
                                   EMG_SAMPLE_RATE);
        char line[32];
        int n = rt_snprintf(line, sizeof(line), "RAW,%u,%d\n",
                            (unsigned int)t_ms, (int)s.emg_raw);
        if (n <= 0) continue;
        if ((uint32_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

        if (used + (uint32_t)n >= EMG_RAW_PRINT_BUF_SIZE)
        {
            emg_raw_flush_print_buf(out_buf, &used);
        }

        rt_memcpy(&out_buf[used], line, (rt_size_t)n);
        used += (uint32_t)n;
    }
}

static void emg_raw_start_thread(void)
{
    if (raw_thread != RT_NULL) return;

    if (!raw_sem_ready)
    {
        if (rt_sem_init(&raw_sem, "rawsem", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
        {
            rt_kprintf("RAW_ERR,sem_init\n");
            return;
        }
        raw_sem_ready = RT_TRUE;
    }

    raw_thread = rt_thread_create("rawprint",
                                  emg_raw_thread_entry,
                                  RT_NULL,
                                  EMG_RAW_THREAD_STACK_SIZE,
                                  EMG_RAW_THREAD_PRIORITY,
                                  EMG_RAW_THREAD_TICK);
    if (raw_thread != RT_NULL)
    {
        rt_thread_startup(raw_thread);
    }
    else
    {
        rt_kprintf("RAW_ERR,thread_create\n");
    }
}

static rt_err_t emg_console_set_baud(rt_uint32_t baud)
{
    rt_device_t console = rt_console_get_device();
    if (console == RT_NULL) return -RT_ERROR;

    struct serial_configure cfg = RT_SERIAL_CONFIG_DEFAULT;
    cfg.baud_rate = baud;
    return rt_device_control(console, RT_DEVICE_CTRL_CONFIG, &cfg);
}

static void emg_raw_switch_baud_delayed(rt_uint32_t baud)
{
    rt_kprintf("RAW_BAUD,%u\n", (unsigned int)baud);
    rt_thread_mdelay(500);
    if (emg_console_set_baud(baud) != RT_EOK)
    {
        rt_kprintf("RAW_ERR,baud=%u\n", (unsigned int)baud);
    }
    rt_thread_mdelay(100);
}

static void emg_finalize_rest_baseline(void)
{
    uint32_t n = rest_cal.count;
    rest_rms = median_u32(rest_cal.rms_buf, n);
    rest_mav = median_u32(rest_cal.mav_buf, n);
    rest_wl  = median_u32(rest_cal.wl_buf,  n);
    rest_zc  = median_u16(rest_cal.zc_buf,  n);
    rest_ssc = median_u16(rest_cal.ssc_buf, n);

    double mean_sq = (rest_cal.var_n > 0) ?
                     ((double)rest_cal.var_sum / (double)rest_cal.var_n) : 0.0;
    double sigma   = sqrt(mean_sq);

    zc_threshold  = clamp_u32((int32_t)(sigma * 3.0),
                              EMG_ZC_THRESHOLD_FLOOR, EMG_ZC_THRESHOLD_CEIL);
    ssc_threshold = clamp_u32((int32_t)(sigma * 4.0),
                              EMG_SSC_THRESHOLD_FLOOR, EMG_SSC_THRESHOLD_CEIL);
    active_threshold = clamp_u32((int32_t)((uint64_t)rest_mav * EMG_ACTIVE_RATIO_PERCENT / 100U),
                                 EMG_ACTIVE_FLOOR, 0x7FFFFFFFU);

    /* Rest calibration establishes noise and activity thresholds. Active
     * baseline learning continues online and does not block normal use. */
    emg_model_reset();
    alert_hold = 0;
    phase = EMG_PHASE_RUNNING;
    status = EMG_STATUS_OK;
}

static void emg_finalize_active_baseline(rt_bool_t enter_running)
{
    uint32_t n = active_cal.count;
    base_rms = median_u32(active_cal.rms_buf, n);
    base_mav = median_u32(active_cal.mav_buf, n);
    base_wl  = median_u32(active_cal.wl_buf,  n);
    base_zc  = median_u16(active_cal.zc_buf,  n);
    base_ssc = median_u16(active_cal.ssc_buf, n);

    if (base_zc == 0)  base_zc = 1;
    if (base_wl == 0)  base_wl = 1;
    if (base_mav == 0) base_mav = 1;
    if (base_rms == 0) base_rms = 1;

    if (enter_running)
    {
        emg_model_reset();
        alert_hold = 0;
        phase  = EMG_PHASE_RUNNING;
        status = EMG_STATUS_OK;
    }
}

static uint16_t emg_score_fatigue(uint32_t rms, uint32_t mav, uint32_t wl, uint16_t zc)
{
    if (base_rms == 0 || base_mav == 0 || base_wl == 0 || base_zc == 0) return 0;

    uint32_t rms_pct = (uint32_t)(((uint64_t)rms * 100U) / base_rms);
    uint32_t mav_pct = (uint32_t)(((uint64_t)mav * 100U) / base_mav);
    uint32_t wl_pct  = (uint32_t)(((uint64_t)wl  * 100U) / base_wl);
    uint32_t zc_pct  = (uint32_t)(((uint64_t)zc  * 100U) / base_zc);

    int32_t rms_score  = ((int32_t)rms_pct - EMG_FATIGUE_RISE_PCT) * EMG_FATIGUE_RMS_WEIGHT  / 50;
    int32_t mav_score  = ((int32_t)mav_pct - EMG_FATIGUE_RISE_PCT) * EMG_FATIGUE_MAV_WEIGHT  / 50;
    int32_t wl_score   = ((int32_t)wl_pct  - EMG_FATIGUE_RISE_PCT) * EMG_FATIGUE_WL_WEIGHT   / 50;
    int32_t freq_score = (100 - (int32_t)zc_pct)                   * EMG_FATIGUE_FREQ_WEIGHT / 50;

    uint32_t sum = (uint32_t)(clamp_i32(rms_score,  0, EMG_FATIGUE_RMS_WEIGHT) +
                              clamp_i32(mav_score,  0, EMG_FATIGUE_MAV_WEIGHT) +
                              clamp_i32(wl_score,   0, EMG_FATIGUE_WL_WEIGHT)  +
                              clamp_i32(freq_score, 0, EMG_FATIGUE_FREQ_WEIGHT));
    if (sum > 100U) sum = 100U;
    return (uint16_t)sum;
}

static uint16_t emg_smooth_fatigue_score(uint16_t instant_score)
{
    if (instant_score > 100U) instant_score = 100U;

    if (fatigue_state_score == 0U)
    {
        fatigue_state_score = instant_score;
    }
    else if (instant_score >= fatigue_state_score)
    {
        fatigue_state_score = (uint16_t)(
            ((uint32_t)fatigue_state_score * 3U + instant_score + 2U) / 4U);
    }
    else
    {
        fatigue_state_score = (uint16_t)(
            ((uint32_t)fatigue_state_score * 19U + instant_score + 10U) / 20U);
    }

    return fatigue_state_score;
}

static void emg_eval_window(void)
{
    if (win.valid < EMG_WINDOW_SAMPLES) return;

    uint32_t rms = emg_isqrt64(win.sum_sq / EMG_WINDOW_SAMPLES);
    uint32_t mav = (uint32_t)(win.sum_abs / EMG_WINDOW_SAMPLES);
    uint32_t wl  = (uint32_t)win.sum_diff;
    uint16_t zc  = (uint16_t)win.sum_zc;
    uint16_t ssc = emg_compute_ssc();

    win.window_count++;

    uint8_t active = 0;
    uint16_t score = 0;
    uint8_t alert = 0;

    switch (phase)
    {
    case EMG_PHASE_REST_CAL:
        if (rest_cal.count < EMG_REST_CAL_WINDOWS)
        {
            uint32_t k = rest_cal.count;
            rest_cal.rms_buf[k] = rms;
            rest_cal.mav_buf[k] = mav;
            rest_cal.wl_buf[k]  = wl;
            rest_cal.zc_buf[k]  = zc;
            rest_cal.ssc_buf[k] = ssc;
            rest_cal.var_sum  += win.sum_sq;
            rest_cal.var_n    += EMG_WINDOW_SAMPLES;
            rest_cal.count++;
            if (rest_cal.count == EMG_REST_CAL_WINDOWS) emg_finalize_rest_baseline();
        }
        status = EMG_STATUS_CAL;
        break;

    case EMG_PHASE_ACTIVE_CAL:
        active = (mav >= active_threshold) ? 1U : 0U;
        if (active && active_cal.count < EMG_ACTIVE_CAL_WINDOWS)
        {
            uint32_t k = active_cal.count;
            active_cal.rms_buf[k] = rms;
            active_cal.mav_buf[k] = mav;
            active_cal.wl_buf[k]  = wl;
            active_cal.zc_buf[k]  = zc;
            active_cal.ssc_buf[k] = ssc;
            active_cal.count++;
            if (active_cal.count == EMG_ACTIVE_CAL_WINDOWS)
                emg_finalize_active_baseline(RT_TRUE);
        }
        status = EMG_STATUS_CAL;
        break;

    case EMG_PHASE_RUNNING:
    default:
        active = (mav >= active_threshold) ? 1U : 0U;
        if (active)
        {
            uint16_t rf_score;
            uint16_t trend_score = 0U;

            model_inactive_windows = 0U;

            if (active_cal.count < EMG_ACTIVE_CAL_WINDOWS)
            {
                uint32_t k = active_cal.count;
                active_cal.rms_buf[k] = rms;
                active_cal.mav_buf[k] = mav;
                active_cal.wl_buf[k]  = wl;
                active_cal.zc_buf[k]  = zc;
                active_cal.ssc_buf[k] = ssc;
                active_cal.count++;
                if (active_cal.count == EMG_ACTIVE_CAL_WINDOWS)
                    emg_finalize_active_baseline(RT_FALSE);
            }

            emg_model_push(rms, mav, wl, zc, ssc);
            rf_score = emg_score_fatigue_rf();
            if (base_rms > 0U && base_mav > 0U && base_wl > 0U && base_zc > 0U)
            {
                trend_score = emg_score_fatigue(rms, mav, wl, zc);
            }

            score = emg_smooth_fatigue_score(
                rf_score >= trend_score ? rf_score : trend_score);
            if (score >= EMG_FATIGUE_SCORE_THRESHOLD)
            {
                if (alert_hold < EMG_FATIGUE_HOLD_WINDOWS) alert_hold++;
            }
            else
            {
                alert_hold = 0;
            }
        }
        else
        {
            alert_hold = 0;
            if (model_inactive_windows < EMG_MODEL_RESET_INACTIVE_WINDOWS)
                model_inactive_windows++;

            if (model_inactive_windows >= EMG_MODEL_RESET_INACTIVE_WINDOWS)
            {
                emg_model_reset();
            }

            score = fatigue_state_score;
        }
        alert = (alert_hold >= EMG_FATIGUE_HOLD_WINDOWS) ? 1U : 0U;
        status = alert ? EMG_STATUS_ALERT : (active ? EMG_STATUS_OK : EMG_STATUS_REST);
        break;
    }

    if (rep_active)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        rep_live.window_count++;
        rep_live.sum_window_rms += rms;
        rep_live.sum_window_mav += mav;
        rep_live.sum_window_wl  += wl;
        if (rms > rep_live.peak_rms) rep_live.peak_rms = rms;
        if (mav > rep_live.peak_mav) rep_live.peak_mav = mav;
        if (wl  > rep_live.peak_wl)  rep_live.peak_wl  = wl;
        if (score > rep_live.peak_fatigue) rep_live.peak_fatigue = score;
        rt_hw_interrupt_enable(level);
    }

    emg_publish_snapshot(rms, mav, wl, zc, ssc, score, alert, active);
}

static void emg_on_sample(rt_int32_t ch1, rt_int32_t emg_raw)
{
    /* The LIVE fatigue pipeline stays on CH2; RAW can be CH1/CH2 for wiring checks. */
    emg_raw_enqueue(win.sample_count,
                    (raw_output_channel == 1U) ? ch1 : emg_raw);

    float y = (float)emg_raw;
    for (int i = 0; i < EMG_BIQUAD_COUNT; i++)
    {
        y = biquad_df2t(&biquad_state[i], &emg_biquad_coefs[i], y);
    }

    int32_t  ac      = (int32_t)y;
    uint32_t abs_ac  = (uint32_t)(ac < 0 ? -ac : ac);

    int32_t  prev_ac = win.prev_ac;
    int32_t  cur_diff = ac - prev_ac;
    uint32_t abs_diff = (uint32_t)(cur_diff < 0 ? -cur_diff : cur_diff);

    uint8_t this_zc = 0;
    if (win.sample_count > 0)
    {
        uint32_t abs_prev = (uint32_t)(prev_ac < 0 ? -prev_ac : prev_ac);
        if (abs_ac > zc_threshold && abs_prev > zc_threshold &&
            ((ac > 0 && prev_ac < 0) || (ac < 0 && prev_ac > 0)))
        {
            this_zc = 1;
        }
    }

    if (win.valid == EMG_WINDOW_SAMPLES)
    {
        int32_t  o_ac  = win.ac_ring[win.pos];
        uint32_t o_abs = win.abs_ring[win.pos];
        uint32_t o_dif = win.diff_ring[win.pos];
        uint8_t  o_zc  = win.zc_ring[win.pos];
        win.sum_sq   -= (uint64_t)o_ac * (uint64_t)o_ac;
        win.sum_abs  -= o_abs;
        win.sum_diff -= o_dif;
        win.sum_zc   -= o_zc;
    }
    else
    {
        win.valid++;
    }

    win.ac_ring[win.pos]   = ac;
    win.abs_ring[win.pos]  = abs_ac;
    win.diff_ring[win.pos] = abs_diff;
    win.zc_ring[win.pos]   = this_zc;

    win.sum_sq   += (uint64_t)ac * (uint64_t)ac;
    win.sum_abs  += abs_ac;
    win.sum_diff += abs_diff;
    win.sum_zc   += this_zc;

    win.pos = (win.pos + 1) % EMG_WINDOW_SAMPLES;
    win.prev_ac = ac;
    win.sample_count++;

    if (rep_active)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        rep_live.iemg += abs_ac;
        rep_live.sample_count++;
        if (abs_ac > rep_live.peak_abs_ac) rep_live.peak_abs_ac = abs_ac;
        rt_hw_interrupt_enable(level);
    }

    win.hop_count++;
    if (win.hop_count >= EMG_HOP_SAMPLES && win.valid == EMG_WINDOW_SAMPLES)
    {
        win.hop_count = 0;
        emg_eval_window();
    }
}

void emg_pipeline_init(void)
{
    rt_memset(biquad_state, 0, sizeof(biquad_state));
    rt_memset(&win, 0, sizeof(win));
    rt_memset(&rest_cal, 0, sizeof(rest_cal));
    rt_memset(&active_cal, 0, sizeof(active_cal));
    rt_memset(&snap_pub, 0, sizeof(snap_pub));
    rt_memset(&rep_live, 0, sizeof(rep_live));

    phase = EMG_PHASE_REST_CAL;
    status = EMG_STATUS_CAL;
    alert_hold = 0;
    rep_active = RT_FALSE;
    emg_model_reset();

    rest_rms = rest_mav = rest_wl = 0;
    rest_zc = rest_ssc = 0;
    base_rms = base_mav = base_wl = 0;
    base_zc = base_ssc = 0;

    zc_threshold  = EMG_ZC_THRESHOLD_FLOOR;
    ssc_threshold = EMG_SSC_THRESHOLD_FLOOR;
    active_threshold = EMG_ACTIVE_FLOOR;

    if (raw_output_enable)
    {
        emg_pipeline_raw_output_set(RT_TRUE);
    }

    ads1292_register_sample_cb(emg_on_sample);

    rt_kprintf("CAL,REST_BEGIN,rest_windows=%u,window_ms=%u\n",
               (unsigned)EMG_REST_CAL_WINDOWS,
               (unsigned)((EMG_WINDOW_SAMPLES * 1000U) / EMG_SAMPLE_RATE));
}

void emg_pipeline_get_snapshot(emg_snapshot_t *out)
{
    if (out == RT_NULL) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = snap_pub;
    rt_hw_interrupt_enable(level);
}

void emg_pipeline_rep_start(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_memset(&rep_live, 0, sizeof(rep_live));
    rep_active = RT_TRUE;
    rt_hw_interrupt_enable(level);
}

void emg_pipeline_rep_stop(emg_rep_features_t *out)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (out != RT_NULL) *out = rep_live;
    rep_active = RT_FALSE;
    rt_hw_interrupt_enable(level);
}

void emg_pipeline_rep_cancel(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rep_active = RT_FALSE;
    rt_memset(&rep_live, 0, sizeof(rep_live));
    rt_hw_interrupt_enable(level);
}

void emg_pipeline_cal_reset(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_memset(&rest_cal, 0, sizeof(rest_cal));
    rt_memset(&active_cal, 0, sizeof(active_cal));
    rest_rms = rest_mav = rest_wl = 0;
    rest_zc = rest_ssc = 0;
    base_rms = base_mav = base_wl = 0;
    base_zc = base_ssc = 0;
    zc_threshold  = EMG_ZC_THRESHOLD_FLOOR;
    ssc_threshold = EMG_SSC_THRESHOLD_FLOOR;
    active_threshold = EMG_ACTIVE_FLOOR;
    phase = EMG_PHASE_REST_CAL;
    status = EMG_STATUS_CAL;
    alert_hold = 0;
    emg_model_reset();
    rt_hw_interrupt_enable(level);

    rt_kprintf("CAL,REST_BEGIN,rest_windows=%u\n", (unsigned)EMG_REST_CAL_WINDOWS);
}

void emg_pipeline_active_cal_force(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_memset(&active_cal, 0, sizeof(active_cal));
    base_rms = base_mav = base_wl = 0;
    base_zc = base_ssc = 0;
    if (rest_cal.count >= EMG_REST_CAL_WINDOWS)
    {
        phase = EMG_PHASE_RUNNING;
        status = EMG_STATUS_OK;
    }
    else
    {
        phase = EMG_PHASE_REST_CAL;
        status = EMG_STATUS_CAL;
    }
    alert_hold = 0;
    emg_model_reset();
    rt_hw_interrupt_enable(level);
}

void emg_pipeline_raw_output_set(rt_bool_t enable)
{
    if (enable)
    {
        emg_raw_start_thread();
        if (raw_thread == RT_NULL)
        {
            rt_kprintf("RAW_ERR,not_started\n");
            return;
        }
    }

    rt_base_t level = rt_hw_interrupt_disable();
    raw_head = 0;
    raw_tail = 0;
    if (enable) raw_dropped = 0;
    raw_output_enable = enable ? RT_TRUE : RT_FALSE;
    rt_hw_interrupt_enable(level);

    if (enable)
    {
        rt_kprintf("RAW_BEGIN,sample_rate=%u,channel=CH%u,format=RAW,t_ms,emg\n",
                   (unsigned int)EMG_SAMPLE_RATE,
                   (unsigned int)raw_output_channel);
    }
    else
    {
        rt_kprintf("RAW_STOP,dropped=%u\n", (unsigned int)raw_dropped);
    }
}

rt_bool_t emg_pipeline_raw_output_get(void)
{
    return raw_output_enable ? RT_TRUE : RT_FALSE;
}

uint32_t emg_pipeline_raw_drop_count(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t dropped = raw_dropped;
    rt_hw_interrupt_enable(level);
    return dropped;
}

void emg_pipeline_raw_channel_set(uint8_t channel)
{
    if (channel != 1U && channel != 2U) channel = 2U;

    rt_base_t level = rt_hw_interrupt_disable();
    raw_output_channel = channel;
    raw_head = 0;
    raw_tail = 0;
    raw_dropped = 0;
    rt_hw_interrupt_enable(level);

    rt_kprintf("RAW_CHANNEL,CH%u\n", (unsigned int)channel);
}

uint8_t emg_pipeline_raw_channel_get(void)
{
    return raw_output_channel;
}

#ifdef FINSH_USING_MSH
#include <finsh.h>

static void emg_cal_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_pipeline_cal_reset();
    rt_kprintf("EMG calibration reset. Keep the muscle relaxed.\n");
}
MSH_CMD_EXPORT(emg_cal_reset, reset EMG calibration);

static void emg_active_cal_force(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_pipeline_active_cal_force();
    rt_kprintf("Active baseline reset. Do %u contractions.\n",
               (unsigned)EMG_ACTIVE_CAL_WINDOWS);
}
MSH_CMD_EXPORT(emg_active_cal_force, restart EMG active baseline);

static void emg_status_dump(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_snapshot_t s;
    emg_pipeline_get_snapshot(&s);
    rt_kprintf("phase=%d status=%d active=%d alert=%d score=%u\n",
               s.phase, s.status, s.active, s.fatigue_alert, s.fatigue_score);
    rt_kprintf("rms=%u mav=%u wl=%u zc=%u ssc=%u\n",
               (unsigned)s.rms, (unsigned)s.mav, (unsigned)s.wl,
               (unsigned)s.zc, (unsigned)s.ssc);
    rt_kprintf("rest_mav=%u  base_mav=%u  zc_th=%u  active_th=%u\n",
               (unsigned)s.rest_mav, (unsigned)s.base_mav,
               (unsigned)s.zc_threshold, (unsigned)s.active_threshold);
}
MSH_CMD_EXPORT(emg_status_dump, dump current EMG pipeline status);

static void raw_start(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_pipeline_raw_output_set(RT_TRUE);
}
MSH_CMD_EXPORT(raw_start, enable 500 Hz RAW t_ms emg output);

static void raw_fast_start(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_raw_switch_baud_delayed(EMG_RAW_FAST_BAUD);
    emg_pipeline_raw_output_set(RT_TRUE);
}
MSH_CMD_EXPORT(raw_fast_start, switch UART to 921600 then start RAW output);

static void raw_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_pipeline_raw_output_set(RT_FALSE);
}
MSH_CMD_EXPORT(raw_stop, disable RAW t_ms emg output);

static void raw_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    rt_kprintf("RAW_STATUS,enable=%u,channel=CH%u,dropped=%u\n",
               emg_pipeline_raw_output_get() ? 1U : 0U,
               (unsigned int)emg_pipeline_raw_channel_get(),
               (unsigned int)emg_pipeline_raw_drop_count());
}
MSH_CMD_EXPORT(raw_status, show RAW output status);

static void raw_baud_115200(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_raw_switch_baud_delayed(EMG_RAW_SLOW_BAUD);
}
MSH_CMD_EXPORT(raw_baud_115200, switch console UART to 115200);

static void raw_baud_921600(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_raw_switch_baud_delayed(EMG_RAW_FAST_BAUD);
}
MSH_CMD_EXPORT(raw_baud_921600, switch console UART to 921600);

static void raw_ch1(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_pipeline_raw_channel_set(1U);
}
MSH_CMD_EXPORT(raw_ch1, select CH1 as RAW output source);

static void raw_ch2(int argc, char **argv)
{
    (void)argc; (void)argv;
    emg_pipeline_raw_channel_set(2U);
}
MSH_CMD_EXPORT(raw_ch2, select CH2 as RAW output source);
#endif
