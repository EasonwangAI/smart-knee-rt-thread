/*
 * Relative thigh/shank IMU estimator for knee valgus/outward motion.
 *
 * Mounting contract:
 *   - thigh MPU6050: existing lateral-thigh position, address 0x68;
 *   - shank MPU6050: proximal anterolateral tibia, PB8/PB9 software I2C2,
 *                    AD0 low, address 0x68;
 *   - both boards use the same face toward the outside of the leg;
 *   - each board's long direction points distally (hip->knee, knee->ankle).
 *
 * The longitudinal accelerometer axis is detected during standing
 * calibration.  With the common MPU6050 breakout orientation, Z is normal to
 * the PCB face, so it is the default anatomical outward axis.  These choices
 * remain compile-time settings to keep RAM/flash usage small.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#ifdef FINSH_USING_MSH
#include <finsh.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "knee_imu.h"

#ifndef KNEE_IMU_I2C_BUS_NAME
#define KNEE_IMU_I2C_BUS_NAME              "i2c2"
#endif
#ifndef KNEE_IMU_SHANK_I2C_ADDR
#define KNEE_IMU_SHANK_I2C_ADDR            0x68U
#endif
#ifndef KNEE_IMU_SAMPLE_PERIOD_MS
#define KNEE_IMU_SAMPLE_PERIOD_MS           20U
#endif
#ifndef KNEE_IMU_CALIBRATION_SAMPLES
#define KNEE_IMU_CALIBRATION_SAMPLES       100U /* 2 s standing still */
#endif

/* 0=X, 1=Y, 2=Z.  Z is normal to most MPU6050 breakout boards. */
#ifndef KNEE_IMU_THIGH_OUTWARD_AXIS
#define KNEE_IMU_THIGH_OUTWARD_AXIS          2U
#endif
#ifndef KNEE_IMU_SHANK_OUTWARD_AXIS
#define KNEE_IMU_SHANK_OUTWARD_AXIS          2U
#endif
/* Use -1 when that sensor's opposite PCB face points outward. */
#ifndef KNEE_IMU_THIGH_OUTWARD_SIGN
#define KNEE_IMU_THIGH_OUTWARD_SIGN           1
#endif
#ifndef KNEE_IMU_SHANK_OUTWARD_SIGN
#define KNEE_IMU_SHANK_OUTWARD_SIGN           1
#endif

/* Positive relative angle is defined as valgus.  Run knee_imu_flip once if a
 * deliberate inward test produces a negative knee_angle_x10. */
#ifndef KNEE_IMU_DEFAULT_DIRECTION_SIGN
#define KNEE_IMU_DEFAULT_DIRECTION_SIGN      -1
#endif

/* Valgus remains sensitive; normal squat knee-out is deliberately wide. */
#ifndef KNEE_IMU_VALGUS_THRESHOLD_X10
#define KNEE_IMU_VALGUS_THRESHOLD_X10        30U /* +3.0 deg: inward/valgus */
#endif
#ifndef KNEE_IMU_OUTWARD_THRESHOLD_X10
#define KNEE_IMU_OUTWARD_THRESHOLD_X10      400U /* -40.0 deg: outward/app "outward" */
#endif
#ifndef KNEE_IMU_RELEASE_THRESHOLD_X10
#define KNEE_IMU_RELEASE_THRESHOLD_X10       30U /* 3.0 deg */
#endif
#ifndef KNEE_IMU_CONFIRM_MS
#define KNEE_IMU_CONFIRM_MS                  130U
#endif
#ifndef KNEE_IMU_BASELINE_TRACK_DIVISOR
#define KNEE_IMU_BASELINE_TRACK_DIVISOR     128L
#endif
#ifndef KNEE_IMU_BASELINE_TRACK_BAND_X10
#define KNEE_IMU_BASELINE_TRACK_BAND_X10     40L /* 4.0 deg */
#endif
#ifndef KNEE_IMU_RESULT_HOLD_MS
#define KNEE_IMU_RESULT_HOLD_MS             1000U
#endif
#ifndef KNEE_IMU_STREAM_PERIOD_MS
#define KNEE_IMU_STREAM_PERIOD_MS            500U
#endif

