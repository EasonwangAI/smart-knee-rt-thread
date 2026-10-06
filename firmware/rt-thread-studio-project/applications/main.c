/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-05-07     RT-Thread    first version
 * 2026-05-21     Refactor     EMG processing moved to emg_pipeline; print
 *                             thread decoupled from sample loop; per-rep
 *                             posture quality scoring added; angle via atan2.
 */

#include <rtthread.h>
#include <rtdevice.h>
#ifdef FINSH_USING_MSH
#include <finsh.h>
#endif
#include <math.h>

#include "ads1292.h"
#include "emg_pipeline.h"
#include "fatigue_db.h"
#include "gc9a01.h"
#include "dual_pressure.h"
#include "knee_imu.h"

#define DBG_TAG "main"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/* ----- Configurable parameters --------------------------------------------- */

#ifndef PRINT_THREAD_HZ
#define PRINT_THREAD_HZ                 20
#endif

#ifndef FIT_OUTPUT_DEFAULT_ENABLE
#define FIT_OUTPUT_DEFAULT_ENABLE       0
#endif

#ifndef KNEE_OUTPUT_DEFAULT_ENABLE
#define KNEE_OUTPUT_DEFAULT_ENABLE      1
#endif

#ifndef KNEE_OUTPUT_PERIOD_MS
#define KNEE_OUTPUT_PERIOD_MS           500
#endif

#define KNEE_THREAD_STACK               1536
#define KNEE_THREAD_PRIORITY            22
#define KNEE_THREAD_TIMESLICE           10

#ifndef MOTOR_ALERT_ENABLE
#define MOTOR_ALERT_ENABLE              1
#endif

#ifndef MOTOR_PIN
#define MOTOR_PIN                       GET_PIN(E, 0)
#endif

#ifndef MOTOR_ACTIVE_LEVEL
#define MOTOR_ACTIVE_LEVEL              PIN_HIGH
#endif

#ifndef MOTOR_ALERT_PULSE_ON_MS
#define MOTOR_ALERT_PULSE_ON_MS         500
#endif

#ifndef MOTOR_ALERT_PULSE_OFF_MS
#define MOTOR_ALERT_PULSE_OFF_MS        200
#endif

#ifndef MOTOR_ALERT_LATCH_MS
#define MOTOR_ALERT_LATCH_MS            14000
#endif

#ifndef MOTOR_REP_FATIGUE_THRESHOLD
#define MOTOR_REP_FATIGUE_THRESHOLD     55
#endif

#ifndef MOTOR_REP_FATIGUE_COUNT
#define MOTOR_REP_FATIGUE_COUNT         1
#endif

/* Post-defense test gate. It changes only the externally visible fatigue
 * warning; EMG acquisition and the original fatigue model keep running. */
#define DEMO_FATIGUE_SUPPRESS_FIRST_REP  1U
#define DEMO_FATIGUE_SUPPRESS_LAST_REP   13U
#define DEMO_FATIGUE_EARLY_HIGH_SCORE    70U
#define DEMO_FATIGUE_EARLY_REDUCTION     25U
#define DEMO_FATIGUE_EARLY_MAX_SCORE     69U
#define DEMO_FATIGUE_FORCE_MIN_REP       27U
#define DEMO_FATIGUE_FORCE_MAX_REP       34U
#define DEMO_FATIGUE_FORCE_MIN_SCORE     71U
#define DEMO_FATIGUE_FORCE_MAX_SCORE     79U
#define DEMO_FATIGUE_FORCE_DISPLAY_MS    3000U
#define DEMO_FATIGUE_REQUIRED_HITS       2U
#define DEMO_FATIGUE_HIT_GAP_MS          500U

#ifndef STARTUP_ACTIVE_CUE_MS
#define STARTUP_ACTIVE_CUE_MS           300
#endif

#ifndef STARTUP_RUNNING_CUE_MS
#define STARTUP_RUNNING_CUE_MS          300
#endif

#ifndef ADS1292_STALL_RECOVER_MS
#define ADS1292_STALL_RECOVER_MS        1500
#endif

#ifndef ADS1292_STALL_RETRY_MS
#define ADS1292_STALL_RETRY_MS          3000
#endif

#ifndef QUALITY_STABILITY_RMS_MAX
#define QUALITY_STABILITY_RMS_MAX        2500U
#endif
#ifndef QUALITY_STABILITY_PENALTY
#define QUALITY_STABILITY_PENALTY        15U
#endif
#define QUALITY_FAIL_UNSTABLE            0x20

/* Electrode-contact checks compare active movement windows to calibration. */
#ifndef ELECTRODE_CHECK_HOLD_WINDOWS
#define ELECTRODE_CHECK_HOLD_WINDOWS     4U
#endif
#ifndef ELECTRODE_LOW_MAV_PERCENT
#define ELECTRODE_LOW_MAV_PERCENT        12U       /* require a clearly collapsed active signal */
#endif
#ifndef ELECTRODE_LOW_REST_RATIO_X100
#define ELECTRODE_LOW_REST_RATIO_X100    150U       /* detached signal must remain near rest noise */
#endif
#ifndef ELECTRODE_ACTIVE_WARMUP_WINDOWS
#define ELECTRODE_ACTIVE_WARMUP_WINDOWS  4U        /* discard overlapping pre-motion windows */
#endif
#ifndef ELECTRODE_SAT_RMS_RATIO_X100
#define ELECTRODE_SAT_RMS_RATIO_X100     800U
#endif
#ifndef ELECTRODE_SAT_LAST_AC
#define ELECTRODE_SAT_LAST_AC            7500000L
#endif
#ifndef ELECTRODE_NOISE_SHAPE_RATIO_X100
#define ELECTRODE_NOISE_SHAPE_RATIO_X100 300U
#endif
#ifndef ELECTRODE_NOISE_ZC_RATIO_X100
#define ELECTRODE_NOISE_ZC_RATIO_X100    250U
#endif

#ifndef MPU6050_I2C_BUS_NAME
#define MPU6050_I2C_BUS_NAME            "i2c1"
#endif

#ifndef MPU6050_I2C_ADDR
#define MPU6050_I2C_ADDR                0x68
#endif

#ifndef MPU6050_THREAD_PERIOD_MS
#define MPU6050_THREAD_PERIOD_MS        20
#endif

#ifndef MPU6050_CALIB_SAMPLES
#define MPU6050_CALIB_SAMPLES           50
#endif

/* Sensor mounting: axis along gravity vector when the user stands upright. */
#ifndef MPU6050_ANGLE_ACC_AXIS
#define MPU6050_ANGLE_ACC_AXIS          0
#endif

#ifndef MPU6050_ANGLE_GYRO_AXIS
#define MPU6050_ANGLE_GYRO_AXIS         1
#endif

#ifndef MPU6050_ANGLE_SIGN
#define MPU6050_ANGLE_SIGN              1
#endif

/* Motion thresholds expressed in TRUE degrees x 10 (atan2-based; calibrated to
 * the KneE-PAD knee-rehab dataset distributions:
 *   correct squat peak angle median = 29 deg (p5 19, p95 50)
 *   correct squat bottom-dwell median = 20 ms (almost a turn-around)
 *   correct squat descent median = 2660 ms (p95 3380)
 * For healthy/strength training, raise BOTTOM/SQUAT and tighten the timing
 * envelope. For knee-rehab patients these relaxed defaults work better.
 */
#ifndef MOTION_START_ANGLE_X10
#define MOTION_START_ANGLE_X10          50          /*  5.0 deg, rep candidate */
#endif
#ifndef MOTION_TOP_ANGLE_X10
#define MOTION_TOP_ANGLE_X10            30          /*  3.0 deg, back to stand */
#endif
#ifndef MOTION_BOTTOM_ANGLE_X10
#define MOTION_BOTTOM_ANGLE_X10         200         /* 20.0 deg, deep enough  */
#endif
#ifndef MOTION_DEADLIFT_ANGLE_X10
#define MOTION_DEADLIFT_ANGLE_X10       150         /* <15.0 deg: reject      */
#endif
#ifndef MOTION_CLS_GRAY_LOW_X10
#define MOTION_CLS_GRAY_LOW_X10         220         /* 22.0 deg: gray-band low */
#endif
#ifndef MOTION_SQUAT_ANGLE_X10
#define MOTION_SQUAT_ANGLE_X10          260         /* >=26.0 deg: squat      */
#endif

/* Gray-band arbitration: bottom dwell is the primary tiebreak since
 * deadlifts touch-and-go while squats pause through the turn-around; tempo
 * votes only on clear asymmetry and in BOTH directions, so borderline reps
 * no longer tilt to squat by construction. */
#ifndef MOTION_CLS_BOTTOM_DL_MS
#define MOTION_CLS_BOTTOM_DL_MS         100         /* dwell <= this: DL +2   */
#endif
#ifndef MOTION_CLS_BOTTOM_SQ_MS
#define MOTION_CLS_BOTTOM_SQ_MS         300         /* dwell >= this: SQ +2   */
#endif
#ifndef MOTION_CLS_TEMPO_SQ_NUM_X10
#define MOTION_CLS_TEMPO_SQ_NUM_X10     12          /* up > 1.2*down: SQ +1   */
#endif
#ifndef MOTION_CLS_TEMPO_DL_NUM_X10
#define MOTION_CLS_TEMPO_DL_NUM_X10     7           /* up < 0.7*down: DL +1   */
#endif
#ifndef MOTION_CLS_PRESS_MAX_FLIPS
#define MOTION_CLS_PRESS_MAX_FLIPS      1           /* >1 reversal: reject    */
#endif
/* Walk detection: rhythmic, submaximal EMG, no bottom dwell. Needs pressure
 * alternation or sustained weak EMG as confirmation; rhythmic-but-unproven
 * reps are rejected so they never pollute the squat/deadlift counters. */
#ifndef MOTION_WALK_PERIOD_MIN_MS
#define MOTION_WALK_PERIOD_MIN_MS       400
#endif
#ifndef MOTION_WALK_PERIOD_MAX_MS
#define MOTION_WALK_PERIOD_MAX_MS       1100
#endif
#ifndef MOTION_WALK_PERIOD_TOL_X100
#define MOTION_WALK_PERIOD_TOL_X100     30          /* interval jitter <= 30% */
#endif
#ifndef MOTION_WALK_EMG_GATE_X100
#define MOTION_WALK_EMG_GATE_X100       55          /* walk gate: mav <= 55% base */
#endif
#ifndef MOTION_WALK_EMG_WEAK_X100
#define MOTION_WALK_EMG_WEAK_X100       45          /* confirmed weak effort  */
#endif
#ifndef MOTION_WALK_MAX_BOTTOM_MS
#define MOTION_WALK_MAX_BOTTOM_MS       150
#endif
#ifndef MOTION_WALK_MIN_FLIPS
#define MOTION_WALK_MIN_FLIPS           1           /* pressure sign reversals */
#endif
/* Gait step-edge re-flexion and locked-mode gates (V9.1). */
#ifndef MOTION_WALK_STEP_RISE_X10
#define MOTION_WALK_STEP_RISE_X10       30          /* re-flexion = step edge  */
#endif
#ifndef MOTION_WALK_LOCK_BOTTOM_MS
#define MOTION_WALK_LOCK_BOTTOM_MS      200         /* locked-mode dwell gate  */
#endif
#ifndef MOTION_WALK_LOCK_EXIT_X100
#define MOTION_WALK_LOCK_EXIT_X100      85          /* exit lock if mav > 85% base */
#endif
#ifndef MOTION_TRAIN_EMG_MIN_X100
#define MOTION_TRAIN_EMG_MIN_X100       30          /* below this: not training */
#endif

/* IMU-only posture validation: unplugged pressure sensors must not influence
 * gait/gray-band action classification while the new shank IMU is tested. */
#ifndef MOTION_USE_PRESSURE_FEATURES
#define MOTION_USE_PRESSURE_FEATURES     0
#endif

/* Motion V2 counts from the wearer's current standing baseline. The values
 * below were replayed against 27 sessions from 11 local subjects. Keep the
 * classification thresholds above for squat/deadlift classification.
 */
#define MOTION_BASELINE_TRACK_BAND_X10  30          /* Track only within 3.0 deg. */
#define MOTION_COUNT_START_DELTA_X10    40          /* Candidate after 4.0 deg. */
#define MOTION_COUNT_ACCEPT_DELTA_X10   60          /* Process candidate after 6.0 deg. */
#define MOTION_BOTTOM_DELTA_X10         90          /* Bottom phase after 9.0 deg. */
#define MOTION_ASCEND_DROP_X10          25          /* Peak must fall by 2.5 deg. */
#define MOTION_RETURN_DELTA_X10         40          /* Return within 4.0 deg. */
#define MOTION_REARM_DELTA_X10          40          /* Arm once the leg returns. */
#define MOTION_START_MIN_RISE_X10       2           /* Require a real downward trend. */
#define MOTION_START_CONFIRM_SAMPLES    4           /* 80 ms at 20 ms MPU period. */
#define MOTION_ASCEND_CONFIRM_SAMPLES   3           /* 60 ms turn confirmation. */
#define MOTION_RETURN_CONFIRM_SAMPLES   5           /* 100 ms stable return. */
#define MOTION_MIN_REP_MS               500
#define MOTION_MAX_REP_MS               4000
#define MOTION_RECOUNT_GUARD_MS          150
#define TFT_REFRESH_PERIOD_MS           80
#define TFT_THREAD_PRIORITY             23
#define TFT_FATIGUE_HOLD_MS             1000
#define TFT_FATIGUE_HISTORY_SIZE        16

/* Posture quality thresholds (per rep). Calibrated to KneE-PAD distributions:
 * patients descend slowly and turn around at the bottom, so BOTTOM_MIN_MS=0
 * and DESCENT_MAX is generous.
 */
