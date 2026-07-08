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
#include "gc9a01.h"

#define DBG_TAG "main"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/* ----- Configurable parameters --------------------------------------------- */

#ifndef PRINT_THREAD_HZ
#define PRINT_THREAD_HZ                 20
#endif

#ifndef LCD_THREAD_PERIOD_MS
#define LCD_THREAD_PERIOD_MS            250
#endif

#ifndef FIT_OUTPUT_DEFAULT_ENABLE
#define FIT_OUTPUT_DEFAULT_ENABLE       1
#endif

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
#define MOTOR_ALERT_PULSE_ON_MS         300
#endif

#ifndef MOTOR_ALERT_PULSE_OFF_MS
#define MOTOR_ALERT_PULSE_OFF_MS        700
#endif

#ifndef MOTOR_ALERT_LATCH_MS
#define MOTOR_ALERT_LATCH_MS            15000
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
#define MPU6050_CALIB_SAMPLES           100
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
#define MOTION_DEADLIFT_ANGLE_X10       150         /* 15.0 deg classify cut  */
#endif
#ifndef MOTION_SQUAT_ANGLE_X10
#define MOTION_SQUAT_ANGLE_X10          300         /* 30.0 deg classify cut  */
#endif

/* Posture quality thresholds (per rep). Calibrated to KneE-PAD distributions:
 * patients descend slowly and turn around at the bottom, so BOTTOM_MIN_MS=0
 * and DESCENT_MAX is generous.
 */
#ifndef QUALITY_DESCENT_MIN_MS
#define QUALITY_DESCENT_MIN_MS          500
#endif
#ifndef QUALITY_DESCENT_MAX_MS
#define QUALITY_DESCENT_MAX_MS          4000
#endif
#ifndef QUALITY_ASCENT_MIN_MS
#define QUALITY_ASCENT_MIN_MS           500
#endif
#ifndef QUALITY_ASCENT_MAX_MS
#define QUALITY_ASCENT_MAX_MS           3000
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
} motion_action_t;

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
    rt_uint32_t     squat_count;
    rt_uint32_t     deadlift_count;

    rt_uint32_t     prev_abs_angle_x10;
    rt_uint32_t     peak_angle_x10;
    rt_tick_t       phase_start_tick;
    rt_tick_t       descend_start_tick;
    rt_tick_t       bottom_start_tick;
    rt_tick_t       ascend_start_tick;
    rt_uint32_t     descend_ms;
    rt_uint32_t     bottom_ms;
    rt_uint32_t     ascend_ms;

    /* "Done" record consumed (and cleared) by the print thread. */
    rt_bool_t           rep_done;
    rt_uint32_t         done_n;
    motion_action_t     done_action;
    rt_uint32_t         done_depth_x10;
    rt_uint32_t         done_descend_ms;
    rt_uint32_t         done_bottom_ms;
    rt_uint32_t         done_ascend_ms;
    emg_rep_features_t  done_emg;
    rt_uint32_t         done_quality_score;
    rt_uint32_t         done_quality_fail_mask;
    rt_uint32_t         done_base_mav;
} motion_context_t;

/* ----- Globals ------------------------------------------------------------- */

static struct rt_i2c_bus_device *mpu6050_i2c_bus = RT_NULL;
static rt_thread_t mpu6050_thread = RT_NULL;
static rt_thread_t print_thread   = RT_NULL;
static rt_thread_t lcd_thread     = RT_NULL;
static mpu6050_state_t  mpu_state;
static motion_context_t motion_ctx;
static volatile rt_bool_t fit_output_enable = FIT_OUTPUT_DEFAULT_ENABLE ? RT_TRUE : RT_FALSE;
static rt_bool_t motor_output_on = RT_FALSE;
static volatile rt_tick_t motor_force_until_tick = 0;
static volatile rt_tick_t motor_alert_until_tick = 0;

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
    case ACTION_UNKNOWN:
    default:              return "UNKNOWN";
    }
}

static motion_action_t motion_classify_depth(rt_uint32_t depth_x10)
{
    if (depth_x10 >= MOTION_SQUAT_ANGLE_X10)    return ACTION_SQUAT;
    if (depth_x10 >= MOTION_DEADLIFT_ANGLE_X10) return ACTION_DEADLIFT;
    return ACTION_UNKNOWN;
}

static void motion_finalize_rep(void);