#define KNEE_IMU_REG_SMPLRT_DIV             0x19U
#define KNEE_IMU_REG_CONFIG                 0x1AU
#define KNEE_IMU_REG_GYRO_CONFIG            0x1BU
#define KNEE_IMU_REG_ACCEL_CONFIG           0x1CU
#define KNEE_IMU_REG_ACCEL_XOUT_H           0x3BU
#define KNEE_IMU_REG_PWR_MGMT_1             0x6BU
#define KNEE_IMU_REG_WHO_AM_I               0x75U

#define KNEE_IMU_ACCEL_TRUST_MIN_SQ  (12288LL * 12288LL) /* 0.75 g */
#define KNEE_IMU_ACCEL_TRUST_MAX_SQ  (20480LL * 20480LL) /* 1.25 g */

typedef struct
{
    knee_imu_sample_t raw;
    rt_int32_t ax_sum;
    rt_int32_t ay_sum;
    rt_int32_t az_sum;
    rt_int32_t gx_sum;
    rt_int32_t gy_sum;
    rt_int32_t gz_sum;
    rt_int32_t gx_bias;
    rt_int32_t gy_bias;
    rt_int32_t gz_bias;
    rt_int32_t base_angle_x10;
    rt_int32_t angle_x10;
    rt_uint8_t long_axis;
    rt_uint8_t gyro_axis;
    rt_uint8_t outward_axis;
    rt_int8_t long_sign;
    rt_int8_t outward_sign;
    rt_int8_t gyro_sign;
} knee_imu_segment_t;

static struct rt_i2c_bus_device *knee_imu_bus = RT_NULL;
static knee_imu_segment_t thigh_state;
static knee_imu_segment_t shank_state;
static knee_imu_snapshot_t knee_snapshot;
static rt_uint16_t calibration_count = 0U;
static rt_uint8_t read_fail_count = 0U;
static rt_int32_t neutral_offset_x10 = 0;
static rt_int32_t neutral_track_accumulator = 0;
static rt_int8_t direction_sign = KNEE_IMU_DEFAULT_DIRECTION_SIGN;
static rt_bool_t initialized = RT_FALSE;
static rt_bool_t recalibrate_requested = RT_FALSE;
static rt_bool_t stream_enabled = RT_TRUE;
static knee_imu_status_t candidate_status = KNEE_IMU_NEUTRAL;
static knee_imu_status_t latched_status = KNEE_IMU_NEUTRAL;
static rt_tick_t candidate_start_tick = 0;
static rt_bool_t motion_was_active = RT_FALSE;
static rt_tick_t result_hold_start_tick = 0;
static rt_tick_t last_stream_tick = 0;

static rt_uint32_t ticks_to_ms(rt_tick_t ticks)
{
    return (rt_uint32_t)(((rt_uint64_t)ticks * 1000ULL) /
                         (rt_uint64_t)RT_TICK_PER_SECOND);
}

static rt_int16_t make_i16(rt_uint8_t msb, rt_uint8_t lsb)
{
    return (rt_int16_t)(((rt_uint16_t)msb << 8) | lsb);
}

static rt_int32_t axis_value(const knee_imu_sample_t *s, rt_uint8_t axis,
                             rt_bool_t gyro)
{
    if (gyro)
    {
        if (axis == 1U) return s->gy;
        if (axis == 2U) return s->gz;
        return s->gx;
    }
    if (axis == 1U) return s->ay;
    if (axis == 2U) return s->az;
    return s->ax;
}

static rt_int32_t wrap_angle_x10(rt_int32_t angle)
{
    while (angle > 1800) angle -= 3600;
    while (angle < -1800) angle += 3600;
    return angle;
}