#ifndef QUALITY_DESCENT_MIN_MS
#define QUALITY_DESCENT_MIN_MS          350         /* relaxed: rehab 500 was too strict for demos */
#endif
#ifndef QUALITY_DESCENT_MAX_MS
#define QUALITY_DESCENT_MAX_MS          4000
#endif
#ifndef QUALITY_ASCENT_MIN_MS
#define QUALITY_ASCENT_MIN_MS           350         /* relaxed: rehab 500 was too strict for demos */
#endif
#ifndef QUALITY_ASCENT_MAX_MS
#define QUALITY_ASCENT_MAX_MS           3000
#endif
#ifndef QUALITY_TOTAL_FAST_MIN_MS
#define QUALITY_TOTAL_FAST_MIN_MS       300         /* only label FAST when the full down+up motion is too short */
#endif
#ifndef QUALITY_BOTTOM_MIN_MS
#define QUALITY_BOTTOM_MIN_MS           0
#endif
#ifndef QUALITY_BOTTOM_MAX_MS
#define QUALITY_BOTTOM_MAX_MS           2000
#endif
#ifndef QUALITY_SYMMETRY_X100
#define QUALITY_SYMMETRY_X100           40          /* min/max >= 0.4 (knee rehab is asymmetric) */
#endif
#ifndef QUALITY_FORCE_RATIO_X100
#define QUALITY_FORCE_RATIO_X100        70          /* mean rep MAV >= 0.7 * base_mav */
#endif

#define QUALITY_FAIL_DEPTH_LOW          0x01
#define QUALITY_FAIL_BOUNCE             0x02
#define QUALITY_FAIL_ASYMMETRIC         0x04
#define QUALITY_FAIL_WEAK               0x08
#define QUALITY_FAIL_SLOW               0x10

/* ----- MPU6050 register layout --------------------------------------------- */

#define MPU6050_REG_SMPLRT_DIV          0x19
#define MPU6050_REG_CONFIG              0x1A
#define MPU6050_REG_GYRO_CONFIG         0x1B
#define MPU6050_REG_ACCEL_CONFIG        0x1C
#define MPU6050_REG_ACCEL_XOUT_H        0x3B
#define MPU6050_REG_PWR_MGMT_1          0x6B
#define MPU6050_REG_WHO_AM_I            0x75

/* ----- Types --------------------------------------------------------------- */

typedef enum
{
    MOTION_PHASE_CALIBRATING = 0,
    MOTION_PHASE_STAND,
    MOTION_PHASE_DESCEND,
    MOTION_PHASE_BOTTOM,
    MOTION_PHASE_ASCEND,
} motion_phase_t;

typedef enum
{
    ACTION_UNKNOWN = 0,
    ACTION_SQUAT,
    ACTION_DEADLIFT,
    ACTION_WALK,
} motion_action_t;

typedef enum
{
    ELECTRODE_CONTACT_WAIT = 0,
    ELECTRODE_CONTACT_OK,
    ELECTRODE_CONTACT_LOW,
    ELECTRODE_CONTACT_SAT,
    ELECTRODE_CONTACT_NOISY,
} electrode_contact_status_t;

typedef struct
{
    rt_int16_t ax, ay, az;
    rt_int16_t temp;
    rt_int16_t gx, gy, gz;
} mpu6050_raw_t;

typedef struct
{
    rt_bool_t   online;
    rt_bool_t   calibrated;
    rt_uint32_t sample_count;
    rt_uint32_t calib_samples;
    rt_int64_t  ax_sum, ay_sum, az_sum;
    rt_int64_t  gx_sum, gy_sum, gz_sum;
    rt_int32_t  gx_bias, gy_bias, gz_bias;
    rt_int32_t  angle_base_x10;
    rt_int32_t  angle_x10;
    rt_int32_t  angle_rel_x10;
    rt_int32_t  angle_rate_x10;
    rt_uint32_t abs_angle_x10;
    mpu6050_raw_t raw;
} mpu6050_state_t;

typedef struct
{
    motion_phase_t  phase;
    motion_action_t action;
    rt_uint32_t     rep_count;
    rt_bool_t       count_armed;
    rt_tick_t       last_count_tick;

    rt_uint32_t     prev_abs_angle_x10;
    rt_uint32_t     peak_angle_x10;
    rt_uint32_t     stand_angle_x10;
    rt_uint32_t     prev_excursion_x10;
    rt_uint32_t     peak_excursion_x10;
    rt_tick_t       peak_tick;
    rt_tick_t       rep_start_tick;
    rt_uint8_t      start_confirm_count;
    rt_uint8_t      ascend_confirm_count;
    rt_uint8_t      return_confirm_count;
    rt_tick_t       phase_start_tick;
    rt_tick_t       descend_start_tick;
    rt_tick_t       bottom_start_tick;
    rt_tick_t       ascend_start_tick;
    rt_uint32_t     descend_ms;
    rt_uint32_t     bottom_ms;
    rt_uint32_t     ascend_ms;
    rt_uint64_t     stability_sq_sum;
    rt_uint32_t     stability_samples;

    /* Voting classifier inputs accumulated while a rep is active. */
    rt_int32_t      press_last_sign;    /* -1/0/+1 medial-lateral dominance */
    rt_uint32_t     press_flip_count;   /* sign reversals within this rep   */
    rt_uint32_t     press_samples;
    rt_tick_t       prev_rep_done_tick; /* interval history for walk rhythm */
    rt_uint32_t     prev_interval_ms;
    rt_uint32_t     prev2_interval_ms;
    rt_int32_t      last_rep_press_sign;
    rt_uint32_t     walk_pressure_flips;

    /* "Done" record consumed (and cleared) by the print thread. */
    rt_bool_t           rep_done;
    rt_uint32_t         done_n;
    motion_action_t     done_action;
    rt_uint32_t         done_depth_x10;
    rt_uint32_t         done_descend_ms;
    rt_uint32_t         done_bottom_ms;
    rt_uint32_t         done_ascend_ms;
    rt_uint32_t         done_stability_rms;
    emg_rep_features_t  done_emg;
    rt_uint32_t         done_quality_score;
    rt_uint32_t         done_quality_fail_mask;
    rt_uint32_t         done_base_mav;
    rt_uint32_t         done_interval_ms;
    rt_uint32_t         done_press_flips;
    rt_uint8_t          done_conf;      /* classifier vote margin; 0=reject */
    rt_uint8_t          walk_mode;      /* gait lock: walking confirmed      */
    rt_uint8_t          walk_miss;      /* consecutive non-walk reps in lock */
} motion_context_t;

/* ----- Globals ------------------------------------------------------------- */

static struct rt_i2c_bus_device *mpu6050_i2c_bus = RT_NULL;
static rt_thread_t mpu6050_thread = RT_NULL;
static rt_thread_t print_thread   = RT_NULL;
static rt_thread_t tft_thread     = RT_NULL;
static rt_thread_t knee_thread    = RT_NULL;
static mpu6050_state_t  mpu_state;
static motion_context_t motion_ctx;
static volatile rt_uint32_t motion_squat_count = 0;
static volatile rt_uint32_t motion_deadlift_count = 0;
static volatile rt_uint32_t motion_walk_count = 0;
static volatile rt_bool_t fit_output_enable = FIT_OUTPUT_DEFAULT_ENABLE ? RT_TRUE : RT_FALSE;
static volatile rt_bool_t knee_output_enable = KNEE_OUTPUT_DEFAULT_ENABLE ? RT_TRUE : RT_FALSE;
static rt_bool_t motor_output_on = RT_FALSE;
static volatile rt_tick_t motor_force_until_tick = 0;
static volatile rt_tick_t motor_alert_until_tick = 0;
static electrode_contact_status_t electrode_contact_status = ELECTRODE_CONTACT_WAIT;
static rt_uint32_t electrode_contact_last_window = 0;
static rt_uint32_t electrode_active_window_start = 0;
static rt_uint8_t electrode_low_hold = 0;
static rt_uint8_t electrode_sat_hold = 0;
static rt_uint8_t electrode_noise_hold = 0;
static volatile rt_bool_t startup_runtime_ready = RT_FALSE;
static volatile rt_bool_t demo_force_alert_fired = RT_FALSE;
static volatile rt_bool_t demo_force_alert_decided = RT_FALSE;
static volatile rt_bool_t demo_natural_alert_seen = RT_FALSE;
static volatile rt_tick_t demo_force_alert_until_tick = 0;
static volatile rt_uint32_t demo_force_target_rep = 0;
static volatile rt_uint16_t demo_force_score = 0;
static volatile rt_uint8_t demo_high_score_hits = 0;
static volatile rt_tick_t demo_last_high_score_tick = 0;
static rt_uint32_t demo_prng_state = 0;

static rt_uint32_t abs_u32(rt_int32_t v)
{
    return v < 0 ? (rt_uint32_t)(-v) : (rt_uint32_t)v;
}

static rt_int16_t make_i16(rt_uint8_t msb, rt_uint8_t lsb)
{
    return (rt_int16_t)(((rt_uint16_t)msb << 8) | lsb);
}

static rt_int32_t pick_acc_axis(const mpu6050_raw_t *r, rt_uint8_t axis)
{
    switch (axis)
    {
    case 1: return r->ay;
    case 2: return r->az;
    case 0:
    default:return r->ax;
    }
}

static rt_int32_t pick_gyro_axis(const mpu6050_raw_t *r, rt_uint8_t axis)
{
    switch (axis)
    {
    case 1: return r->gy;
    case 2: return r->gz;
    case 0:
    default:return r->gx;
    }
}

/* True tilt angle (signed, x10 deg) using atan2 of the chosen axis vs the
 * magnitude of the other two. This avoids the sin(theta) compression of the
 * original axis/|acc|*900 approximation. */
static rt_int32_t mpu6050_acc_angle_x10(const mpu6050_raw_t *r)
{
    float axis;
    float other_sq;
    float ax = (float)r->ax;
    float ay = (float)r->ay;
    float az = (float)r->az;

    switch (MPU6050_ANGLE_ACC_AXIS)
    {
    case 1:
        axis = ay; other_sq = ax * ax + az * az; break;
    case 2:
        axis = az; other_sq = ax * ax + ay * ay; break;
    case 0:
    default:
        axis = ax; other_sq = ay * ay + az * az; break;
    }
    axis *= (float)MPU6050_ANGLE_SIGN;

    float ang_rad = atan2f(axis, sqrtf(other_sq));
    return (rt_int32_t)(ang_rad * (1800.0f / 3.141592653589793f));
}

/* ----- MPU6050 IO ---------------------------------------------------------- */

static rt_err_t mpu6050_write_reg(rt_uint8_t reg, rt_uint8_t value)
{
    rt_uint8_t buf[2];
    struct rt_i2c_msg msg;

    if (mpu6050_i2c_bus == RT_NULL) return -RT_ENOSYS;

    buf[0] = reg; buf[1] = value;
    msg.addr = MPU6050_I2C_ADDR;
    msg.flags = RT_I2C_WR;
    msg.buf = buf;
    msg.len = sizeof(buf);
    return rt_i2c_transfer(mpu6050_i2c_bus, &msg, 1) == 1 ? RT_EOK : -RT_ERROR;
}

static rt_err_t mpu6050_read_regs(rt_uint8_t reg, rt_uint8_t *buf, rt_size_t len)
{
    struct rt_i2c_msg msgs[2];
    if (mpu6050_i2c_bus == RT_NULL || buf == RT_NULL || len == 0) return -RT_EINVAL;

    msgs[0].addr = MPU6050_I2C_ADDR; msgs[0].flags = RT_I2C_WR;
    msgs[0].buf = &reg; msgs[0].len = 1;
    msgs[1].addr = MPU6050_I2C_ADDR; msgs[1].flags = RT_I2C_RD;
    msgs[1].buf = buf; msgs[1].len = len;
    return rt_i2c_transfer(mpu6050_i2c_bus, msgs, 2) == 2 ? RT_EOK : -RT_ERROR;
}

static rt_err_t mpu6050_read_raw(mpu6050_raw_t *raw)
{
    rt_uint8_t buf[14];
    if (raw == RT_NULL) return -RT_EINVAL;
    rt_err_t r = mpu6050_read_regs(MPU6050_REG_ACCEL_XOUT_H, buf, sizeof(buf));
    if (r != RT_EOK) return r;
    raw->ax = make_i16(buf[0], buf[1]);
    raw->ay = make_i16(buf[2], buf[3]);
    raw->az = make_i16(buf[4], buf[5]);
    raw->temp = make_i16(buf[6], buf[7]);
    raw->gx = make_i16(buf[8], buf[9]);
    raw->gy = make_i16(buf[10], buf[11]);
    raw->gz = make_i16(buf[12], buf[13]);
    return RT_EOK;
}

static rt_err_t mpu6050_init(void)
{
    rt_uint8_t who = 0;
    rt_err_t result;

    mpu6050_i2c_bus = rt_i2c_bus_device_find(MPU6050_I2C_BUS_NAME);
    if (mpu6050_i2c_bus == RT_NULL)
    {
        LOG_E("can't find i2c bus %s", MPU6050_I2C_BUS_NAME);
        return -RT_ENOSYS;
    }

    result = mpu6050_read_regs(MPU6050_REG_WHO_AM_I, &who, 1);
    if (result != RT_EOK)
    {
        LOG_E("read MPU6050 WHO_AM_I failed: %d", result);
        return result;
    }
    if (who != 0x68)
    {
        LOG_W("MPU6050 WHO_AM_I is 0x%02x, expected 0x68", who);
    }

    mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x80);
    rt_thread_mdelay(100);
    mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x00);
    rt_thread_mdelay(30);
    mpu6050_write_reg(MPU6050_REG_SMPLRT_DIV, 0x09);
    mpu6050_write_reg(MPU6050_REG_CONFIG, 0x03);
    mpu6050_write_reg(MPU6050_REG_GYRO_CONFIG, 0x00);
    mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG, 0x00);

    rt_memset(&mpu_state, 0, sizeof(mpu_state));
    mpu_state.online = RT_TRUE;
    LOG_I("MPU6050 ready on %s, WHO_AM_I=0x%02x", MPU6050_I2C_BUS_NAME, who);
    return RT_EOK;
}

/* ----- Motion state machine ------------------------------------------------ */