static void motion_reset_rep(void)
{
    motion_ctx.peak_angle_x10 = 0;
    motion_ctx.descend_start_tick = 0;
    motion_ctx.bottom_start_tick  = 0;
    motion_ctx.ascend_start_tick  = 0;
    motion_ctx.descend_ms = 0;
    motion_ctx.bottom_ms  = 0;
    motion_ctx.ascend_ms  = 0;
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
    rt_kprintf("Motor alert ready: pin=PE0, active=%s\n",
               MOTOR_ACTIVE_LEVEL == PIN_HIGH ? "HIGH" : "LOW");
#endif
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

/* Compute a 0-7 quality score and a failure bitmask for the just-completed rep. */
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

    /* Descent timing. */
    if (m->done_descend_ms >= QUALITY_DESCENT_MIN_MS &&
        m->done_descend_ms <= QUALITY_DESCENT_MAX_MS) score++;
    else if (m->done_descend_ms < QUALITY_DESCENT_MIN_MS) mask |= QUALITY_FAIL_BOUNCE;
    else                                                  mask |= QUALITY_FAIL_SLOW;

    /* Ascent timing. */
    if (m->done_ascend_ms >= QUALITY_ASCENT_MIN_MS &&
        m->done_ascend_ms <= QUALITY_ASCENT_MAX_MS) score++;
    else if (m->done_ascend_ms < QUALITY_ASCENT_MIN_MS) mask |= QUALITY_FAIL_BOUNCE;
    else                                                mask |= QUALITY_FAIL_SLOW;

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
        score++;

    if (fail_mask_out) *fail_mask_out = mask;
    return score;
}

static const char *motion_quality_label(rt_uint32_t score)
{
    if (score == 7)      return "GOOD";
    if (score >= 5)      return "OK";
    if (score >= 3)      return "WEAK";
    return "WRONG";
}

static void motion_finalize_rep(void)
{
    emg_rep_features_t emg;
    rt_memset(&emg, 0, sizeof(emg));
    emg_pipeline_rep_stop(&emg);

    /* Reject reps that never reached the start threshold (false trigger). */
    if (motion_ctx.peak_angle_x10 < MOTION_START_ANGLE_X10)
    {
        motion_reset_rep();
        return;
    }

    motion_ctx.rep_count++;
    motion_ctx.done_n            = motion_ctx.rep_count;
    motion_ctx.done_action       = motion_classify_depth(motion_ctx.peak_angle_x10);
    if (motion_ctx.done_action == ACTION_SQUAT)
    {
        motion_ctx.squat_count++;
    }
    else if (motion_ctx.done_action == ACTION_DEADLIFT)
    {
        motion_ctx.deadlift_count++;
    }
    motion_ctx.done_depth_x10    = motion_ctx.peak_angle_x10;
    motion_ctx.done_descend_ms   = motion_ctx.descend_ms;
    motion_ctx.done_bottom_ms    = motion_ctx.bottom_ms;
    motion_ctx.done_ascend_ms    = motion_ctx.ascend_ms;
    motion_ctx.done_emg          = emg;
    motion_ctx.action            = motion_ctx.done_action;

    emg_snapshot_t snap;
    emg_pipeline_get_snapshot(&snap);
    motion_ctx.done_base_mav = snap.base_mav;

    rt_uint32_t fail_mask = 0;
    motion_ctx.done_quality_score = motion_score_quality(
        &motion_ctx, &emg, snap.base_mav, &fail_mask);
    motion_ctx.done_quality_fail_mask = fail_mask;

    motion_ctx.rep_done = RT_TRUE;
    motion_reset_rep();
}

static void motion_init(void)
{
    rt_memset(&motion_ctx, 0, sizeof(motion_ctx));
    motion_ctx.phase  = MOTION_PHASE_CALIBRATING;
    motion_ctx.action = ACTION_UNKNOWN;
}

static void motion_enter_descend(rt_uint32_t abs_angle)
{
    motion_reset_rep();
    motion_ctx.phase = MOTION_PHASE_DESCEND;
    motion_ctx.descend_start_tick = rt_tick_get();
    motion_ctx.peak_angle_x10 = abs_angle;
    emg_pipeline_rep_start();
}

static void motion_enter_bottom(rt_uint32_t abs_angle)
{
    motion_ctx.bottom_start_tick = rt_tick_get();
    motion_ctx.descend_ms = ticks_to_ms(motion_ctx.bottom_start_tick - motion_ctx.descend_start_tick);
    if (abs_angle > motion_ctx.peak_angle_x10) motion_ctx.peak_angle_x10 = abs_angle;
    motion_ctx.phase = MOTION_PHASE_BOTTOM;
}