/* Levi-Civita sign for three distinct X/Y/Z axis indices. */
static rt_int8_t permutation_sign(rt_uint8_t a, rt_uint8_t b, rt_uint8_t c)
{
    if ((a == 0U && b == 1U && c == 2U) ||
        (a == 1U && b == 2U && c == 0U) ||
        (a == 2U && b == 0U && c == 1U)) return 1;
    return -1;
}

static rt_uint8_t remaining_axis(rt_uint8_t a, rt_uint8_t b)
{
    return (rt_uint8_t)(3U - a - b);
}

static rt_err_t write_reg(rt_uint8_t address, rt_uint8_t reg, rt_uint8_t value)
{
    rt_uint8_t data[2];
    struct rt_i2c_msg msg;

    if (knee_imu_bus == RT_NULL) return -RT_ENOSYS;
    data[0] = reg;
    data[1] = value;
    msg.addr = address;
    msg.flags = RT_I2C_WR;
    msg.buf = data;
    msg.len = sizeof(data);
    return rt_i2c_transfer(knee_imu_bus, &msg, 1) == 1 ? RT_EOK : -RT_ERROR;
}

static rt_err_t read_regs(rt_uint8_t address, rt_uint8_t reg,
                          rt_uint8_t *data, rt_size_t size)
{
    struct rt_i2c_msg msgs[2];

    if (knee_imu_bus == RT_NULL || data == RT_NULL || size == 0U)
        return -RT_EINVAL;
    msgs[0].addr = address;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf = &reg;
    msgs[0].len = 1U;
    msgs[1].addr = address;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf = data;
    msgs[1].len = size;
    return rt_i2c_transfer(knee_imu_bus, msgs, 2) == 2 ? RT_EOK : -RT_ERROR;
}

static rt_err_t read_shank(knee_imu_sample_t *sample)
{
    rt_uint8_t data[14];
    rt_err_t result;

    if (sample == RT_NULL) return -RT_EINVAL;
    result = read_regs(KNEE_IMU_SHANK_I2C_ADDR,
                       KNEE_IMU_REG_ACCEL_XOUT_H, data, sizeof(data));
    if (result != RT_EOK) return result;
    sample->ax = make_i16(data[0], data[1]);
    sample->ay = make_i16(data[2], data[3]);
    sample->az = make_i16(data[4], data[5]);
    sample->gx = make_i16(data[8], data[9]);
    sample->gy = make_i16(data[10], data[11]);
    sample->gz = make_i16(data[12], data[13]);
    return RT_EOK;
}

static rt_uint8_t detect_long_axis(const knee_imu_segment_t *segment)
{
    rt_uint8_t best = 0U;
    rt_int32_t best_abs = -1;
    rt_uint8_t axis;

    for (axis = 0U; axis < 3U; axis++)
    {
        rt_int32_t value;
        rt_int32_t magnitude;
        if (axis == segment->outward_axis) continue;
        if (axis == 1U) value = segment->ay_sum;
        else if (axis == 2U) value = segment->az_sum;
        else value = segment->ax_sum;
        magnitude = value < 0 ? -value : value;
        if (magnitude > best_abs)
        {
            best_abs = magnitude;
            best = axis;
        }
    }
    return best;
}

static rt_int32_t segment_acc_angle_x10(const knee_imu_segment_t *segment,
                                        const knee_imu_sample_t *sample)
{
    float outward = (float)axis_value(sample, segment->outward_axis, RT_FALSE) *
                    (float)segment->outward_sign;
    float longitudinal = (float)axis_value(sample, segment->long_axis, RT_FALSE) *
                         (float)segment->long_sign;
    return (rt_int32_t)(atan2f(outward, longitudinal) *
                        (1800.0f / 3.141592653589793f));
}

static rt_bool_t acceleration_is_trusted(const knee_imu_sample_t *sample)
{
    rt_int64_t magnitude_sq = (rt_int64_t)sample->ax * sample->ax +
                              (rt_int64_t)sample->ay * sample->ay +
                              (rt_int64_t)sample->az * sample->az;
    return magnitude_sq >= KNEE_IMU_ACCEL_TRUST_MIN_SQ &&
           magnitude_sq <= KNEE_IMU_ACCEL_TRUST_MAX_SQ;
}