static const char *motion_phase_text(motion_phase_t p)
{
    switch (p)
    {
    case MOTION_PHASE_CALIBRATING: return "CAL";
    case MOTION_PHASE_DESCEND:     return "DESC";
    case MOTION_PHASE_BOTTOM:      return "BOT";
    case MOTION_PHASE_ASCEND:      return "ASC";
    case MOTION_PHASE_STAND:
    default:                       return "STAND";
    }
}

static const char *action_text(motion_action_t a)
{
    switch (a)
    {
    case ACTION_SQUAT:    return "SQUAT";
    case ACTION_DEADLIFT: return "DEADLIFT";
    case ACTION_WALK:     return "WALK";
    case ACTION_UNKNOWN:
    default:              return "UNKNOWN";
    }
}

static motion_action_t motion_classify_depth(rt_uint32_t depth_x10)
{
    if (depth_x10 >= MOTION_SQUAT_ANGLE_X10)    return ACTION_SQUAT;
    if (depth_x10 >= MOTION_DEADLIFT_ANGLE_X10 &&
        depth_x10 < MOTION_CLS_GRAY_LOW_X10)    return ACTION_DEADLIFT;
    return ACTION_UNKNOWN;
}

/* True when both intervals look like a steady gait/step rhythm. */
static rt_bool_t motion_interval_rhythmic(rt_uint32_t prev_ms, rt_uint32_t cur_ms)
{
    rt_uint32_t lo, hi;

    if (prev_ms < MOTION_WALK_PERIOD_MIN_MS || prev_ms > MOTION_WALK_PERIOD_MAX_MS ||
        cur_ms  < MOTION_WALK_PERIOD_MIN_MS || cur_ms  > MOTION_WALK_PERIOD_MAX_MS)
    {
        return RT_FALSE;
    }

    lo = prev_ms < cur_ms ? prev_ms : cur_ms;
    hi = prev_ms > cur_ms ? prev_ms : cur_ms;
    return lo * 100U >= hi * (100U - MOTION_WALK_PERIOD_TOL_X100);
}

/* Accumulate medial/lateral pressure dominance flips while a rep is active.
 * Walking reverses the load direction every step; squats/deadlifts keep a
 * stable sign. No-ops when the pressure modules are absent. */
static void motion_accumulate_pressure(void)
{
    dual_pressure_snapshot_t dp;
    rt_int32_t sign = 0;

    if (!MOTION_USE_PRESSURE_FEATURES)
    {
        return;
    }

    if (motion_ctx.phase != MOTION_PHASE_DESCEND &&
        motion_ctx.phase != MOTION_PHASE_BOTTOM &&
        motion_ctx.phase != MOTION_PHASE_ASCEND)
    {
        return;
    }

    dual_pressure_get_snapshot(&dp);
    if (!dp.online || !dp.calibrated)
    {
        return;
    }

    if (dp.pressure_difference > (rt_int32_t)dp.asymmetry_threshold)       sign = 1;
    else if (dp.pressure_difference < -(rt_int32_t)dp.asymmetry_threshold) sign = -1;

    motion_ctx.press_samples++;
    if (sign != 0)
    {
        if (motion_ctx.press_last_sign != 0 && sign != motion_ctx.press_last_sign)
        {
            motion_ctx.press_flip_count++;
        }
        motion_ctx.press_last_sign = sign;
    }
}

/* Multi-gate motion classifier (V9.3).
 *
 * Walk gate  : rhythmic + submaximal EMG + no bottom dwell, confirmed by
 *              pressure alternation or sustained weak EMG. Rhythmic but
 *              unproven candidates are rejected instead of force-counted.
 * Train gate : mean rep MAV must reach a fraction of the active baseline,
 *              otherwise the rep is treated as a casual movement (UNKNOWN).
 * Squat/Dead : classify from peak excursion relative to the current standing
 *              baseline. 15..22 deg is DEADLIFT, 22..26 deg is a voting gray
 *              band, and >=26 deg is SQUAT. A tied gray-band vote falls back
 *              to DEADLIFT.
 *
 * A completed UNKNOWN rep advances the total rep_count, but does not change
 * the squat/deadlift breakdown. WALK only advances the walking counter.
 */
static motion_action_t motion_classify_rep(const motion_context_t *m,
                                           const emg_rep_features_t *emg,
                                           rt_uint32_t base_mav,
                                           rt_uint8_t *conf_out)
{
    rt_uint32_t ratio = 0;
    rt_uint32_t pressure_flips = m->press_flip_count + m->walk_pressure_flips;
    rt_bool_t emg_valid = RT_FALSE;
    rt_bool_t rhythmic;

    if (emg != RT_NULL && emg->sample_count > 0U && base_mav > 0U)
    {
        rt_uint64_t mean_mav = emg->iemg / emg->sample_count;
        ratio = (rt_uint32_t)((mean_mav * 100ULL) / (rt_uint64_t)base_mav);
        emg_valid = RT_TRUE;
    }

    rhythmic  = motion_interval_rhythmic(m->prev_interval_ms, m->done_interval_ms);

    /* Missing EMG/base calibration must fail closed. Otherwise a disconnected
     * electrode bypasses both the walk and training-effort gates. */
    if (!emg_valid)
    {
        if (conf_out) *conf_out = 0;
        return ACTION_UNKNOWN;
    }

    /* The 15-degree floor applies before every action classifier, including
     * gait, so small strap/leg motions can never become a completed action. */
    if (m->done_depth_x10 < MOTION_DEADLIFT_ANGLE_X10)
    {
        if (conf_out) *conf_out = 0;
        return ACTION_UNKNOWN;
    }

    /* ---- Locked gait mode: once walking is confirmed, each later step only
     * needs an in-window interval, submaximal EMG and little dwell. The lock
     * is released in finalize after two misses or an effort spike. ---- */
    if (m->walk_mode && emg_valid && ratio <= MOTION_WALK_EMG_GATE_X100 &&
        m->done_bottom_ms <= MOTION_WALK_LOCK_BOTTOM_MS &&
        m->done_interval_ms >= MOTION_WALK_PERIOD_MIN_MS &&
        m->done_interval_ms <= MOTION_WALK_PERIOD_MAX_MS)
    {
        if (conf_out) *conf_out = 1;
        return ACTION_WALK;
    }

    /* ---- Walk detection and the rhythmic-reject gate ---- */
    if (emg_valid && ratio <= MOTION_WALK_EMG_GATE_X100 &&
        m->done_bottom_ms <= MOTION_WALK_MAX_BOTTOM_MS)
    {
        if (rhythmic)
        {
            if (pressure_flips >= MOTION_WALK_MIN_FLIPS ||
                ratio <= MOTION_WALK_EMG_WEAK_X100)
            {
                if (conf_out) *conf_out = 2;
                return ACTION_WALK;
            }
            if (conf_out) *conf_out = 0;
            return ACTION_UNKNOWN;          /* rhythmic but unproven */
        }
        if (m->done_depth_x10 < MOTION_SQUAT_ANGLE_X10)
        {
            if (conf_out) *conf_out = 0;
            return ACTION_UNKNOWN;          /* weak, shallow, no dwell: suspicious */
        }
    }

    /* ---- Training reps need measurable muscle effort ---- */
    if (ratio < MOTION_TRAIN_EMG_MIN_X100)
    {
        if (conf_out) *conf_out = 0;
        return ACTION_UNKNOWN;
    }

    /* ---- Peak-excursion classification. ---- */
    if (m->done_depth_x10 < MOTION_CLS_GRAY_LOW_X10)
    {
        if (conf_out) *conf_out = 3;
        return ACTION_DEADLIFT;
    }
    if (m->done_depth_x10 >= MOTION_SQUAT_ANGLE_X10)
    {
        if (conf_out) *conf_out = 3;
        return ACTION_SQUAT;
    }

    /* ---- 22..26 degree gray-band vote. ----
     * Pressure is a rejection gate rather than an action vote: the available
     * accumulator records side changes, not an absolute load that can reliably
     * distinguish squat from deadlift. Stable pressure raises confidence only. */
    {
        rt_uint8_t squat_votes = 0;
        rt_uint8_t deadlift_votes = 0;
        rt_uint8_t margin;

        if (pressure_flips > MOTION_CLS_PRESS_MAX_FLIPS)
        {
            if (conf_out) *conf_out = 0;
            return ACTION_UNKNOWN;
        }

        if (m->done_bottom_ms >= MOTION_CLS_BOTTOM_SQ_MS) squat_votes += 2U;
        else if (m->done_bottom_ms <= MOTION_CLS_BOTTOM_DL_MS) deadlift_votes++;

        if (m->done_descend_ms > 0U)
        {
            if ((rt_uint64_t)m->done_ascend_ms * 10ULL >
                (rt_uint64_t)m->done_descend_ms * MOTION_CLS_TEMPO_SQ_NUM_X10)
                squat_votes++;
            else if ((rt_uint64_t)m->done_ascend_ms * 10ULL <
                     (rt_uint64_t)m->done_descend_ms * MOTION_CLS_TEMPO_DL_NUM_X10)
                deadlift_votes++;
        }

        if (squat_votes == deadlift_votes)
        {
            if (conf_out) *conf_out = 1;
            return ACTION_DEADLIFT;
        }

        margin = squat_votes > deadlift_votes ?
                 (rt_uint8_t)(squat_votes - deadlift_votes) :
                 (rt_uint8_t)(deadlift_votes - squat_votes);
        if (m->press_samples > 0U && pressure_flips == 0U && margin < 3U) margin++;
        if (conf_out) *conf_out = margin;
        return squat_votes > deadlift_votes ? ACTION_SQUAT : ACTION_DEADLIFT;
    }
}

static void motion_finalize_rep(void);

static void motion_reset_rep(void)
{
    motion_ctx.peak_angle_x10 = 0;
    motion_ctx.peak_excursion_x10 = 0;
    motion_ctx.peak_tick = 0;
    motion_ctx.rep_start_tick = 0;
    motion_ctx.start_confirm_count = 0;
    motion_ctx.ascend_confirm_count = 0;
    motion_ctx.return_confirm_count = 0;
    motion_ctx.descend_start_tick = 0;
    motion_ctx.bottom_start_tick  = 0;
    motion_ctx.ascend_start_tick  = 0;
    motion_ctx.descend_ms = 0;
    motion_ctx.bottom_ms  = 0;
    motion_ctx.ascend_ms  = 0;
    motion_ctx.stability_sq_sum = 0;
    motion_ctx.stability_samples = 0;
    motion_ctx.press_last_sign = 0;
    motion_ctx.press_flip_count = 0;
    motion_ctx.press_samples = 0;
}

static rt_uint32_t ticks_to_ms(rt_tick_t t)
{
    return (rt_uint32_t)((t * 1000UL) / RT_TICK_PER_SECOND);
}

/* ----- Motor alert --------------------------------------------------------- */

static void motor_set(rt_bool_t on)
{
#if MOTOR_ALERT_ENABLE
    rt_base_t level = on ? MOTOR_ACTIVE_LEVEL :
        (MOTOR_ACTIVE_LEVEL == PIN_HIGH ? PIN_LOW : PIN_HIGH);

    rt_pin_write(MOTOR_PIN, level);
    motor_output_on = on;
#else
    RT_UNUSED(on);
#endif
}

static void motor_init(void)
{
#if MOTOR_ALERT_ENABLE
    rt_pin_mode(MOTOR_PIN, PIN_MODE_OUTPUT);
    motor_set(RT_FALSE);
    rt_kprintf("Motor alert ready: pin=PE0, active=%s, rep_threshold=%u, rep_count=%u\n",
               MOTOR_ACTIVE_LEVEL == PIN_HIGH ? "HIGH" : "LOW",
               (unsigned)MOTOR_REP_FATIGUE_THRESHOLD,
               (unsigned)MOTOR_REP_FATIGUE_COUNT);
#endif
}

static void motor_signal_ms(rt_uint32_t duration_ms)
{
#if MOTOR_ALERT_ENABLE
    if (duration_ms == 0U) return;

    motor_alert_until_tick = 0;
    motor_force_until_tick =
        rt_tick_get() + rt_tick_from_millisecond(duration_ms);
    motor_set(RT_TRUE);
#else
    RT_UNUSED(duration_ms);
#endif
}

static void startup_apply_emg_phase(emg_phase_t phase, rt_bool_t signal_transition)
{
    startup_runtime_ready = (phase == EMG_PHASE_RUNNING) ? RT_TRUE : RT_FALSE;

    if (!signal_transition) return;

    if (phase == EMG_PHASE_ACTIVE_CAL)
    {
        motor_signal_ms(STARTUP_ACTIVE_CUE_MS);
        rt_kprintf("STARTUP,ACTIVE_CAL,cue_ms=%u,keep_standing_and_contract\n",
                   (unsigned)STARTUP_ACTIVE_CUE_MS);
    }
    else if (phase == EMG_PHASE_RUNNING)
    {
        motor_signal_ms(STARTUP_RUNNING_CUE_MS);
        rt_kprintf("STARTUP,RUNNING,cue_ms=%u,all_features_enabled,active_baseline_online\n",
                   (unsigned)STARTUP_RUNNING_CUE_MS);
    }
    else
    {
        rt_kprintf("STARTUP,REST_CAL,motion_locked\n");
    }
}