static void motion_enter_ascend(rt_uint32_t abs_angle)
{
    motion_ctx.ascend_start_tick = rt_tick_get();
    if (motion_ctx.bottom_start_tick != 0)
    {
        motion_ctx.bottom_ms = ticks_to_ms(motion_ctx.ascend_start_tick - motion_ctx.bottom_start_tick);
    }
    if (abs_angle > motion_ctx.peak_angle_x10) motion_ctx.peak_angle_x10 = abs_angle;
    motion_ctx.phase = MOTION_PHASE_ASCEND;
}

static void motion_complete_rep(void)
{
    rt_tick_t now = rt_tick_get();
    motion_ctx.ascend_ms = ticks_to_ms(now - motion_ctx.ascend_start_tick);
    motion_finalize_rep();
    motion_ctx.phase = MOTION_PHASE_STAND;
}

static void motion_update_by_mpu(const mpu6050_state_t *mpu)
{
    rt_uint32_t abs_angle = mpu->abs_angle_x10;
    rt_int32_t delta = (rt_int32_t)abs_angle - (rt_int32_t)motion_ctx.prev_abs_angle_x10;

    if (!mpu->calibrated)
    {
        motion_ctx.phase = MOTION_PHASE_CALIBRATING;
        motion_ctx.prev_abs_angle_x10 = abs_angle;
        return;
    }

    if (motion_ctx.phase == MOTION_PHASE_CALIBRATING)
    {
        motion_ctx.phase = MOTION_PHASE_STAND;
        motion_ctx.prev_abs_angle_x10 = abs_angle;
        return;
    }

    switch (motion_ctx.phase)
    {
    case MOTION_PHASE_STAND:
        if (abs_angle > MOTION_START_ANGLE_X10 && delta > 0)
        {
            motion_enter_descend(abs_angle);
        }
        break;

    case MOTION_PHASE_DESCEND:
        if (abs_angle > motion_ctx.peak_angle_x10)
        {
            motion_ctx.peak_angle_x10 = abs_angle;
        }
        if (abs_angle >= MOTION_BOTTOM_ANGLE_X10)
        {
            motion_enter_bottom(abs_angle);
        }
        else if (delta < -20 && abs_angle > MOTION_START_ANGLE_X10)
        {
            /* Reverse direction before bottom reached -- partial rep. */
            motion_enter_ascend(abs_angle);
        }
        break;

    case MOTION_PHASE_BOTTOM:
        if (abs_angle > motion_ctx.peak_angle_x10)
        {
            motion_ctx.peak_angle_x10 = abs_angle;
        }
        if (delta < -20)
        {
            motion_enter_ascend(abs_angle);
        }
        break;

    case MOTION_PHASE_ASCEND:
        if (abs_angle < MOTION_TOP_ANGLE_X10)
        {
            motion_complete_rep();
        }
        break;

    case MOTION_PHASE_CALIBRATING:
    default:
        motion_ctx.phase = MOTION_PHASE_STAND;
        break;
    }

    motion_ctx.prev_abs_angle_x10 = abs_angle;
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
        motion_update_by_mpu(&mpu_state);
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
    motion_update_by_mpu(&mpu_state);
    rt_hw_interrupt_enable(level);
}