static void accumulate_calibration(knee_imu_segment_t *segment,
                                   const knee_imu_sample_t *sample)
{
    segment->raw = *sample;
    segment->ax_sum += sample->ax;
    segment->ay_sum += sample->ay;
    segment->az_sum += sample->az;
    segment->gx_sum += sample->gx;
    segment->gy_sum += sample->gy;
    segment->gz_sum += sample->gz;
}

static void finish_segment_calibration(knee_imu_segment_t *segment)
{
    knee_imu_sample_t average;
    rt_int32_t long_average;

    average.ax = (rt_int16_t)(segment->ax_sum / (rt_int32_t)calibration_count);
    average.ay = (rt_int16_t)(segment->ay_sum / (rt_int32_t)calibration_count);
    average.az = (rt_int16_t)(segment->az_sum / (rt_int32_t)calibration_count);
    average.gx = (rt_int16_t)(segment->gx_sum / (rt_int32_t)calibration_count);
    average.gy = (rt_int16_t)(segment->gy_sum / (rt_int32_t)calibration_count);
    average.gz = (rt_int16_t)(segment->gz_sum / (rt_int32_t)calibration_count);

    segment->gx_bias = average.gx;
    segment->gy_bias = average.gy;
    segment->gz_bias = average.gz;
    segment->long_axis = detect_long_axis(segment);
    segment->gyro_axis = remaining_axis(segment->outward_axis,
                                        segment->long_axis);
    long_average = axis_value(&average, segment->long_axis, RT_FALSE);
    segment->long_sign = long_average < 0 ? -1 : 1;

    /* For body-frame gravity, d(acc_out)/dt = -gyro x gravity. */
    segment->gyro_sign = (rt_int8_t)(
        -segment->outward_sign * segment->long_sign *
        permutation_sign(segment->outward_axis, segment->gyro_axis,
                         segment->long_axis));
    segment->base_angle_x10 = segment_acc_angle_x10(segment, &average);
    segment->angle_x10 = segment->base_angle_x10;
}

static void update_segment(knee_imu_segment_t *segment,
                           const knee_imu_sample_t *sample)
{
    rt_int32_t gyro_raw;
    rt_int32_t gyro_bias;
    rt_int32_t gyro_rate_x10;
    rt_int32_t predicted_x10;

    segment->raw = *sample;
    gyro_raw = axis_value(sample, segment->gyro_axis, RT_TRUE);
    if (segment->gyro_axis == 1U) gyro_bias = segment->gy_bias;
    else if (segment->gyro_axis == 2U) gyro_bias = segment->gz_bias;
    else gyro_bias = segment->gx_bias;
    gyro_rate_x10 = (gyro_raw - gyro_bias) * segment->gyro_sign * 10 / 131;
    predicted_x10 = wrap_angle_x10(segment->angle_x10 +
        gyro_rate_x10 * (rt_int32_t)KNEE_IMU_SAMPLE_PERIOD_MS / 1000);

    if (acceleration_is_trusted(sample))
    {
        rt_int32_t acc_angle_x10 = segment_acc_angle_x10(segment, sample);
        rt_int32_t correction = wrap_angle_x10(acc_angle_x10 - predicted_x10);
        segment->angle_x10 = wrap_angle_x10(predicted_x10 + correction * 2 / 100);
    }
    else
    {
        segment->angle_x10 = predicted_x10;
    }
}

const char *knee_imu_status_short(knee_imu_status_t status)
{
    switch (status)
    {
    case KNEE_IMU_CALIBRATING: return "CAL";
    case KNEE_IMU_NEUTRAL:     return "NEU";
    case KNEE_IMU_VALGUS:      return "VAL";
    case KNEE_IMU_OUTWARD:     return "OUT";
    case KNEE_IMU_ERROR:       return "ERR";
    case KNEE_IMU_OFFLINE:
    default:                   return "OFF";
    }
}