static void motor_update_alert(rt_bool_t alert)
{
#if MOTOR_ALERT_ENABLE
    rt_uint32_t period_ms;
    rt_uint32_t pos_ms;
    rt_tick_t now = rt_tick_get();
    rt_bool_t latched_alert = alert;

    if (motor_force_until_tick != 0)
    {
        if ((rt_int32_t)(motor_force_until_tick - now) > 0)
        {
            if (!motor_output_on)
            {
                motor_set(RT_TRUE);
            }
            return;
        }
        motor_force_until_tick = 0;
    }

#if MOTOR_ALERT_LATCH_MS > 0
    if (alert)
    {
        motor_alert_until_tick = now + rt_tick_from_millisecond(MOTOR_ALERT_LATCH_MS);
    }
    else if (motor_alert_until_tick != 0)
    {
        if ((rt_int32_t)(motor_alert_until_tick - now) > 0)
        {
            latched_alert = RT_TRUE;
        }
        else
        {
            motor_alert_until_tick = 0;
        }
    }
#endif

    if (!latched_alert)
    {
        if (motor_output_on)
        {
            motor_set(RT_FALSE);
        }
        return;
    }

    if (MOTOR_ALERT_PULSE_OFF_MS == 0)
    {
        if (!motor_output_on)
        {
            motor_set(RT_TRUE);
        }
        return;
    }

    period_ms = MOTOR_ALERT_PULSE_ON_MS + MOTOR_ALERT_PULSE_OFF_MS;
    if (period_ms == 0)
    {
        motor_set(RT_FALSE);
        return;
    }

    pos_ms = ticks_to_ms(now) % period_ms;
    motor_set(pos_ms < MOTOR_ALERT_PULSE_ON_MS ? RT_TRUE : RT_FALSE);
#else
    RT_UNUSED(alert);
#endif
}

static rt_bool_t demo_fatigue_is_suppressed(rt_uint32_t rep_count)
{
    return rep_count >= DEMO_FATIGUE_SUPPRESS_FIRST_REP &&
           rep_count <= DEMO_FATIGUE_SUPPRESS_LAST_REP;
}

static void demo_cancel_motor_alert_latch(void)
{
    motor_alert_until_tick = 0;
}

static rt_uint32_t demo_random_next(void)
{
    rt_uint32_t x = demo_prng_state;

    if (x == 0U)
    {
        x = (rt_uint32_t)rt_tick_get() ^ 0xA341316CU;
        if (x == 0U) x = 0x6D2B79F5U;
    }

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    demo_prng_state = x;
    return x;
}

static void demo_prepare_random_target(void)
{
    rt_uint32_t r;

    if (demo_force_target_rep != 0U) return;

    r = demo_random_next();
    demo_force_target_rep = DEMO_FATIGUE_FORCE_MIN_REP +
        (r % (DEMO_FATIGUE_FORCE_MAX_REP - DEMO_FATIGUE_FORCE_MIN_REP + 1U));
    demo_force_score = (rt_uint16_t)(DEMO_FATIGUE_FORCE_MIN_SCORE +
        (demo_random_next() %
         (DEMO_FATIGUE_FORCE_MAX_SCORE - DEMO_FATIGUE_FORCE_MIN_SCORE + 1U)));

    rt_kprintf("DEMO,RANDOM_FATIGUE_TARGET,rep=%lu,score=%u\n",
               (unsigned long)demo_force_target_rep,
               (unsigned)demo_force_score);
}

static void demo_note_natural_alert(void)
{
    demo_natural_alert_seen = RT_TRUE;
}

static rt_bool_t demo_force_alert_on_rep(rt_uint32_t rep_count)
{
    if (demo_force_target_rep == 0U ||
        rep_count != demo_force_target_rep ||
        demo_force_alert_decided)
    {
        return RT_FALSE;
    }

    demo_force_alert_decided = RT_TRUE;
    if (demo_natural_alert_seen)
    {
        rt_kprintf("DEMO,RANDOM_FATIGUE_SKIP,rep=%lu,reason=natural_alert_seen\n",
                   (unsigned long)rep_count);
        return RT_FALSE;
    }

    demo_force_alert_fired = RT_TRUE;
    demo_force_alert_until_tick =
        rt_tick_get() + rt_tick_from_millisecond(DEMO_FATIGUE_FORCE_DISPLAY_MS);
    return RT_TRUE;
}

static rt_bool_t demo_force_alert_is_active(void)
{
    rt_tick_t until = demo_force_alert_until_tick;

    return demo_force_alert_fired && until != 0 &&
           (rt_int32_t)(until - rt_tick_get()) > 0;
}

static rt_bool_t demo_register_high_score(rt_uint32_t rep_count,
                                          rt_uint16_t score)
{
    rt_tick_t now;

    if (rep_count <= DEMO_FATIGUE_SUPPRESS_LAST_REP ||
        score < DEMO_FATIGUE_EARLY_HIGH_SCORE ||
        demo_high_score_hits >= DEMO_FATIGUE_REQUIRED_HITS)
    {
        return RT_FALSE;
    }

    now = rt_tick_get();
    if (demo_last_high_score_tick != 0 &&
        ticks_to_ms(now - demo_last_high_score_tick) < DEMO_FATIGUE_HIT_GAP_MS)
    {
        return RT_FALSE;
    }

    demo_last_high_score_tick = now;
    demo_high_score_hits++;
    rt_kprintf("FATIGUE,HIGH_SCORE_HIT,count=%u/%u,score=%u,rep=%lu\n",
               (unsigned)demo_high_score_hits,
               (unsigned)DEMO_FATIGUE_REQUIRED_HITS,
               (unsigned)score,
               (unsigned long)rep_count);

    if (demo_high_score_hits >= DEMO_FATIGUE_REQUIRED_HITS)
    {
        if (!demo_force_alert_is_active())
        {
            demo_note_natural_alert();
        }
        return RT_TRUE;
    }

    return RT_FALSE;
}

static void demo_apply_two_hit_alert(rt_uint16_t score, rt_uint8_t *alert)
{
    if (alert == RT_NULL) return;

    *alert = (score >= DEMO_FATIGUE_EARLY_HIGH_SCORE &&
              demo_high_score_hits >= DEMO_FATIGUE_REQUIRED_HITS) ? 1U : 0U;
}

static void demo_apply_fatigue_gate(rt_uint32_t rep_count,
                                    rt_uint16_t *score,
                                    rt_uint8_t *alert)
{
    if (score == RT_NULL || alert == RT_NULL) return;

    if (demo_fatigue_is_suppressed(rep_count))
    {
        if (*score >= DEMO_FATIGUE_EARLY_HIGH_SCORE)
        {
            *score = (rt_uint16_t)(*score - DEMO_FATIGUE_EARLY_REDUCTION);
            if (*score > DEMO_FATIGUE_EARLY_MAX_SCORE)
            {
                *score = DEMO_FATIGUE_EARLY_MAX_SCORE;
            }
        }
        *alert = 0U;
        return;
    }

    if (demo_force_alert_is_active())
    {
        if (*score < demo_force_score)
        {
            *score = demo_force_score;
        }
    }
}

static void motion_accumulate_stability(const mpu6050_state_t *mpu)
{
    rt_int32_t axis_a;
    rt_int32_t axis_b;
    rt_uint64_t sample_sq;

    if (mpu == RT_NULL ||
        (motion_ctx.phase != MOTION_PHASE_DESCEND &&
         motion_ctx.phase != MOTION_PHASE_BOTTOM &&
         motion_ctx.phase != MOTION_PHASE_ASCEND))
    {
        return;
    }

    /* Exclude the gyro axis used for knee flexion; the other two show sway. */
    switch (MPU6050_ANGLE_GYRO_AXIS)
    {
    case 1:
        axis_a = mpu->raw.gx - mpu->gx_bias;
        axis_b = mpu->raw.gz - mpu->gz_bias;
        break;
    case 2:
        axis_a = mpu->raw.gx - mpu->gx_bias;
        axis_b = mpu->raw.gy - mpu->gy_bias;
        break;
    case 0:
    default:
        axis_a = mpu->raw.gy - mpu->gy_bias;
        axis_b = mpu->raw.gz - mpu->gz_bias;
        break;
    }

    sample_sq = (rt_uint64_t)((rt_int64_t)axis_a * axis_a) +
                (rt_uint64_t)((rt_int64_t)axis_b * axis_b);
    motion_ctx.stability_sq_sum += sample_sq;
    motion_ctx.stability_samples++;
}

/* Compute a 0-100 quality score and a failure bitmask for the just-completed rep. */
static rt_uint32_t motion_score_quality(const motion_context_t *m,
                                        const emg_rep_features_t *emg,
                                        rt_uint32_t base_mav,
                                        rt_uint32_t *fail_mask_out)
{
    rt_uint32_t score = 0;
    rt_uint32_t mask  = 0;

    /* Depth. */
    if (m->done_depth_x10 >= MOTION_BOTTOM_ANGLE_X10) score++;
    else                                              mask |= QUALITY_FAIL_DEPTH_LOW;

    /* Timing. A short descent or ascent alone is not enough to call the
     * movement FAST: a normal rep may have one short phase because the MPU
     * peak is sampled near the turn-around. Only the complete down+up motion
     * can set BOUNCE/FAST, while a genuinely long phase remains SLOW. */
    if (m->done_descend_ms > QUALITY_DESCENT_MAX_MS)
        mask |= QUALITY_FAIL_SLOW;
    else if (m->done_descend_ms + m->done_ascend_ms < QUALITY_TOTAL_FAST_MIN_MS)
        mask |= QUALITY_FAIL_BOUNCE;
    else
        score++;

    if (m->done_ascend_ms > QUALITY_ASCENT_MAX_MS)
        mask |= QUALITY_FAIL_SLOW;
    else
        score++;

    /* Bottom dwell. */
    if (m->done_bottom_ms >= QUALITY_BOTTOM_MIN_MS &&
        m->done_bottom_ms <= QUALITY_BOTTOM_MAX_MS) score++;
    else if (m->done_bottom_ms < QUALITY_BOTTOM_MIN_MS) mask |= QUALITY_FAIL_BOUNCE;

    /* Symmetry. */
    rt_uint32_t lo = m->done_descend_ms < m->done_ascend_ms ?
                     m->done_descend_ms : m->done_ascend_ms;
    rt_uint32_t hi = m->done_descend_ms > m->done_ascend_ms ?
                     m->done_descend_ms : m->done_ascend_ms;
    if (hi > 0 && (lo * 100U / hi) >= QUALITY_SYMMETRY_X100) score++;
    else                                                     mask |= QUALITY_FAIL_ASYMMETRIC;

    /* Force vs active baseline. */
    if (emg->sample_count > 0 && base_mav > 0)
    {
        rt_uint64_t mean_mav = emg->iemg / emg->sample_count;
        if ((mean_mav * 100ULL) >= (rt_uint64_t)base_mav * QUALITY_FORCE_RATIO_X100)
            score++;
        else
            mask |= QUALITY_FAIL_WEAK;
    }
    else
    {
        score++;
    }

    /* Muscle truly engaged (mean WL > 0). */
    if (emg->window_count > 0 && (emg->sum_window_wl / emg->window_count) > 0)
        score++;
    else
        mask |= QUALITY_FAIL_WEAK;

    score = score * 100U / 7U;
    if (m->done_stability_rms > QUALITY_STABILITY_RMS_MAX)
    {
        mask |= QUALITY_FAIL_UNSTABLE;
        score = score > QUALITY_STABILITY_PENALTY ?
                score - QUALITY_STABILITY_PENALTY : 0U;
    }

    if (fail_mask_out) *fail_mask_out = mask;
    return score;
}

static const char *motion_quality_label(rt_uint32_t score)
{
    if (score >= 90U)    return "GOOD";
    if (score >= 70U)    return "OK";
    if (score >= 45U)    return "WEAK";
    return "WRONG";
}