static void mpu6050_thread_entry(void *p)
{
    mpu6050_raw_t raw;
    RT_UNUSED(p);
    while (1)
    {
        if (mpu6050_read_raw(&raw) == RT_EOK)
        {
            mpu6050_update_state(&raw);
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

static void emit_rep_line(const motion_context_t *m)
{
    rt_uint64_t mean_mav = m->done_emg.sample_count > 0 ?
        (m->done_emg.iemg / m->done_emg.sample_count) : 0;
    rt_uint64_t mean_rms = m->done_emg.window_count > 0 ?
        (m->done_emg.sum_window_rms / m->done_emg.window_count) : 0;
    rt_uint64_t mean_wl  = m->done_emg.window_count > 0 ?
        (m->done_emg.sum_window_wl / m->done_emg.window_count) : 0;

    rt_kprintf("REP,%lu,%s,depth_x10=%lu,t_dn=%lu,t_bot=%lu,t_up=%lu,"
               "iemg=%lu,mean_mav=%lu,mean_rms=%lu,mean_wl=%lu,peak_rms=%lu,"
               "fatigue=%lu,quality=%lu,label=%s,fail=0x%02x,base_mav=%lu\n",
               (unsigned long)m->done_n,
               action_text(m->done_action),
               (unsigned long)m->done_depth_x10,
               (unsigned long)m->done_descend_ms,
               (unsigned long)m->done_bottom_ms,
               (unsigned long)m->done_ascend_ms,
               (unsigned long)m->done_emg.iemg,
               (unsigned long)mean_mav,
               (unsigned long)mean_rms,
               (unsigned long)mean_wl,
               (unsigned long)m->done_emg.peak_rms,
               (unsigned long)m->done_emg.peak_fatigue,
               (unsigned long)m->done_quality_score,
               motion_quality_label(m->done_quality_score),
               (unsigned)m->done_quality_fail_mask,
               (unsigned long)m->done_base_mav);
}

static void emit_live_line(const emg_snapshot_t *e, const motion_context_t *m,
                           const mpu6050_state_t *mpu, rt_tick_t t0)
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
               (unsigned)e->fatigue_score,
               (unsigned)e->fatigue_alert,
               emg_phase_text(e->phase),
               emg_status_text(e->status),
               motion_phase_text(m->phase),
               (long)mpu->angle_rel_x10,
               (unsigned)e->active,
               mpu->raw.ax, mpu->raw.ay, mpu->raw.az,
               mpu->raw.gx, mpu->raw.gy, mpu->raw.gz);
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
    else if ((prev == EMG_PHASE_ACTIVE_CAL || prev == EMG_PHASE_REST_CAL) &&
             e->phase == EMG_PHASE_RUNNING)
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
    rt_kprintf("# REP,n,action,depth_x10,t_dn,t_bot,t_up,iemg,mean_mav,mean_rms,mean_wl,peak_rms,fatigue,quality,label,fail,base_mav\n");

    while (1)
    {
        emg_snapshot_t snap;
        motion_context_t m;
        mpu6050_state_t mpu;

        emg_pipeline_get_snapshot(&snap);
        motion_get_snapshot(&m, &mpu);
        motor_update_alert(snap.fatigue_alert || snap.status == EMG_STATUS_ALERT);

        if (first_iter)
        {
            last_phase = snap.phase;
            first_iter = RT_FALSE;
        }
        else if (snap.phase != last_phase)
        {
            emit_cal_line_on_transition(&snap, last_phase);
            last_phase = snap.phase;
        }

        if (m.rep_done)
        {
            emit_rep_line(&m);
            motion_clear_rep_done();
        }

        if (fit_output_enable)
        {
            emit_live_line(&snap, &m, &mpu, t0);
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

/* ----- LCD thread ---------------------------------------------------------- */

static void lcd_thread_entry(void *p)
{
    RT_UNUSED(p);

    if (gc9a01_init() != RT_EOK)
    {
        rt_kprintf("GC9A01 LCD init failed, display disabled\n");
        return;
    }

    rt_kprintf("GC9A01 LCD ready on %s\n", GC9A01_SPI_BUS_NAME);

    while (1)
    {
        emg_snapshot_t snap;
        motion_context_t m;

        emg_pipeline_get_snapshot(&snap);
        motion_get_snapshot(&m, RT_NULL);

        gc9a01_show_fit(m.rep_count,
                        m.squat_count,
                        m.deadlift_count,
                        snap.fatigue_score,
                        snap.fatigue_alert,
                        action_text(m.action),
                        emg_status_text(snap.status));

        rt_thread_mdelay(LCD_THREAD_PERIOD_MS);
    }
}

static rt_err_t lcd_start_thread(void)
{
    if (lcd_thread != RT_NULL) return RT_EOK;
    lcd_thread = rt_thread_create("fitlcd", lcd_thread_entry, RT_NULL, 2048, 26, 10);
    if (lcd_thread == RT_NULL) return -RT_ENOMEM;
    rt_thread_startup(lcd_thread);
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
    rt_kprintf("Keep the MPU6050 still and target muscle relaxed during calibration.\n");

    motion_init();
    motor_init();

    result = mpu6050_init();
    if (result == RT_EOK)
    {
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

    if (print_start_thread() != RT_EOK)
    {
        rt_kprintf("print thread start failed\n");
        return -RT_ERROR;
    }

    if (lcd_start_thread() != RT_EOK)
    {
        rt_kprintf("LCD thread start failed\n");
    }

    rt_kprintf("FIT monitor ready. fit_start / fit_stop to toggle live output.\n");

    /* main thread is no longer in the data path. */
    while (1)
    {
        rt_thread_mdelay(1000);
    }
}