void knee_imu_get_snapshot(knee_imu_snapshot_t *snapshot)
{
    rt_base_t level;
    if (snapshot == RT_NULL) return;
    level = rt_hw_interrupt_disable();
    *snapshot = knee_snapshot;
    rt_hw_interrupt_enable(level);
}

static void publish_snapshot(const knee_imu_snapshot_t *snapshot)
{
    rt_base_t level = rt_hw_interrupt_disable();
    knee_snapshot = *snapshot;
    rt_hw_interrupt_enable(level);
}

static void emit_stream(const knee_imu_snapshot_t *snapshot)
{
    rt_tick_t now;
    if (!stream_enabled || snapshot == RT_NULL) return;
    now = rt_tick_get();
    if (last_stream_tick != 0 &&
        ticks_to_ms(now - last_stream_tick) < KNEE_IMU_STREAM_PERIOD_MS) return;
    last_stream_tick = now;
    rt_kprintf("KIMU,angle_x10=%ld,thigh_x10=%ld,shank_x10=%ld,status=%s,"
               "motion=%u,peak_in_x10=%lu,peak_out_x10=%lu,sign=%d\n",
               (long)snapshot->knee_angle_x10,
               (long)snapshot->thigh_angle_x10,
               (long)snapshot->shank_angle_x10,
               knee_imu_status_short(snapshot->status),
               (unsigned)snapshot->motion_active,
               (unsigned long)snapshot->peak_valgus_x10,
               (unsigned long)snapshot->peak_outward_x10,
               (int)snapshot->direction_sign);
}

static void reset_calibration(void)
{
    rt_uint8_t thigh_out_axis = KNEE_IMU_THIGH_OUTWARD_AXIS;
    rt_uint8_t shank_out_axis = KNEE_IMU_SHANK_OUTWARD_AXIS;
    rt_int8_t thigh_out_sign = KNEE_IMU_THIGH_OUTWARD_SIGN;
    rt_int8_t shank_out_sign = KNEE_IMU_SHANK_OUTWARD_SIGN;

    rt_memset(&thigh_state, 0, sizeof(thigh_state));
    rt_memset(&shank_state, 0, sizeof(shank_state));
    thigh_state.outward_axis = thigh_out_axis;
    shank_state.outward_axis = shank_out_axis;
    thigh_state.outward_sign = thigh_out_sign;
    shank_state.outward_sign = shank_out_sign;
    calibration_count = 0U;
    neutral_offset_x10 = 0;
    neutral_track_accumulator = 0;
    candidate_status = KNEE_IMU_NEUTRAL;
    latched_status = KNEE_IMU_NEUTRAL;
    candidate_start_tick = 0;
    motion_was_active = RT_FALSE;
    result_hold_start_tick = 0;
    knee_snapshot.calibrated = RT_FALSE;
    knee_snapshot.status = KNEE_IMU_CALIBRATING;
    knee_snapshot.knee_angle_x10 = 0;
    knee_snapshot.peak_valgus_x10 = 0U;
    knee_snapshot.peak_outward_x10 = 0U;
}