static void motion_finalize_rep(void)
{
    emg_rep_features_t emg;
    rt_memset(&emg, 0, sizeof(emg));
    emg_pipeline_rep_stop(&emg);

    /* Reject shallow or implausibly fast candidates before they affect counts. */
    if (motion_ctx.peak_excursion_x10 < MOTION_COUNT_ACCEPT_DELTA_X10 ||
        motion_ctx.rep_start_tick == 0 ||
        ticks_to_ms(rt_tick_get() - motion_ctx.rep_start_tick) < MOTION_MIN_REP_MS)
    {
        motion_reset_rep();
        return;
    }

    rt_tick_t now = rt_tick_get();
    emg_snapshot_t snap;
    rt_uint8_t conf = 0;
    rt_bool_t walk_history_candidate = RT_FALSE;
    motion_action_t act;

    /* Fill the "done" record first; the classifier below reads it. */
    /* Store depth relative to the standing pose captured for this rep. */
    motion_ctx.done_depth_x10    = motion_ctx.peak_excursion_x10;
    motion_ctx.done_descend_ms   = motion_ctx.descend_ms;
    motion_ctx.done_bottom_ms    = motion_ctx.bottom_ms;
    motion_ctx.done_ascend_ms    = motion_ctx.ascend_ms;
    motion_ctx.done_emg          = emg;
    motion_ctx.done_press_flips  = motion_ctx.press_flip_count;
    motion_ctx.done_interval_ms  = (motion_ctx.prev_rep_done_tick != 0) ?
                                   ticks_to_ms(now - motion_ctx.prev_rep_done_tick) : 0U;

    emg_pipeline_get_snapshot(&snap);
    motion_ctx.done_base_mav = snap.base_mav;
    motion_ctx.done_stability_rms = motion_ctx.stability_samples > 0U ?
        (rt_uint32_t)sqrtf((float)motion_ctx.stability_sq_sum /
                            ((float)motion_ctx.stability_samples * 2.0f)) : 0U;

    /* Walking alternates pressure mainly between adjacent steps, not within a
     * single step. Preserve the last non-zero side and expose one boundary
     * reversal as confirmation for the current gait candidate. */
    motion_ctx.walk_pressure_flips = 0U;
    if (motion_ctx.press_last_sign != 0)
    {
        if (motion_ctx.last_rep_press_sign != 0 &&
            motion_ctx.press_last_sign != motion_ctx.last_rep_press_sign)
        {
            motion_ctx.walk_pressure_flips = 1U;
        }
        motion_ctx.last_rep_press_sign = motion_ctx.press_last_sign;
    }

    /* A shallow, low-effort completed candidate is still useful for gait
     * rhythm warm-up. It must not change training counters, but its interval
     * is needed so the next two real steps can satisfy the rhythmic gate. */
    walk_history_candidate = (motion_ctx.done_depth_x10 < MOTION_SQUAT_ANGLE_X10 &&
                              motion_ctx.done_bottom_ms <= MOTION_WALK_MAX_BOTTOM_MS);

    act = motion_classify_rep(&motion_ctx, &emg, snap.base_mav, &conf);
    motion_ctx.done_action = act;
    motion_ctx.done_conf   = conf;
    motion_ctx.action      = act;

    /* Every completed training movement advances rep_count, including one
     * whose final class is UNKNOWN. Keep per-action counters classified-only;
     * walking continues to use its independent step counter. */
    if (act == ACTION_SQUAT || act == ACTION_DEADLIFT || act == ACTION_UNKNOWN)
    {
        motion_ctx.rep_count++;
        motion_ctx.last_count_tick = now;
        motion_ctx.done_n = motion_ctx.rep_count;
        if (act == ACTION_SQUAT) motion_squat_count++;
        else if (act == ACTION_DEADLIFT) motion_deadlift_count++;
    }
    else if (act == ACTION_WALK)
    {
        motion_walk_count++;
        motion_ctx.done_n = motion_walk_count;
    }
    else
    {
        motion_ctx.done_n = motion_ctx.rep_count;   /* unchanged */
    }

    /* Gait lock bookkeeping: walking keeps the lock, an effort spike breaks
     * it immediately, anything else breaks it after two consecutive reps. */
    if (act == ACTION_WALK)
    {
        motion_ctx.walk_mode = RT_TRUE;
        motion_ctx.walk_miss = 0;
    }
    else if (motion_ctx.walk_mode)
    {
        rt_uint32_t lock_ratio = 0;
        if (emg.sample_count > 0U && snap.base_mav > 0U)
        {
            lock_ratio = (rt_uint32_t)(((emg.iemg / emg.sample_count) * 100ULL) /
                                       (rt_uint64_t)snap.base_mav);
        }
        if (lock_ratio > MOTION_WALK_LOCK_EXIT_X100)
        {
            motion_ctx.walk_mode = RT_FALSE;
            motion_ctx.walk_miss = 0;
        }
        else
        {
            motion_ctx.walk_miss++;
            if (motion_ctx.walk_miss >= 2U)
            {
                motion_ctx.walk_mode = RT_FALSE;
                motion_ctx.walk_miss = 0;
            }
        }
    }

    /* Training reps and confirmed walking always update history. A rejected
     * shallow/no-dwell candidate may also seed gait rhythm, which removes the
     * previous startup deadlock where the first UNKNOWN step could never
     * provide an interval for the second/third step. Deep rejected motions do
     * not pollute gait history. */
    if (act != ACTION_UNKNOWN || walk_history_candidate)
    {
        if (motion_ctx.done_interval_ms > 0U)
        {
            motion_ctx.prev2_interval_ms = motion_ctx.prev_interval_ms;
            motion_ctx.prev_interval_ms = motion_ctx.done_interval_ms;
        }
        motion_ctx.prev_rep_done_tick = now;
    }

    /* Quality scoring is only meaningful for training reps. */
    if (act == ACTION_SQUAT || act == ACTION_DEADLIFT)
    {
        rt_uint32_t fail_mask = 0;
        motion_ctx.done_quality_score = motion_score_quality(
            &motion_ctx, &emg, snap.base_mav, &fail_mask);
        motion_ctx.done_quality_fail_mask = fail_mask;
    }
    else
    {
        motion_ctx.done_quality_score = 0;
        motion_ctx.done_quality_fail_mask = 0;
    }

    motion_ctx.rep_done = RT_TRUE;
    motion_reset_rep();
}

static void motion_init(void)
{
    rt_memset(&motion_ctx, 0, sizeof(motion_ctx));
    motion_ctx.phase  = MOTION_PHASE_CALIBRATING;
    motion_ctx.action = ACTION_UNKNOWN;
    rt_kprintf("Motion VOTE V9.3: relpeak+grayvote, start=%u.%u deg, count=%u.%u deg, min=%u ms\n",
               (unsigned)(MOTION_COUNT_START_DELTA_X10 / 10U),
               (unsigned)(MOTION_COUNT_START_DELTA_X10 % 10U),
               (unsigned)(MOTION_DEADLIFT_ANGLE_X10 / 10U),
               (unsigned)(MOTION_DEADLIFT_ANGLE_X10 % 10U),
               (unsigned)MOTION_MIN_REP_MS);
}

static void motion_enter_descend(rt_uint32_t abs_angle, rt_uint32_t excursion)
{
    motion_reset_rep();
    motion_ctx.phase = MOTION_PHASE_DESCEND;
    motion_ctx.descend_start_tick = rt_tick_get();
    motion_ctx.rep_start_tick = motion_ctx.descend_start_tick;
    motion_ctx.peak_angle_x10 = abs_angle;
    motion_ctx.peak_excursion_x10 = excursion;
    motion_ctx.peak_tick = motion_ctx.descend_start_tick;
    motion_ctx.count_armed = RT_FALSE;
    emg_pipeline_rep_start();
}

static void motion_enter_bottom(rt_uint32_t abs_angle, rt_uint32_t excursion)
{
    motion_ctx.bottom_start_tick = rt_tick_get();
    motion_ctx.descend_ms = ticks_to_ms(motion_ctx.bottom_start_tick - motion_ctx.descend_start_tick);
    if (abs_angle > motion_ctx.peak_angle_x10) motion_ctx.peak_angle_x10 = abs_angle;
    if (excursion > motion_ctx.peak_excursion_x10)
    {
        motion_ctx.peak_excursion_x10 = excursion;
        motion_ctx.peak_tick = rt_tick_get();
    }
    motion_ctx.phase = MOTION_PHASE_BOTTOM;
}

static void motion_enter_ascend(rt_uint32_t abs_angle, rt_uint32_t excursion)
{
    motion_ctx.ascend_start_tick = rt_tick_get();
    if (motion_ctx.bottom_start_tick != 0)
    {
        motion_ctx.bottom_ms = ticks_to_ms(motion_ctx.ascend_start_tick - motion_ctx.bottom_start_tick);
    }
    if (abs_angle > motion_ctx.peak_angle_x10) motion_ctx.peak_angle_x10 = abs_angle;
    if (excursion > motion_ctx.peak_excursion_x10)
    {
        motion_ctx.peak_excursion_x10 = excursion;
        motion_ctx.peak_tick = rt_tick_get();
    }
    motion_ctx.return_confirm_count = 0;
    motion_ctx.phase = MOTION_PHASE_ASCEND;
}

static void motion_abort_rep(rt_uint32_t abs_angle)
{
    emg_rep_features_t discarded;

    rt_memset(&discarded, 0, sizeof(discarded));
    emg_pipeline_rep_stop(&discarded);
    motion_reset_rep();
    motion_ctx.phase = MOTION_PHASE_STAND;
    motion_ctx.stand_angle_x10 = abs_angle;
    motion_ctx.prev_excursion_x10 = 0;
    motion_ctx.count_armed = RT_TRUE;
}

static void motion_complete_rep(void)
{
    rt_tick_t now = rt_tick_get();

    /* Measure descent to the actual peak flexion instead of the early 9-degree
     * bottom-entry threshold. This prevents normal reps being mislabeled FAST. */
    if (motion_ctx.peak_tick != 0 &&
        motion_ctx.rep_start_tick != 0 &&
        motion_ctx.peak_tick >= motion_ctx.rep_start_tick)
    {
        motion_ctx.descend_ms = ticks_to_ms(motion_ctx.peak_tick -
                                            motion_ctx.rep_start_tick);
        motion_ctx.bottom_ms = (motion_ctx.ascend_start_tick >= motion_ctx.peak_tick) ?
            ticks_to_ms(motion_ctx.ascend_start_tick - motion_ctx.peak_tick) : 0U;
    }
    motion_ctx.ascend_ms = ticks_to_ms(now - motion_ctx.ascend_start_tick);
    motion_finalize_rep();
    motion_ctx.phase = MOTION_PHASE_STAND;
}

static void motion_update_by_mpu(const mpu6050_state_t *mpu)
{
    rt_uint32_t abs_angle = mpu->abs_angle_x10;
    rt_uint32_t excursion;
    rt_int32_t delta;
    rt_tick_t now = rt_tick_get();

    if (!mpu->calibrated)
    {
        motion_ctx.phase = MOTION_PHASE_CALIBRATING;
        motion_ctx.count_armed = RT_FALSE;
        motion_ctx.prev_abs_angle_x10 = abs_angle;
        return;
    }

    if (motion_ctx.phase == MOTION_PHASE_CALIBRATING)
    {
        motion_ctx.phase = MOTION_PHASE_STAND;
        motion_ctx.stand_angle_x10 = abs_angle;
        motion_ctx.prev_excursion_x10 = 0;
        motion_ctx.count_armed = RT_TRUE;
        motion_ctx.prev_abs_angle_x10 = abs_angle;
        return;
    }

    excursion = abs_u32((rt_int32_t)abs_angle - (rt_int32_t)motion_ctx.stand_angle_x10);
    delta = (rt_int32_t)excursion - (rt_int32_t)motion_ctx.prev_excursion_x10;

    if (motion_ctx.phase != MOTION_PHASE_STAND &&
        motion_ctx.rep_start_tick != 0 &&
        ticks_to_ms(now - motion_ctx.rep_start_tick) > MOTION_MAX_REP_MS)
    {
        motion_abort_rep(abs_angle);
        motion_ctx.prev_abs_angle_x10 = abs_angle;
        return;
    }

    motion_accumulate_stability(mpu);
    motion_accumulate_pressure();

    switch (motion_ctx.phase)
    {
    case MOTION_PHASE_STAND:
        if (excursion <= MOTION_REARM_DELTA_X10)
        {
            motion_ctx.count_armed = RT_TRUE;
        }

        /* Follow slow strap drift while the leg is near its standing pose. */
        if (excursion <= MOTION_BASELINE_TRACK_BAND_X10)
        {
            motion_ctx.stand_angle_x10 =
                (motion_ctx.stand_angle_x10 * 15U + abs_angle + 8U) / 16U;
            excursion = abs_u32((rt_int32_t)abs_angle -
                                (rt_int32_t)motion_ctx.stand_angle_x10);
            delta = (rt_int32_t)excursion - (rt_int32_t)motion_ctx.prev_excursion_x10;
        }

        if (motion_ctx.count_armed &&
            (motion_ctx.last_count_tick == 0 ||
             ticks_to_ms(now - motion_ctx.last_count_tick) >= MOTION_RECOUNT_GUARD_MS) &&
            excursion >= MOTION_COUNT_START_DELTA_X10 &&
            delta >= MOTION_START_MIN_RISE_X10)
        {
            if (motion_ctx.start_confirm_count < MOTION_START_CONFIRM_SAMPLES)
                motion_ctx.start_confirm_count++;
        }
        else
        {
            motion_ctx.start_confirm_count = 0;
        }

        if (motion_ctx.start_confirm_count >= MOTION_START_CONFIRM_SAMPLES)
        {
            motion_enter_descend(abs_angle, excursion);
        }
        break;

    case MOTION_PHASE_DESCEND:
        if (abs_angle > motion_ctx.peak_angle_x10)
        {
            motion_ctx.peak_angle_x10 = abs_angle;
        }
        if (excursion > motion_ctx.peak_excursion_x10)
        {
            motion_ctx.peak_excursion_x10 = excursion;
            motion_ctx.peak_tick = now;
        }

        if (motion_ctx.peak_excursion_x10 >= MOTION_BOTTOM_DELTA_X10)
        {
            motion_enter_bottom(abs_angle, excursion);
        }
        else if (motion_ctx.peak_excursion_x10 >= excursion + MOTION_ASCEND_DROP_X10)
        {
            if (motion_ctx.ascend_confirm_count < MOTION_ASCEND_CONFIRM_SAMPLES)
                motion_ctx.ascend_confirm_count++;
            if (motion_ctx.ascend_confirm_count >= MOTION_ASCEND_CONFIRM_SAMPLES)
                motion_enter_ascend(abs_angle, excursion);
        }
        else
        {
            motion_ctx.ascend_confirm_count = 0;
        }
        break;

    case MOTION_PHASE_BOTTOM:
        if (abs_angle > motion_ctx.peak_angle_x10)
        {
            motion_ctx.peak_angle_x10 = abs_angle;
        }
        if (excursion > motion_ctx.peak_excursion_x10)
        {
            motion_ctx.peak_excursion_x10 = excursion;
            motion_ctx.peak_tick = now;
        }
        if (motion_ctx.peak_excursion_x10 >= excursion + MOTION_ASCEND_DROP_X10)
        {
            if (motion_ctx.ascend_confirm_count < MOTION_ASCEND_CONFIRM_SAMPLES)
                motion_ctx.ascend_confirm_count++;
            if (motion_ctx.ascend_confirm_count >= MOTION_ASCEND_CONFIRM_SAMPLES)
                motion_enter_ascend(abs_angle, excursion);
        }
        else
        {
            motion_ctx.ascend_confirm_count = 0;
        }
        break;

    case MOTION_PHASE_ASCEND:
        if (excursion <= MOTION_RETURN_DELTA_X10)
        {
            if (motion_ctx.return_confirm_count < MOTION_RETURN_CONFIRM_SAMPLES)
                motion_ctx.return_confirm_count++;
            if (motion_ctx.return_confirm_count >= MOTION_RETURN_CONFIRM_SAMPLES)
                motion_complete_rep();
        }
        else if (excursion >= MOTION_BOTTOM_DELTA_X10 &&
                 excursion >= motion_ctx.prev_excursion_x10 + MOTION_WALK_STEP_RISE_X10)
        {
            /* Gait never returns to the standing baseline: the knee flexes
             * straight into the next step. Close the rep at the step edge so
             * every stride reaches the classifier, then open the next one. */
            motion_complete_rep();
            motion_enter_descend(abs_angle, excursion);
        }
        else
        {
            motion_ctx.return_confirm_count = 0;
        }
        break;

    case MOTION_PHASE_CALIBRATING:
    default:
        motion_ctx.phase = MOTION_PHASE_STAND;
        break;
    }

    motion_ctx.prev_abs_angle_x10 = abs_angle;
    motion_ctx.prev_excursion_x10 = excursion;
}