rt_err_t knee_imu_init(void)
{
    rt_uint8_t who = 0U;
    rt_err_t result;

    knee_imu_bus = rt_i2c_bus_device_find(KNEE_IMU_I2C_BUS_NAME);
    if (knee_imu_bus == RT_NULL)
    {
        rt_kprintf("KIMU,ERROR,i2c_bus_not_found,bus=%s\n",
                   KNEE_IMU_I2C_BUS_NAME);
        return -RT_ENOSYS;
    }
    result = read_regs(KNEE_IMU_SHANK_I2C_ADDR, KNEE_IMU_REG_WHO_AM_I,
                       &who, 1U);
    if (result != RT_EOK)
    {
        rt_kprintf("KIMU,ERROR,shank_not_found,addr=0x%02x,result=%d\n",
                   KNEE_IMU_SHANK_I2C_ADDR, result);
        return result;
    }
    if (who != 0x68U)
    {
        rt_kprintf("KIMU,ERROR,bad_whoami,value=0x%02x\n", who);
        return -RT_ERROR;
    }

    write_reg(KNEE_IMU_SHANK_I2C_ADDR, KNEE_IMU_REG_PWR_MGMT_1, 0x80U);
    rt_thread_mdelay(100);
    write_reg(KNEE_IMU_SHANK_I2C_ADDR, KNEE_IMU_REG_PWR_MGMT_1, 0x00U);
    rt_thread_mdelay(30);
    write_reg(KNEE_IMU_SHANK_I2C_ADDR, KNEE_IMU_REG_SMPLRT_DIV, 0x09U);
    write_reg(KNEE_IMU_SHANK_I2C_ADDR, KNEE_IMU_REG_CONFIG, 0x03U);
    write_reg(KNEE_IMU_SHANK_I2C_ADDR, KNEE_IMU_REG_GYRO_CONFIG, 0x00U);
    write_reg(KNEE_IMU_SHANK_I2C_ADDR, KNEE_IMU_REG_ACCEL_CONFIG, 0x00U);

    rt_memset(&knee_snapshot, 0, sizeof(knee_snapshot));
    knee_snapshot.online = RT_TRUE;
    knee_snapshot.shank_i2c_address = KNEE_IMU_SHANK_I2C_ADDR;
    knee_snapshot.direction_sign = direction_sign;
    knee_snapshot.valgus_threshold_x10 = KNEE_IMU_VALGUS_THRESHOLD_X10;
    knee_snapshot.outward_threshold_x10 = KNEE_IMU_OUTWARD_THRESHOLD_X10;
    reset_calibration();
    initialized = RT_TRUE;
    rt_kprintf("KIMU,READY,bus=%s,thigh=0x68,shank=0x%02x,period=%ums,"
               "stand_still_ms=%u\n",
               KNEE_IMU_I2C_BUS_NAME, KNEE_IMU_SHANK_I2C_ADDR,
               (unsigned)KNEE_IMU_SAMPLE_PERIOD_MS,
               (unsigned)(KNEE_IMU_CALIBRATION_SAMPLES *
                          KNEE_IMU_SAMPLE_PERIOD_MS));
    return RT_EOK;
}

static knee_imu_status_t desired_status(rt_int32_t angle_x10,
                                        knee_imu_status_t current)
{
    if (angle_x10 >= (rt_int32_t)KNEE_IMU_VALGUS_THRESHOLD_X10)
        return KNEE_IMU_VALGUS;
    if (angle_x10 <= -(rt_int32_t)KNEE_IMU_OUTWARD_THRESHOLD_X10)
        return KNEE_IMU_OUTWARD;
    if (current == KNEE_IMU_VALGUS &&
        angle_x10 >= (rt_int32_t)KNEE_IMU_RELEASE_THRESHOLD_X10)
        return current;
    if (current == KNEE_IMU_OUTWARD &&
        angle_x10 <= -(rt_int32_t)KNEE_IMU_RELEASE_THRESHOLD_X10)
        return current;
    return KNEE_IMU_NEUTRAL;
}