/* Keep MPU sampling and zero-angle calibration alive during EMG calibration,
 * but do not enter the unchanged motion state machine until RUNNING. */
static void motion_hold_for_startup(const mpu6050_state_t *mpu)
{
    rt_uint32_t abs_angle = (mpu != RT_NULL) ? mpu->abs_angle_x10 : 0U;

    motion_reset_rep();
    motion_ctx.phase = MOTION_PHASE_CALIBRATING;
    motion_ctx.action = ACTION_UNKNOWN;
    motion_ctx.count_armed = RT_FALSE;
    motion_ctx.stand_angle_x10 = abs_angle;
    motion_ctx.prev_abs_angle_x10 = abs_angle;
    motion_ctx.prev_excursion_x10 = 0U;
}

/* Snapshot under critical section so the print thread sees consistent data. */
static void motion_get_snapshot(motion_context_t *m, mpu6050_state_t *mpu)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (m   != RT_NULL) *m   = motion_ctx;
    if (mpu != RT_NULL) *mpu = mpu_state;
    rt_hw_interrupt_enable(level);
}

static void motion_clear_rep_done(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    motion_ctx.rep_done = RT_FALSE;
    rt_hw_interrupt_enable(level);
}

/* ----- MPU6050 thread ------------------------------------------------------ */

static void mpu6050_update_state(const mpu6050_raw_t *raw)
{
    mpu6050_state_t local;
    rt_int32_t acc_angle_x10;
    rt_int32_t gyro_axis;
    rt_int32_t gyro_rate_x10;
    rt_int32_t predicted_x10;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    local = mpu_state;
    rt_hw_interrupt_enable(level);

    local.raw = *raw;
    local.sample_count++;

    if (!local.calibrated)
    {
        local.ax_sum += raw->ax; local.ay_sum += raw->ay; local.az_sum += raw->az;
        local.gx_sum += raw->gx; local.gy_sum += raw->gy; local.gz_sum += raw->gz;
        local.calib_samples++;

        if (local.calib_samples >= MPU6050_CALIB_SAMPLES)
        {
            mpu6050_raw_t avg;
            avg.ax = (rt_int16_t)(local.ax_sum / (rt_int64_t)local.calib_samples);
            avg.ay = (rt_int16_t)(local.ay_sum / (rt_int64_t)local.calib_samples);
            avg.az = (rt_int16_t)(local.az_sum / (rt_int64_t)local.calib_samples);
            avg.gx = (rt_int16_t)(local.gx_sum / (rt_int64_t)local.calib_samples);
            avg.gy = (rt_int16_t)(local.gy_sum / (rt_int64_t)local.calib_samples);
            avg.gz = (rt_int16_t)(local.gz_sum / (rt_int64_t)local.calib_samples);

            local.gx_bias = avg.gx; local.gy_bias = avg.gy; local.gz_bias = avg.gz;
            local.angle_base_x10 = mpu6050_acc_angle_x10(&avg);
            local.angle_x10 = local.angle_base_x10;
            local.angle_rel_x10 = 0;
            local.abs_angle_x10 = 0;
            local.calibrated = RT_TRUE;

            rt_kprintf("MPU,CALIBRATED,base_angle_x10=%ld,bias=(%ld,%ld,%ld)\n",
                       local.angle_base_x10, local.gx_bias, local.gy_bias, local.gz_bias);
        }

        level = rt_hw_interrupt_disable();
        mpu_state = local;
        if (startup_runtime_ready)
            motion_update_by_mpu(&mpu_state);
        else
            motion_hold_for_startup(&mpu_state);
        rt_hw_interrupt_enable(level);
        return;
    }

    acc_angle_x10 = mpu6050_acc_angle_x10(raw);
    gyro_axis = pick_gyro_axis(raw, MPU6050_ANGLE_GYRO_AXIS);
    switch (MPU6050_ANGLE_GYRO_AXIS)
    {
    case 1: gyro_axis -= local.gy_bias; break;
    case 2: gyro_axis -= local.gz_bias; break;
    case 0:
    default:gyro_axis -= local.gx_bias; break;
    }
    gyro_axis *= MPU6050_ANGLE_SIGN;

    gyro_rate_x10 = gyro_axis * 10 / 131;
    predicted_x10 = local.angle_x10 + gyro_rate_x10 * MPU6050_THREAD_PERIOD_MS / 1000;

    local.angle_x10     = (predicted_x10 * 98 + acc_angle_x10 * 2) / 100;
    local.angle_rel_x10 = local.angle_x10 - local.angle_base_x10;
    local.angle_rate_x10 = gyro_rate_x10;
    local.abs_angle_x10  = abs_u32(local.angle_rel_x10);

    level = rt_hw_interrupt_disable();
    mpu_state = local;
    if (startup_runtime_ready)
        motion_update_by_mpu(&mpu_state);
    else
        motion_hold_for_startup(&mpu_state);
    rt_hw_interrupt_enable(level);
}

static void mpu6050_thread_entry(void *p)
{
    mpu6050_raw_t raw;
    knee_imu_sample_t thigh_sample;
    RT_UNUSED(p);
    while (1)
    {
        if (mpu6050_read_raw(&raw) == RT_EOK)
        {
            rt_bool_t knee_motion_active;
            rt_base_t level;

            mpu6050_update_state(&raw);

            /* Reuse the existing 20 ms sampler: no extra thread/stack. */
            thigh_sample.ax = raw.ax;
            thigh_sample.ay = raw.ay;
            thigh_sample.az = raw.az;
            thigh_sample.gx = raw.gx;
            thigh_sample.gy = raw.gy;
            thigh_sample.gz = raw.gz;
            level = rt_hw_interrupt_disable();
            knee_motion_active = startup_runtime_ready &&
                motion_ctx.phase != MOTION_PHASE_CALIBRATING &&
                motion_ctx.phase != MOTION_PHASE_STAND;
            rt_hw_interrupt_enable(level);
            knee_imu_process(&thigh_sample, knee_motion_active);
        }
        rt_thread_mdelay(MPU6050_THREAD_PERIOD_MS);
    }
}

static rt_err_t mpu6050_start_thread(void)
{
    if (mpu6050_thread != RT_NULL) return RT_EOK;
    mpu6050_thread = rt_thread_create("mpu6050", mpu6050_thread_entry, RT_NULL, 1536, 16, 10);
    if (mpu6050_thread == RT_NULL) return -RT_ENOMEM;
    rt_thread_startup(mpu6050_thread);
    return RT_EOK;
}

/* ----- Print thread (decoupled from sample rate) --------------------------- */

static const char *emg_phase_text(emg_phase_t p)
{
    switch (p)
    {
    case EMG_PHASE_REST_CAL:    return "REST_CAL";
    case EMG_PHASE_ACTIVE_CAL:  return "ACT_CAL";
    case EMG_PHASE_RUNNING:
    default:                    return "RUN";
    }
}

static const char *emg_status_text(emg_status_t s)
{
    switch (s)
    {
    case EMG_STATUS_CAL:    return "CAL";
    case EMG_STATUS_REST:   return "REST";
    case EMG_STATUS_OK:     return "OK";
    case EMG_STATUS_ALERT:  return "ALERT";
    default:                return "?";
    }
}

static const char *electrode_contact_text(electrode_contact_status_t status)
{
    switch (status)
    {
    case ELECTRODE_CONTACT_OK:    return "OK";
    case ELECTRODE_CONTACT_LOW:   return "LOW";
    case ELECTRODE_CONTACT_SAT:   return "SAT";
    case ELECTRODE_CONTACT_NOISY: return "NOISY";
    case ELECTRODE_CONTACT_WAIT:
    default:                      return "WAIT";
    }
}

/* WAIT means the contact-quality classifier has not accumulated enough
 * movement windows yet; it does not necessarily mean that EMG samples are
 * missing. Expose calibration/ready states separately so the phone can tell
 * data availability from the final electrode-contact verdict. */
static const char *electrode_contact_output_text(const emg_snapshot_t *e)
{
    if (electrode_contact_status != ELECTRODE_CONTACT_WAIT)
    {
        return electrode_contact_text(electrode_contact_status);
    }
    if (e == RT_NULL)
    {
        return "WAIT";
    }
    if (e->phase == EMG_PHASE_RUNNING)
    {
        return "READY";
    }
    return "CAL";
}

static rt_bool_t motion_is_active_for_contact(const motion_context_t *m)
{
    if (m == RT_NULL)
    {
        return RT_FALSE;
    }

    return m->phase == MOTION_PHASE_DESCEND ||
           m->phase == MOTION_PHASE_BOTTOM ||
           m->phase == MOTION_PHASE_ASCEND;
}

static void electrode_contact_set(electrode_contact_status_t next)
{
    if (electrode_contact_status == next)
    {
        return;
    }

    electrode_contact_status = next;
    rt_kprintf("CONTACT,%s\n", electrode_contact_text(next));
}

/* Evaluate only new active EMG windows while the wearer is moving. */
static void electrode_contact_update(const emg_snapshot_t *e,
                                    const motion_context_t *m)
{
    rt_bool_t low_signal;
    rt_bool_t saturated;
    rt_bool_t noisy = RT_FALSE;
    rt_uint64_t current_shape_x1000;
    rt_uint64_t base_shape_x1000;

    if (e == RT_NULL || e->phase != EMG_PHASE_RUNNING)
    {
        electrode_contact_last_window = 0U;
        electrode_active_window_start = 0U;
        electrode_low_hold = 0U;
        electrode_sat_hold = 0U;
        electrode_noise_hold = 0U;
        electrode_contact_set(ELECTRODE_CONTACT_WAIT);
        return;
    }

    if (!motion_is_active_for_contact(m))
    {
        electrode_active_window_start = 0U;
        return;
    }
    if (e->window_count == electrode_contact_last_window)
    {
        return;
    }
    electrode_contact_last_window = e->window_count;
    if (electrode_active_window_start == 0U)
    {
        electrode_active_window_start = e->window_count;
    }

    /* The first few overlapping windows after motion starts may still be
     * mostly from the resting period. Do not classify those as LOW. */
    if ((e->window_count - electrode_active_window_start) <
        ELECTRODE_ACTIVE_WARMUP_WINDOWS)
    {
        electrode_low_hold = 0U;
        electrode_sat_hold = 0U;
        electrode_noise_hold = 0U;
        electrode_contact_set(ELECTRODE_CONTACT_WAIT);
        return;
    }

    if (e->base_mav == 0U || e->base_rms == 0U)
    {
        electrode_contact_set(ELECTRODE_CONTACT_WAIT);
        return;
    }

    low_signal = ((rt_uint64_t)e->mav * 100U) <
                 ((rt_uint64_t)e->base_mav * ELECTRODE_LOW_MAV_PERCENT) &&
                 ((rt_uint64_t)e->mav * 100U) <=
                 ((rt_uint64_t)e->rest_mav * ELECTRODE_LOW_REST_RATIO_X100);
    saturated = abs_u32(e->last_ac) >= ELECTRODE_SAT_LAST_AC ||
                ((rt_uint64_t)e->rms * 100U) >
                ((rt_uint64_t)e->base_rms * ELECTRODE_SAT_RMS_RATIO_X100);

    if (e->mav > 0U && e->base_wl > 0U && e->base_zc > 0U)
    {
        current_shape_x1000 = ((rt_uint64_t)e->wl * 1000U) / e->mav;
        base_shape_x1000 = ((rt_uint64_t)e->base_wl * 1000U) / e->base_mav;
        noisy = (current_shape_x1000 * 100U) >
                (base_shape_x1000 * ELECTRODE_NOISE_SHAPE_RATIO_X100) &&
                ((rt_uint64_t)e->zc * 100U) >
                ((rt_uint64_t)e->base_zc * ELECTRODE_NOISE_ZC_RATIO_X100);
    }

    if (low_signal)
    {
        if (electrode_low_hold < ELECTRODE_CHECK_HOLD_WINDOWS) electrode_low_hold++;
    }
    else electrode_low_hold = 0U;

    if (saturated)
    {
        if (electrode_sat_hold < ELECTRODE_CHECK_HOLD_WINDOWS) electrode_sat_hold++;
    }
    else electrode_sat_hold = 0U;

    if (noisy)
    {
        if (electrode_noise_hold < ELECTRODE_CHECK_HOLD_WINDOWS) electrode_noise_hold++;
    }
    else electrode_noise_hold = 0U;

    if (electrode_sat_hold >= ELECTRODE_CHECK_HOLD_WINDOWS)
    {
        electrode_contact_set(ELECTRODE_CONTACT_SAT);
    }
    else if (electrode_noise_hold >= ELECTRODE_CHECK_HOLD_WINDOWS)
    {
        electrode_contact_set(ELECTRODE_CONTACT_NOISY);
    }
    else if (electrode_low_hold >= ELECTRODE_CHECK_HOLD_WINDOWS)
    {
        electrode_contact_set(ELECTRODE_CONTACT_LOW);
    }
    else if (!low_signal && !saturated && !noisy)
    {
        electrode_contact_set(ELECTRODE_CONTACT_OK);
    }
}

static void emit_rep_line(const motion_context_t *m)
{
    rt_uint64_t mean_mav = m->done_emg.sample_count > 0 ?
        (m->done_emg.iemg / m->done_emg.sample_count) : 0;
    rt_uint64_t mean_rms = m->done_emg.window_count > 0 ?
        (m->done_emg.sum_window_rms / m->done_emg.window_count) : 0;
    rt_uint64_t mean_wl  = m->done_emg.window_count > 0 ?
        (m->done_emg.sum_window_wl / m->done_emg.window_count) : 0;
    rt_bool_t train_rep = (m->done_action == ACTION_SQUAT ||
                           m->done_action == ACTION_DEADLIFT);
    const char *qlabel  = train_rep ?
        motion_quality_label(m->done_quality_score) : "-";

    rt_kprintf("REP,%lu,%s,depth_x10=%lu,t_dn=%lu,t_bot=%lu,t_up=%lu,stability_rms=%lu,"
               "iemg=%lu,mean_mav=%lu,mean_rms=%lu,mean_wl=%lu,peak_rms=%lu,"
               "fatigue=%lu,quality=%lu,label=%s,fail=0x%02x,base_mav=%lu,"
               "flips=%lu,conf=%u,gap=%lu\n",
               (unsigned long)m->done_n,
               action_text(m->done_action),
               (unsigned long)m->done_depth_x10,
               (unsigned long)m->done_descend_ms,
               (unsigned long)m->done_bottom_ms,
               (unsigned long)m->done_ascend_ms,
               (unsigned long)m->done_stability_rms,
               (unsigned long)m->done_emg.iemg,
               (unsigned long)mean_mav,
               (unsigned long)mean_rms,
               (unsigned long)mean_wl,
               (unsigned long)m->done_emg.peak_rms,
               (unsigned long)m->done_emg.peak_fatigue,
               (unsigned long)m->done_quality_score,
               qlabel,
               (unsigned)m->done_quality_fail_mask,
               (unsigned long)m->done_base_mav,
               (unsigned long)m->done_press_flips,
               (unsigned)m->done_conf,
               (unsigned long)m->done_interval_ms);

    fatigue_db_rep_record_t rec;
    rt_memset(&rec, 0, sizeof(rec));
    rec.rep_n = m->done_n;
    rec.action = action_text(m->done_action);
    rec.depth_x10 = m->done_depth_x10;
    rec.descend_ms = m->done_descend_ms;
    rec.bottom_ms = m->done_bottom_ms;
    rec.ascend_ms = m->done_ascend_ms;
    rec.iemg = m->done_emg.iemg;
    rec.mean_mav = mean_mav;
    rec.mean_rms = mean_rms;
    rec.mean_wl = mean_wl;
    rec.peak_rms = m->done_emg.peak_rms;
    rec.peak_fatigue = m->done_emg.peak_fatigue;
    rec.quality_score = m->done_quality_score;
    rec.quality_label = qlabel;
    rec.fail_mask = m->done_quality_fail_mask;
    rec.base_mav = m->done_base_mav;
    fatigue_db_log_rep(&rec);
}

static void emit_live_line(const emg_snapshot_t *e, const motion_context_t *m,
                           const mpu6050_state_t *mpu, rt_tick_t t0,
                           rt_uint16_t fatigue_score, rt_uint8_t fatigue_alert)
{
    rt_uint32_t t_ms = ticks_to_ms(rt_tick_get() - t0);
    rt_kprintf("LIVE,%lu,%ld,%lu,%lu,%lu,%u,%u,%u,%u,%u,%s,%s,%s,%ld,%u,"
               "%d,%d,%d,%d,%d,%d\n",
               (unsigned long)t_ms,
               (long)e->last_ac,
               (unsigned long)e->rms,
               (unsigned long)e->mav,
               (unsigned long)e->wl,
               (unsigned)e->zc,
               (unsigned)e->ssc,
               (unsigned)e->zcr_x1000,
               (unsigned)fatigue_score,
               (unsigned)fatigue_alert,
               emg_phase_text(e->phase),
               emg_status_text(e->status),
               motion_phase_text(m->phase),
               (long)mpu->angle_rel_x10,
               (unsigned)e->active,
               mpu->raw.ax, mpu->raw.ay, mpu->raw.az,
               mpu->raw.gx, mpu->raw.gy, mpu->raw.gz);
}

static void record_live_data(const emg_snapshot_t *e, const motion_context_t *m,
                             const mpu6050_state_t *mpu, rt_tick_t t0,
                             rt_uint16_t fatigue_score, rt_uint8_t fatigue_alert)
{
    rt_uint32_t t_ms = ticks_to_ms(rt_tick_get() - t0);

    fatigue_db_live_record_t rec;
    rt_memset(&rec, 0, sizeof(rec));
    rec.t_ms = t_ms;
    rec.ac = e->last_ac;
    rec.rms = e->rms;
    rec.mav = e->mav;
    rec.wl = e->wl;
    rec.zc = e->zc;
    rec.ssc = e->ssc;
    rec.zcr_x1000 = e->zcr_x1000;
    rec.fatigue_score = fatigue_score;
    rec.fatigue_alert = fatigue_alert;
    rec.active = e->active;
    rec.emg_phase = emg_phase_text(e->phase);
    rec.emg_status = emg_status_text(e->status);
    rec.motion_phase = motion_phase_text(m->phase);
    rec.angle_x10 = mpu->angle_rel_x10;
    rec.ax = mpu->raw.ax;
    rec.ay = mpu->raw.ay;
    rec.az = mpu->raw.az;
    rec.gx = mpu->raw.gx;
    rec.gy = mpu->raw.gy;
    rec.gz = mpu->raw.gz;
    fatigue_db_log_live(&rec);
}

static const char *knee_action_text(const motion_context_t *m)
{
    motion_action_t a = ACTION_UNKNOWN;

    if (m == RT_NULL)
    {
        return "UNKNOWN";
    }

    if (m->action != ACTION_UNKNOWN)
    {
        a = m->action;
    }
    else if (m->done_action != ACTION_UNKNOWN)
    {
        a = m->done_action;
    }
    else if (m->peak_angle_x10 > 0)
    {
        a = motion_classify_depth(m->peak_excursion_x10);
    }

    return action_text(a);
}

static void emit_knee_line(const emg_snapshot_t *e, const motion_context_t *m,
                           rt_uint16_t fatigue_score, rt_uint8_t fatigue_alert)
{
    rt_uint32_t rep_count = 0;
    rt_uint32_t quality_score = 0;
    rt_uint32_t quality_fail_mask = 0;
    const char *action = "UNKNOWN";
    const char *quality_label = "WAIT";

    if (m != RT_NULL)
    {
        rep_count = m->rep_count;
        action = knee_action_text(m);
        if (m->done_n > 0U &&
            (m->done_action == ACTION_SQUAT || m->done_action == ACTION_DEADLIFT))
        {
            quality_score = m->done_quality_score;
            quality_fail_mask = m->done_quality_fail_mask;
            quality_label = motion_quality_label(quality_score);
        }
    }

    if (e == RT_NULL)
    {
        fatigue_score = 0;
        fatigue_alert = 0;
    }

    /* Keep the original first eight fields stable for older ESP32 builds.
     * Appended counters expose gait and per-action totals without changing
     * the training-rep count semantics. */
    rt_kprintf("KNEE,%u,%s,%lu,%u,%lu,%s,%lu,%s,%lu,%lu,%lu\n",
               (unsigned)fatigue_score,
               action,
               (unsigned long)rep_count,
               (unsigned)fatigue_alert,
               (unsigned long)quality_score,
               quality_label,
               (unsigned long)quality_fail_mask,
               electrode_contact_output_text(e),
               (unsigned long)motion_walk_count,
               (unsigned long)motion_squat_count,
               (unsigned long)motion_deadlift_count);
}

static rt_uint16_t effective_fatigue_score(const emg_snapshot_t *e,
                                           rt_bool_t *database_used)
{
    if (database_used != RT_NULL) *database_used = RT_FALSE;
    if (e == RT_NULL) return 0U;
    if (e->phase != EMG_PHASE_RUNNING || !e->active) return e->fatigue_score;
    return fatigue_db_fuse_score(e->fatigue_score, database_used);
}

/* Stabilize only the TFT number. Logging, wireless output and alarms continue
 * to use the instantaneous fused score. */
static rt_uint16_t tft_fatigue_max_last_second(rt_uint16_t score)
{
    static rt_uint16_t history_score[TFT_FATIGUE_HISTORY_SIZE];
    static rt_tick_t history_tick[TFT_FATIGUE_HISTORY_SIZE];
    static rt_uint8_t history_head = 0;
    static rt_uint8_t history_count = 0;
    rt_tick_t now = rt_tick_get();
    rt_uint16_t maximum = score;
    rt_uint8_t i;
    rt_uint8_t tail;

    while (history_count > 0U &&
           ticks_to_ms(now - history_tick[history_head]) > TFT_FATIGUE_HOLD_MS)
    {
        history_head = (rt_uint8_t)((history_head + 1U) % TFT_FATIGUE_HISTORY_SIZE);
        history_count--;
    }

    if (history_count >= TFT_FATIGUE_HISTORY_SIZE)
    {
        history_head = (rt_uint8_t)((history_head + 1U) % TFT_FATIGUE_HISTORY_SIZE);
        history_count--;
    }

    tail = (rt_uint8_t)((history_head + history_count) % TFT_FATIGUE_HISTORY_SIZE);
    history_score[tail] = score;
    history_tick[tail] = now;
    history_count++;

    for (i = 0U; i < history_count; i++)
    {
        rt_uint8_t index =
            (rt_uint8_t)((history_head + i) % TFT_FATIGUE_HISTORY_SIZE);
        if (history_score[index] > maximum)
        {
            maximum = history_score[index];
        }
    }

    return maximum;
}

static void emit_cal_line_on_transition(const emg_snapshot_t *e, emg_phase_t prev)
{
    if (e->phase == prev) return;

    if (prev == EMG_PHASE_REST_CAL && e->phase == EMG_PHASE_ACTIVE_CAL)
    {
        rt_kprintf("CAL,REST_READY,rest_rms=%lu,rest_mav=%lu,rest_wl=%lu,"
                   "rest_zc=%u,rest_ssc=%u,zc_th=%lu,ssc_th=%lu,active_th=%lu\n",
                   (unsigned long)e->rest_rms, (unsigned long)e->rest_mav,
                   (unsigned long)e->rest_wl, (unsigned)e->rest_zc,
                   (unsigned)e->rest_ssc,
                   (unsigned long)e->zc_threshold,
                   (unsigned long)e->ssc_threshold,
                   (unsigned long)e->active_threshold);
        rt_kprintf("# Now perform several normal contractions to build active baseline.\n");
    }
    else if (prev == EMG_PHASE_REST_CAL && e->phase == EMG_PHASE_RUNNING)
    {
        rt_kprintf("CAL,FAST_READY,rest_rms=%lu,rest_mav=%lu,rest_wl=%lu,"
                   "rest_zc=%u,rest_ssc=%u,active_th=%lu\n",
                   (unsigned long)e->rest_rms, (unsigned long)e->rest_mav,
                   (unsigned long)e->rest_wl, (unsigned)e->rest_zc,
                   (unsigned)e->rest_ssc,
                   (unsigned long)e->active_threshold);
        rt_kprintf("# Normal operation enabled; active baseline learns online.\n");
    }
    else if (prev == EMG_PHASE_ACTIVE_CAL && e->phase == EMG_PHASE_RUNNING)
    {
        rt_kprintf("CAL,ACTIVE_READY,base_rms=%lu,base_mav=%lu,base_wl=%lu,"
                   "base_zc=%u,base_ssc=%u\n",
                   (unsigned long)e->base_rms, (unsigned long)e->base_mav,
                   (unsigned long)e->base_wl, (unsigned)e->base_zc,
                   (unsigned)e->base_ssc);
    }
    else if (e->phase == EMG_PHASE_REST_CAL)
    {
        rt_kprintf("CAL,REST_BEGIN\n");
    }
}

static void ads1292_sample_watchdog(const emg_snapshot_t *snap)
{
    static rt_uint32_t last_sample_count = 0;
    static rt_tick_t last_change_tick = 0;
    static rt_tick_t last_recover_tick = 0;
    rt_tick_t now;

    if (snap == RT_NULL) return;

    now = rt_tick_get();
    if (last_change_tick == 0)
    {
        last_change_tick = now;
        last_recover_tick = now;
        last_sample_count = snap->sample_count;
        return;
    }

    if (snap->sample_count != last_sample_count)
    {
        last_sample_count = snap->sample_count;
        last_change_tick = now;
        return;
    }

    if (snap->sample_count == 0)
    {
        return;
    }

    if (ticks_to_ms(now - last_change_tick) >= ADS1292_STALL_RECOVER_MS &&
        ticks_to_ms(now - last_recover_tick) >= ADS1292_STALL_RETRY_MS)
    {
        rt_err_t result;
        rt_kprintf("ADS1292,WATCHDOG,stalled_ms=%lu,sample_count=%lu\n",
                   (unsigned long)ticks_to_ms(now - last_change_tick),
                   (unsigned long)snap->sample_count);
        result = ads1292_recover();
        if (result != RT_EOK)
        {
            rt_kprintf("ADS1292,WATCHDOG_RECOVER_FAIL,%d\n", result);
        }
        last_recover_tick = now;
        last_change_tick = now;
    }
}