void knee_imu_process(const knee_imu_sample_t *thigh, rt_bool_t motion_active)
{
    knee_imu_sample_t shank;
    knee_imu_snapshot_t local;
    rt_int32_t raw_relative_x10;
    knee_imu_status_t desired;
    rt_tick_t now;

    if (!initialized || thigh == RT_NULL) return;
    if (read_shank(&shank) != RT_EOK)
    {
        if (read_fail_count < 255U) read_fail_count++;
        if (read_fail_count >= 10U)
        {
            knee_imu_get_snapshot(&local);
            local.online = RT_FALSE;
            local.status = KNEE_IMU_ERROR;
            publish_snapshot(&local);
        }
        return;
    }
    read_fail_count = 0U;
    knee_imu_get_snapshot(&local);
    local.online = RT_TRUE;
    local.motion_active = motion_active ? RT_TRUE : RT_FALSE;
    local.direction_sign = direction_sign;

    if (recalibrate_requested)
    {
        recalibrate_requested = RT_FALSE;
        reset_calibration();
        knee_imu_get_snapshot(&local);
        local.online = RT_TRUE;
        local.direction_sign = direction_sign;
        rt_kprintf("KIMU,CAL_BEGIN,stand_still=1,samples=%u\n",
                   (unsigned)KNEE_IMU_CALIBRATION_SAMPLES);
    }

    if (!local.calibrated)
    {
        accumulate_calibration(&thigh_state, thigh);
        accumulate_calibration(&shank_state, &shank);
        calibration_count++;
        local.status = KNEE_IMU_CALIBRATING;
        if (calibration_count >= KNEE_IMU_CALIBRATION_SAMPLES)
        {
            finish_segment_calibration(&thigh_state);
            finish_segment_calibration(&shank_state);
            local.calibrated = RT_TRUE;
            local.status = KNEE_IMU_NEUTRAL;
            local.thigh_long_axis = thigh_state.long_axis;
            local.shank_long_axis = shank_state.long_axis;
            local.thigh_angle_x10 = 0;
            local.shank_angle_x10 = 0;
            local.knee_angle_x10 = 0;
            rt_kprintf("KIMU,CAL_DONE,thigh_long=%u,thigh_gyro=%u,"
                       "shank_long=%u,shank_gyro=%u,sign=%d\n",
                       (unsigned)thigh_state.long_axis,
                       (unsigned)thigh_state.gyro_axis,
                       (unsigned)shank_state.long_axis,
                       (unsigned)shank_state.gyro_axis,
                       (int)direction_sign);
        }
        publish_snapshot(&local);
        emit_stream(&local);
        return;
    }

    update_segment(&thigh_state, thigh);
    update_segment(&shank_state, &shank);
    local.thigh_angle_x10 = wrap_angle_x10(
        thigh_state.angle_x10 - thigh_state.base_angle_x10);
    local.shank_angle_x10 = wrap_angle_x10(
        shank_state.angle_x10 - shank_state.base_angle_x10);
    raw_relative_x10 = wrap_angle_x10(local.shank_angle_x10 -
                                     local.thigh_angle_x10);

    if (!motion_active)
    {
        rt_int32_t baseline_error = wrap_angle_x10(raw_relative_x10 -
                                                   neutral_offset_x10);
        if (labs((long)baseline_error) <= KNEE_IMU_BASELINE_TRACK_BAND_X10)
        {
            /* Preserve sub-0.1-degree corrections instead of losing them to
             * integer division on every sample. */
            neutral_track_accumulator += baseline_error;
            neutral_offset_x10 += neutral_track_accumulator /
                                  KNEE_IMU_BASELINE_TRACK_DIVISOR;
            neutral_track_accumulator %= KNEE_IMU_BASELINE_TRACK_DIVISOR;
        }
        else
        {
            neutral_track_accumulator = 0;
        }
    }
    local.knee_angle_x10 = wrap_angle_x10(
        (raw_relative_x10 - neutral_offset_x10) * direction_sign);

    if (motion_active)
    {
        now = rt_tick_get();
        if (!motion_was_active)
        {
            candidate_status = KNEE_IMU_NEUTRAL;
            latched_status = KNEE_IMU_NEUTRAL;
            candidate_start_tick = now;
            local.peak_valgus_x10 = 0U;
            local.peak_outward_x10 = 0U;
            local.status = KNEE_IMU_NEUTRAL;
            result_hold_start_tick = 0;
        }
        if (local.knee_angle_x10 > 0 &&
            (rt_uint32_t)local.knee_angle_x10 > local.peak_valgus_x10)
            local.peak_valgus_x10 = (rt_uint32_t)local.knee_angle_x10;
        if (local.knee_angle_x10 < 0 &&
            (rt_uint32_t)(-local.knee_angle_x10) > local.peak_outward_x10)
            local.peak_outward_x10 = (rt_uint32_t)(-local.knee_angle_x10);

        desired = desired_status(local.knee_angle_x10, local.status);
        if (desired != candidate_status)
        {
            candidate_status = desired;
            candidate_start_tick = now;
        }
        if (ticks_to_ms(now - candidate_start_tick) >= KNEE_IMU_CONFIRM_MS)
        {
            /* A detected inward collapse is safety-priority for the rest of
             * the repetition.  Standing up cannot replace it with OUT. */
            if (candidate_status == KNEE_IMU_VALGUS)
                latched_status = KNEE_IMU_VALGUS;
            else if (candidate_status == KNEE_IMU_OUTWARD &&
                     latched_status != KNEE_IMU_VALGUS)
                latched_status = KNEE_IMU_OUTWARD;
            else if (candidate_status == KNEE_IMU_NEUTRAL &&
                     latched_status == KNEE_IMU_NEUTRAL)
                latched_status = KNEE_IMU_NEUTRAL;
        }
        local.status = latched_status;
    }
    else
    {
        now = rt_tick_get();
        if (motion_was_active)
        {
            result_hold_start_tick = now;
        }
        if (result_hold_start_tick != 0 &&
            ticks_to_ms(now - result_hold_start_tick) < KNEE_IMU_RESULT_HOLD_MS)
            local.status = latched_status;
        else
            local.status = KNEE_IMU_NEUTRAL;
    }
    motion_was_active = motion_active ? RT_TRUE : RT_FALSE;
    publish_snapshot(&local);
    emit_stream(&local);
}

#ifdef FINSH_USING_MSH
static void knee_imu_info(int argc, char **argv)
{
    knee_imu_snapshot_t snapshot;
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    knee_imu_get_snapshot(&snapshot);
    rt_kprintf("KIMU,online=%u,cal=%u,addr=0x%02x,angle_x10=%ld,"
               "thigh_x10=%ld,shank_x10=%ld,status=%s,motion=%u,"
               "peak_in_x10=%lu,peak_out_x10=%lu,sign=%d,axes=%u/%u\n",
               (unsigned)snapshot.online, (unsigned)snapshot.calibrated,
               (unsigned)snapshot.shank_i2c_address,
               (long)snapshot.knee_angle_x10,
               (long)snapshot.thigh_angle_x10,
               (long)snapshot.shank_angle_x10,
               knee_imu_status_short(snapshot.status),
               (unsigned)snapshot.motion_active,
               (unsigned long)snapshot.peak_valgus_x10,
               (unsigned long)snapshot.peak_outward_x10,
               (int)snapshot.direction_sign,
               (unsigned)snapshot.thigh_long_axis,
               (unsigned)snapshot.shank_long_axis);
}
MSH_CMD_EXPORT(knee_imu_info, show two-IMU knee posture state);

static void knee_imu_recal(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    recalibrate_requested = RT_TRUE;
    rt_kprintf("KIMU,recalibration_requested,stand_still=1\n");
}
MSH_CMD_EXPORT(knee_imu_recal, recalibrate both IMUs while standing still);

static void knee_imu_flip(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    direction_sign = (rt_int8_t)-direction_sign;
    knee_snapshot.direction_sign = direction_sign;
    candidate_status = KNEE_IMU_NEUTRAL;
    latched_status = KNEE_IMU_NEUTRAL;
    candidate_start_tick = 0;
    rt_kprintf("KIMU,direction_sign=%d,positive=valgus\n", (int)direction_sign);
}
MSH_CMD_EXPORT(knee_imu_flip, flip relative-angle direction after inward test);

static void knee_imu_stream(int argc, char **argv)
{
    if (argc >= 2)
        stream_enabled = strcmp(argv[1], "off") == 0 ? RT_FALSE : RT_TRUE;
    rt_kprintf("KIMU,stream=%s\n", stream_enabled ? "on" : "off");
}
MSH_CMD_EXPORT(knee_imu_stream, knee_imu_stream on or off);
#endif