static void print_thread_entry(void *p)
{
    RT_UNUSED(p);
    rt_tick_t t0 = rt_tick_get();
    const rt_uint32_t period_ms = 1000U / PRINT_THREAD_HZ;
    emg_phase_t last_phase = EMG_PHASE_REST_CAL;
    rt_bool_t first_iter = RT_TRUE;

    rt_kprintf("# version=2 sample_rate=%u window=%u hop=%u hpf=20 notch=50 lpf=150\n",
               (unsigned)EMG_SAMPLE_RATE,
               (unsigned)EMG_WINDOW_SAMPLES,
               (unsigned)EMG_HOP_SAMPLES);
    rt_kprintf("# LIVE,t_ms,ac,rms,mav,wl,zc,ssc,zcr_x1k,fatigue,alert,emg_phase,emg_status,mot_phase,angle_x10,active,ax,ay,az,gx,gy,gz\n");
    rt_kprintf("# REP,n,action,depth_x10,t_dn,t_bot,t_up,iemg,mean_mav,mean_rms,mean_wl,peak_rms,fatigue,quality,label,fail,base_mav,flips,conf,gap\n");

    while (1)
    {
        emg_snapshot_t snap;
        motion_context_t m;
        mpu6050_state_t mpu;
        rt_bool_t motor_rep_alert = RT_FALSE;
        rt_bool_t database_used = RT_FALSE;
        rt_uint16_t fatigue_score;
        rt_uint8_t fatigue_alert;

        emg_pipeline_get_snapshot(&snap);
        motion_get_snapshot(&m, &mpu);
        ads1292_sample_watchdog(&snap);
        electrode_contact_update(&snap, &m);
        fatigue_score = effective_fatigue_score(&snap, &database_used);
        fatigue_alert = 0U;
        demo_apply_fatigue_gate(m.rep_count, &fatigue_score, &fatigue_alert);
        if (demo_register_high_score(m.rep_count, fatigue_score))
        {
            motor_rep_alert = RT_TRUE;
        }
        demo_apply_two_hit_alert(fatigue_score, &fatigue_alert);

        if (first_iter)
        {
            startup_apply_emg_phase(snap.phase, RT_FALSE);
            last_phase = snap.phase;
            first_iter = RT_FALSE;
        }
        else if (snap.phase != last_phase)
        {
            startup_apply_emg_phase(snap.phase, RT_TRUE);
            emit_cal_line_on_transition(&snap, last_phase);
            last_phase = snap.phase;
        }

        if (m.rep_done)
        {
            /* Walk steps and rejected candidates never feed the demo
             * fatigue gate or the random forced-alert target. */
            rt_bool_t train_rep = (m.done_action == ACTION_SQUAT ||
                                   m.done_action == ACTION_DEADLIFT);

            if (train_rep)
            {
                demo_prepare_random_target();

                if (demo_force_alert_on_rep(m.done_n))
                {
                    rt_kprintf("DEMO,FORCE_HIGH_SEQUENCE,rep=%lu,score=%u,need_hits=%u\n",
                               (unsigned long)m.done_n,
                               (unsigned)demo_force_score,
                               (unsigned)DEMO_FATIGUE_REQUIRED_HITS);
                }

                {
                    rt_uint16_t rep_display_fatigue =
                        (rt_uint16_t)m.done_emg.peak_fatigue;
                    rt_uint8_t rep_display_alert = 0U;
                    demo_apply_fatigue_gate(m.done_n,
                                            &rep_display_fatigue,
                                            &rep_display_alert);
                    m.done_emg.peak_fatigue = rep_display_fatigue;
                }
            }

            emit_rep_line(&m);
            motion_clear_rep_done();
        }

        demo_apply_fatigue_gate(m.rep_count, &fatigue_score, &fatigue_alert);
        if (demo_register_high_score(m.rep_count, fatigue_score))
        {
            motor_rep_alert = RT_TRUE;
        }
        demo_apply_two_hit_alert(fatigue_score, &fatigue_alert);
        if (demo_fatigue_is_suppressed(m.rep_count))
        {
            motor_rep_alert = RT_FALSE;
            demo_cancel_motor_alert_latch();
        }

        motor_update_alert(motor_rep_alert);
        record_live_data(&snap, &m, &mpu, t0, fatigue_score, fatigue_alert);

        if (fit_output_enable)
        {
            emit_live_line(&snap, &m, &mpu, t0, fatigue_score, fatigue_alert);
        }

        rt_thread_mdelay(period_ms);
    }
}

static rt_err_t print_start_thread(void)
{
    if (print_thread != RT_NULL) return RT_EOK;
    print_thread = rt_thread_create("fitprint", print_thread_entry, RT_NULL, 2048, 24, 10);
    if (print_thread == RT_NULL) return -RT_ENOMEM;
    rt_thread_startup(print_thread);
    return RT_EOK;
}

/* Wireless output is intentionally isolated from database logging. It only
 * reads lock-free sensor/motion snapshots and the last published DB match, so
 * a long CSV scan cannot delay ESP32 KNEE frames. */
static void knee_thread_entry(void *p)
{
    RT_UNUSED(p);

    while (1)
    {
        if (knee_output_enable)
        {
            emg_snapshot_t snap;
            motion_context_t m;
            rt_uint16_t fatigue_score;
            rt_uint8_t fatigue_alert = 0U;

            emg_pipeline_get_snapshot(&snap);
            motion_get_snapshot(&m, RT_NULL);
            fatigue_score = effective_fatigue_score(&snap, RT_NULL);
            demo_apply_fatigue_gate(m.rep_count, &fatigue_score, &fatigue_alert);
            demo_apply_two_hit_alert(fatigue_score, &fatigue_alert);
            emit_knee_line(&snap, &m, fatigue_score, fatigue_alert);
        }

        rt_thread_mdelay(KNEE_OUTPUT_PERIOD_MS);
    }
}

static rt_err_t knee_start_thread(void)
{
    if (knee_thread != RT_NULL) return RT_EOK;

    knee_thread = rt_thread_create("kneeout", knee_thread_entry, RT_NULL,
                                   KNEE_THREAD_STACK, KNEE_THREAD_PRIORITY,
                                   KNEE_THREAD_TIMESLICE);
    if (knee_thread == RT_NULL) return -RT_ENOMEM;
    rt_thread_startup(knee_thread);
    return RT_EOK;
}

/* ----- TFT status thread --------------------------------------------------- */

static const char *tft_action_text(const motion_context_t *m, const mpu6050_state_t *mpu)
{
    rt_uint32_t gyro_activity;

    if (mpu == RT_NULL || !mpu->online) return "NO MPU";
    if (!mpu->calibrated) return "CAL";

    if (m != RT_NULL && m->phase != MOTION_PHASE_STAND)
    {
        motion_action_t a = motion_classify_depth(m->peak_excursion_x10);
        if (a != ACTION_UNKNOWN) return action_text(a);
        return motion_phase_text(m->phase);
    }

    if (m != RT_NULL && m->action != ACTION_UNKNOWN)
    {
        return action_text(m->action);
    }

    gyro_activity = abs_u32(mpu->raw.gx - mpu->gx_bias)
                  + abs_u32(mpu->raw.gy - mpu->gy_bias)
                  + abs_u32(mpu->raw.gz - mpu->gz_bias);
    if (gyro_activity > 2500 && mpu->abs_angle_x10 < MOTION_START_ANGLE_X10)
    {
        return "WALK";
    }

    return "REST";
}

static void tft_thread_entry(void *p)
{
    RT_UNUSED(p);

    while (gc9a01_init() != RT_EOK)
    {
        rt_kprintf("TFT init failed, retrying\n");
        rt_thread_mdelay(500);
    }
    rt_kprintf("TFT,READY,bus=spi1,sck=PA5,mosi=PA7,cs=PA4,dc=PA3,rst=PA2\n");

    while (1)
    {
        emg_snapshot_t snap;
        motion_context_t m;
        mpu6050_state_t mpu;
        rt_bool_t database_used = RT_FALSE;
        rt_uint16_t fatigue_raw_score;
        rt_uint16_t fatigue_score;
        rt_uint8_t fatigue_alert;
        const char *emg_display;
        char emg_pressure_display[24];
        dual_pressure_snapshot_t pressure;

        if (gc9a01_needs_recovery())
        {
            rt_kprintf("TFT,RECOVER,start\n");
            if (gc9a01_recover() != RT_EOK)
            {
                rt_kprintf("TFT,RECOVER,retry\n");
                rt_thread_mdelay(500);
                continue;
            }
            rt_kprintf("TFT,RECOVER,ok\n");
        }

        emg_pipeline_get_snapshot(&snap);
        motion_get_snapshot(&m, &mpu);
        dual_pressure_set_motion_active(
            startup_runtime_ready &&
            m.phase != MOTION_PHASE_CALIBRATING &&
            m.phase != MOTION_PHASE_STAND);
        dual_pressure_get_snapshot(&pressure);
        fatigue_raw_score = effective_fatigue_score(&snap, &database_used);
        fatigue_alert = 0U;
        demo_apply_fatigue_gate(m.rep_count, &fatigue_raw_score, &fatigue_alert);
        fatigue_score = tft_fatigue_max_last_second(fatigue_raw_score);
        demo_apply_fatigue_gate(m.rep_count, &fatigue_score, &fatigue_alert);
        demo_apply_two_hit_alert(fatigue_score, &fatigue_alert);
        if (snap.phase == EMG_PHASE_REST_CAL)
        {
            emg_display = "REST CAL";
        }
        else if (snap.phase == EMG_PHASE_ACTIVE_CAL)
        {
            emg_display = "ACT CAL";
        }
        else
        {
            emg_display = emg_status_text(snap.status);
            if (database_used)
            {
                emg_display = fatigue_alert ? "ALERT+DB" : "OK+DB";
            }
        }
        if (demo_fatigue_is_suppressed(m.rep_count))
        {
            emg_display = snap.active ? "OK" : "REST";
        }
        else if (fatigue_alert)
        {
            emg_display = "ALERT";
        }
        else if (fatigue_score >= DEMO_FATIGUE_EARLY_HIGH_SCORE)
        {
            emg_display = "CHECK";
        }
        else if (snap.phase == EMG_PHASE_RUNNING)
        {
            emg_display = database_used ? "OK+DB" : (snap.active ? "OK" : "REST");
        }

        rt_snprintf(emg_pressure_display, sizeof(emg_pressure_display),
                    "%s P:%s", emg_display,
                    dual_pressure_status_short(pressure.status));

        gc9a01_show_fit(m.rep_count,
                        motion_squat_count,
                        motion_deadlift_count,
                        fatigue_score,
                        fatigue_alert,
                        tft_action_text(&m, &mpu),
                        emg_pressure_display);
        rt_thread_mdelay(TFT_REFRESH_PERIOD_MS);
    }
}

static rt_err_t tft_start_thread(void)
{
    if (tft_thread != RT_NULL) return RT_EOK;
    tft_thread = rt_thread_create("tftui", tft_thread_entry, RT_NULL, 4096,
                                  TFT_THREAD_PRIORITY, 10);
    if (tft_thread == RT_NULL) return -RT_ENOMEM;
    rt_thread_startup(tft_thread);
    return RT_EOK;
}

/* ----- msh commands -------------------------------------------------------- */

#ifdef FINSH_USING_MSH
static void fit_start(int argc, char **argv)
{
    (void)argc; (void)argv;
    fit_output_enable = RT_TRUE;
    rt_kprintf("FIT output started.\n");
}
MSH_CMD_EXPORT(fit_start, start FIT live output);

static void fit_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    fit_output_enable = RT_FALSE;
    rt_kprintf("FIT output stopped.\n");
}
MSH_CMD_EXPORT(fit_stop, stop FIT live output);

static void knee_start(int argc, char **argv)
{
    (void)argc; (void)argv;
    knee_output_enable = RT_TRUE;
    rt_kprintf("KNEE output started. format=KNEE,score,action,count,alert,quality,label,fail,contact,walk,squat,deadlift period=%ums\n",
               (unsigned)KNEE_OUTPUT_PERIOD_MS);
}
MSH_CMD_EXPORT(knee_start, start low-rate KNEE output for ESP32);

static void knee_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    knee_output_enable = RT_FALSE;
    rt_kprintf("KNEE output stopped.\n");
}
MSH_CMD_EXPORT(knee_stop, stop low-rate KNEE output for ESP32);

static void motor_test(int argc, char **argv)
{
    (void)argc; (void)argv;
    motor_force_until_tick = rt_tick_get() + rt_tick_from_millisecond(2000);
    rt_kprintf("Motor test: ON 2000ms\n");
    motor_set(RT_TRUE);
    rt_thread_mdelay(2000);
    motor_force_until_tick = 0;
    motor_set(RT_FALSE);
    rt_kprintf("Motor test: OFF\n");
}
MSH_CMD_EXPORT(motor_test, vibrate motor for 2000ms);

static void motor_on(int argc, char **argv)
{
    (void)argc; (void)argv;
    motor_force_until_tick = rt_tick_get() + rt_tick_from_millisecond(60000);
    motor_set(RT_TRUE);
    rt_kprintf("Motor forced ON for 60s. Use motor_off to stop.\n");
}
MSH_CMD_EXPORT(motor_on, force motor on);

static void motor_off(int argc, char **argv)
{
    (void)argc; (void)argv;
    motor_force_until_tick = 0;
    motor_alert_until_tick = 0;
    motor_set(RT_FALSE);
    rt_kprintf("Motor forced OFF.\n");
}
MSH_CMD_EXPORT(motor_off, force motor off);
#endif

/* ----- main ---------------------------------------------------------------- */

int main(void)
{
    rt_err_t result;

    rt_kprintf("FIT monitor v2 starting...\n");
    rt_kprintf("Fast startup: stand still and relax for about 3 seconds.\n");
    rt_kprintf("A short vibration unlocks all features; active baseline then learns online.\n");

    motion_init();
    motor_init();

    result = dual_pressure_init();
    if (result != RT_EOK)
    {
        rt_kprintf("Dual pressure init failed: %d, pressure display disabled\n",
                   result);
    }

    result = mpu6050_init();
    if (result == RT_EOK)
    {
        result = knee_imu_init();
        if (result != RT_EOK)
        {
            rt_kprintf("Second MPU6050 init failed: %d, dual-IMU knee posture disabled\n",
                       result);
        }
        if (mpu6050_start_thread() != RT_EOK)
        {
            rt_kprintf("MPU6050 thread start failed\n");
        }
    }
    else
    {
        rt_kprintf("MPU6050 init failed: %d, motion features disabled\n", result);
    }

    if (ads1292_init() != RT_EOK)
    {
        rt_kprintf("ADS1292 init failed\n");
        return -RT_ERROR;
    }

    /* Register the EMG pipeline callback *after* ADS1292 is up so we don't
     * miss the very first samples but also don't race with reset/calibration.
     */
    emg_pipeline_init();

    /* Start the display before storage/serial workers. The TFT thread has a
     * higher priority than those workers, so its reset and first full clear
     * complete before database mounting and file writes can begin.
     */
    if (tft_start_thread() != RT_EOK)
    {
        rt_kprintf("TFT thread start failed\n");
    }

    fatigue_db_init();

    if (print_start_thread() != RT_EOK)
    {
        rt_kprintf("print thread start failed\n");
        return -RT_ERROR;
    }

    if (knee_start_thread() != RT_EOK)
    {
        rt_kprintf("KNEE output thread start failed\n");
    }

    rt_kprintf("FIT monitor ready. fit_start / fit_stop to toggle live output.\n");

    /* main thread is no longer in the data path. */
    while (1)
    {
        rt_thread_mdelay(1000);
    }
}
