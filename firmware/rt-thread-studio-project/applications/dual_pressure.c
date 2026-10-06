/*
 * Dual pressure sensing for the smart knee brace.
 *
 * This module deliberately runs in its own low-priority thread. ADC or wiring
 * problems therefore cannot delay the existing MPU6050 motion state machine.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#ifdef FINSH_USING_MSH
#include <finsh.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "dual_pressure.h"
#include "knee_imu.h"

/* Temporary validation mode: pressure ADC values remain available for
 * diagnostics, but all externally visible knee-posture states come from the
 * relative thigh/shank IMU estimator. */
#define DP_POSTURE_SOURCE_IMU            1U

#define DP_ADC_DEVICE_NAME              "adc1"
#define DP_MEDIAL_ADC_CHANNEL           10U /* PC0 / ADC1_IN10 */
#define DP_LATERAL_ADC_CHANNEL          11U /* PC1 / ADC1_IN11 */

#define DP_SAMPLE_PERIOD_MS             20U
#define DP_CALIBRATION_SAMPLES          100U
#define DP_FILTER_DIVISOR               4U
#define DP_BASELINE_TRACK_DIVISOR       256U
#define DP_DEFAULT_ASYM_THRESHOLD       100U
#define DP_OUTWARD_ASYM_THRESHOLD       1000U
#define DP_RELEASE_THRESHOLD            60U
#define DP_MIN_ACTIVITY                 60U
#define DP_CONFIRM_SAMPLES              8U  /* 160 ms at 20 ms sampling */
#define DP_BALANCE_CONFIRM_SAMPLES      5U
#define DP_STREAM_PERIOD_MS             500U
#define DP_RESULT_HOLD_MS               3000U

#define DP_THREAD_STACK                 1024U
#define DP_THREAD_PRIORITY              20U
#define DP_THREAD_TIMESLICE             10U

static rt_adc_device_t pressure_adc = RT_NULL;
static rt_thread_t pressure_thread = RT_NULL;
static dual_pressure_snapshot_t pressure_snapshot;
static volatile rt_bool_t pressure_motion_active = RT_FALSE;
static volatile rt_bool_t pressure_recalibrate_requested = RT_FALSE;
/* Stream pressure frames by default so the ESP32/App does not depend on a
 * startup command arriving after the STM32 shell is ready. The MSH command
 * pressure_stream off remains available for diagnostics. */
static volatile rt_bool_t pressure_stream_enabled = RT_TRUE;
static volatile rt_bool_t pressure_force_raises_adc = RT_TRUE;
static volatile rt_uint32_t pressure_asym_threshold = DP_DEFAULT_ASYM_THRESHOLD;

const char *dual_pressure_status_short(dual_pressure_status_t status);

static dual_pressure_status_t pressure_effective_status(
    dual_pressure_status_t pressure_status)
{
#if DP_POSTURE_SOURCE_IMU
    knee_imu_snapshot_t imu;
    RT_UNUSED(pressure_status);
    knee_imu_get_snapshot(&imu);

    switch (imu.status)
    {
    case KNEE_IMU_CALIBRATING: return DUAL_PRESSURE_CALIBRATING;
    case KNEE_IMU_VALGUS:      return DUAL_PRESSURE_LATERAL_HIGH;
    case KNEE_IMU_OUTWARD:     return DUAL_PRESSURE_MEDIAL_HIGH;
    case KNEE_IMU_NEUTRAL:     return DUAL_PRESSURE_BALANCED;
    case KNEE_IMU_ERROR:
    case KNEE_IMU_OFFLINE:
    default:                   return DUAL_PRESSURE_ERROR;
    }
#else
    return pressure_status;
#endif
}

static void pressure_emit_standard_frame(const dual_pressure_snapshot_t *snapshot)
{
    if (snapshot == RT_NULL) return;

    rt_kprintf("PRESS,med=%lu,lat=%lu,dm=%ld,dl=%ld,diff=%ld,status=%s,motion=%u\n",
               (unsigned long)snapshot->medial_filtered,
               (unsigned long)snapshot->lateral_filtered,
               (long)snapshot->medial_change,
               (long)snapshot->lateral_change,
               (long)snapshot->pressure_difference,
               dual_pressure_status_short(
                   pressure_effective_status(snapshot->status)),
               (unsigned)snapshot->motion_active);
}

static rt_uint32_t abs_i32(rt_int32_t value)
{
    return value < 0 ? (rt_uint32_t)(-value) : (rt_uint32_t)value;
}

static rt_uint32_t ticks_to_ms(rt_tick_t ticks)
{
    return (rt_uint32_t)(((rt_uint64_t)ticks * 1000ULL) /
                         (rt_uint64_t)RT_TICK_PER_SECOND);
}

const char *dual_pressure_status_short(dual_pressure_status_t status)
{
    switch (status)
    {
    case DUAL_PRESSURE_CALIBRATING: return "CAL";
    case DUAL_PRESSURE_BALANCED:    return "BAL";
    case DUAL_PRESSURE_MEDIAL_HIGH: return "MED";
    case DUAL_PRESSURE_LATERAL_HIGH:return "LAT";
    case DUAL_PRESSURE_ERROR:       return "ERR";
    case DUAL_PRESSURE_OFFLINE:
    default:                        return "OFF";
    }
}

void dual_pressure_get_snapshot(dual_pressure_snapshot_t *snapshot)
{
    rt_base_t level;

    if (snapshot == RT_NULL) return;
    level = rt_hw_interrupt_disable();
    *snapshot = pressure_snapshot;
    rt_hw_interrupt_enable(level);
    snapshot->status = pressure_effective_status(snapshot->status);
}

void dual_pressure_set_motion_active(rt_bool_t active)
{
    pressure_motion_active = active ? RT_TRUE : RT_FALSE;
}

static void dual_pressure_publish(const dual_pressure_snapshot_t *snapshot)
{
    rt_base_t level = rt_hw_interrupt_disable();
    pressure_snapshot = *snapshot;
    rt_hw_interrupt_enable(level);
}

static dual_pressure_status_t pressure_desired_status(
    const dual_pressure_snapshot_t *snapshot,
    dual_pressure_status_t current)
{
    rt_uint32_t activity;
    rt_int32_t difference;

    if (!snapshot->motion_active)
    {
        return DUAL_PRESSURE_BALANCED;
    }

    activity = abs_i32(snapshot->medial_change) +
               abs_i32(snapshot->lateral_change);
    difference = snapshot->pressure_difference;

    if (activity < DP_MIN_ACTIVITY)
    {
        return DUAL_PRESSURE_BALANCED;
    }

    /* Current brace mapping: MED (positive difference) is knee outward.
     * Normal squats intentionally drive the knees outward, so only report
     * this direction when the asymmetry is exceptionally large. The LAT
     * (negative) knee-valgus path keeps its existing configurable threshold. */
    if (difference >= (rt_int32_t)DP_OUTWARD_ASYM_THRESHOLD)
    {
        return DUAL_PRESSURE_MEDIAL_HIGH;
    }
    if (difference <= -(rt_int32_t)snapshot->asymmetry_threshold)
    {
        return DUAL_PRESSURE_LATERAL_HIGH;
    }

    /* Hysteresis prevents MED/LAT from flickering at the threshold. */
    if (current == DUAL_PRESSURE_MEDIAL_HIGH &&
        difference >= (rt_int32_t)DP_RELEASE_THRESHOLD)
    {
        return current;
    }
    if (current == DUAL_PRESSURE_LATERAL_HIGH &&
        difference <= -(rt_int32_t)DP_RELEASE_THRESHOLD)
    {
        return current;
    }

    return DUAL_PRESSURE_BALANCED;
}

static void dual_pressure_thread_entry(void *parameter)
{
    dual_pressure_snapshot_t local;
    rt_uint64_t medial_cal_sum = 0;
    rt_uint64_t lateral_cal_sum = 0;
    rt_uint32_t calibration_count = 0;
    dual_pressure_status_t candidate = DUAL_PRESSURE_BALANCED;
    dual_pressure_status_t latched_status = DUAL_PRESSURE_BALANCED;
    rt_uint8_t candidate_count = 0;
    rt_tick_t last_stream_tick = 0;
    rt_tick_t result_hold_start_tick = 0;
    rt_bool_t motion_was_active = RT_FALSE;

    RT_UNUSED(parameter);
    rt_memset(&local, 0, sizeof(local));
    local.online = RT_TRUE;
    local.status = DUAL_PRESSURE_CALIBRATING;
    local.asymmetry_threshold = pressure_asym_threshold;
    local.force_raises_adc = pressure_force_raises_adc;
    dual_pressure_publish(&local);

    while (1)
    {
        rt_uint32_t medial_raw;
        rt_uint32_t lateral_raw;
        dual_pressure_status_t desired;
        rt_uint8_t needed_samples;

        medial_raw = rt_adc_read(pressure_adc, DP_MEDIAL_ADC_CHANNEL);
        lateral_raw = rt_adc_read(pressure_adc, DP_LATERAL_ADC_CHANNEL);

        if (pressure_recalibrate_requested)
        {
            pressure_recalibrate_requested = RT_FALSE;
            medial_cal_sum = 0;
            lateral_cal_sum = 0;
            calibration_count = 0;
            candidate_count = 0;
            latched_status = DUAL_PRESSURE_BALANCED;
            result_hold_start_tick = 0;
            motion_was_active = RT_FALSE;
            local.calibrated = RT_FALSE;
            local.status = DUAL_PRESSURE_CALIBRATING;
            rt_kprintf("PRESS,CAL_BEGIN,stand_still=1,samples=%u\n",
                       (unsigned)DP_CALIBRATION_SAMPLES);
        }

        local.medial_raw = medial_raw;
        local.lateral_raw = lateral_raw;
        local.motion_active = pressure_motion_active;
        local.force_raises_adc = pressure_force_raises_adc;
        local.asymmetry_threshold = pressure_asym_threshold;

        if (!local.calibrated)
        {
            medial_cal_sum += medial_raw;
            lateral_cal_sum += lateral_raw;
            calibration_count++;
            local.status = DUAL_PRESSURE_CALIBRATING;

            if (calibration_count >= DP_CALIBRATION_SAMPLES)
            {
                local.medial_baseline =
                    (rt_uint32_t)(medial_cal_sum / calibration_count);
                local.lateral_baseline =
                    (rt_uint32_t)(lateral_cal_sum / calibration_count);
                local.medial_filtered = local.medial_baseline;
                local.lateral_filtered = local.lateral_baseline;
                local.medial_change = 0;
                local.lateral_change = 0;
                local.pressure_difference = 0;
                local.calibrated = RT_TRUE;
                local.status = DUAL_PRESSURE_BALANCED;
                candidate = DUAL_PRESSURE_BALANCED;
                candidate_count = 0;
                latched_status = DUAL_PRESSURE_BALANCED;
                result_hold_start_tick = 0;
                motion_was_active = RT_FALSE;
                rt_kprintf("PRESS,CAL_DONE,med_base=%lu,lat_base=%lu\n",
                           (unsigned long)local.medial_baseline,
                           (unsigned long)local.lateral_baseline);
            }

            dual_pressure_publish(&local);
            if (pressure_stream_enabled &&
                (last_stream_tick == 0 ||
                 ticks_to_ms(rt_tick_get() - last_stream_tick) >= DP_STREAM_PERIOD_MS))
            {
                rt_kprintf("PRESS,med=%lu,lat=%lu,dm=0,dl=0,diff=0,status=%s,motion=0\n",
                           (unsigned long)medial_raw,
                           (unsigned long)lateral_raw,
                           dual_pressure_status_short(
                               pressure_effective_status(local.status)));
                last_stream_tick = rt_tick_get();
            }
            rt_thread_mdelay(DP_SAMPLE_PERIOD_MS);
            continue;
        }

        local.medial_filtered =
            (local.medial_filtered * (DP_FILTER_DIVISOR - 1U) +
             medial_raw + DP_FILTER_DIVISOR / 2U) / DP_FILTER_DIVISOR;
        local.lateral_filtered =
            (local.lateral_filtered * (DP_FILTER_DIVISOR - 1U) +
             lateral_raw + DP_FILTER_DIVISOR / 2U) / DP_FILTER_DIVISOR;

        /* Follow slow strap drift only while the motion state machine is idle. */
        if (!local.motion_active)
        {
            local.medial_baseline =
                (local.medial_baseline * (DP_BASELINE_TRACK_DIVISOR - 1U) +
                 local.medial_filtered + DP_BASELINE_TRACK_DIVISOR / 2U) /
                DP_BASELINE_TRACK_DIVISOR;
            local.lateral_baseline =
                (local.lateral_baseline * (DP_BASELINE_TRACK_DIVISOR - 1U) +
                 local.lateral_filtered + DP_BASELINE_TRACK_DIVISOR / 2U) /
                DP_BASELINE_TRACK_DIVISOR;
        }

        if (local.force_raises_adc)
        {
            local.medial_change = (rt_int32_t)local.medial_filtered -
                                  (rt_int32_t)local.medial_baseline;
            local.lateral_change = (rt_int32_t)local.lateral_filtered -
                                   (rt_int32_t)local.lateral_baseline;
        }
        else
        {
            local.medial_change = (rt_int32_t)local.medial_baseline -
                                  (rt_int32_t)local.medial_filtered;
            local.lateral_change = (rt_int32_t)local.lateral_baseline -
                                   (rt_int32_t)local.lateral_filtered;
        }
        local.pressure_difference = local.medial_change - local.lateral_change;

        /* Start each motion with a fresh posture result. Once a confirmed
         * MED/LAT event is seen, keep that first abnormal direction for the
         * complete movement. The opposite unloading pressure during ascent
         * must not overwrite the fault that occurred during descent. */
        if (local.motion_active && !motion_was_active)
        {
            candidate = DUAL_PRESSURE_BALANCED;
            candidate_count = 0;
            latched_status = DUAL_PRESSURE_BALANCED;
            result_hold_start_tick = 0;
            local.status = DUAL_PRESSURE_BALANCED;
        }
        else if (!local.motion_active && motion_was_active)
        {
            candidate = DUAL_PRESSURE_BALANCED;
            candidate_count = 0;
            if (latched_status == DUAL_PRESSURE_MEDIAL_HIGH ||
                latched_status == DUAL_PRESSURE_LATERAL_HIGH)
            {
                result_hold_start_tick = rt_tick_get();
            }
        }
        motion_was_active = local.motion_active;

        if (local.motion_active)
        {
            if (latched_status == DUAL_PRESSURE_MEDIAL_HIGH ||
                latched_status == DUAL_PRESSURE_LATERAL_HIGH)
            {
                local.status = latched_status;
            }
            else
            {
                desired = pressure_desired_status(&local, local.status);
                needed_samples = desired == DUAL_PRESSURE_BALANCED ?
                                 DP_BALANCE_CONFIRM_SAMPLES : DP_CONFIRM_SAMPLES;
                if (desired != candidate)
                {
                    candidate = desired;
                    candidate_count = 1U;
                }
                else if (candidate_count < needed_samples)
                {
                    candidate_count++;
                }
                if (candidate_count >= needed_samples)
                {
                    local.status = candidate;
                    if (candidate == DUAL_PRESSURE_MEDIAL_HIGH ||
                        candidate == DUAL_PRESSURE_LATERAL_HIGH)
                    {
                        latched_status = candidate;
                    }
                }
            }
        }
        else if ((latched_status == DUAL_PRESSURE_MEDIAL_HIGH ||
                  latched_status == DUAL_PRESSURE_LATERAL_HIGH) &&
                 result_hold_start_tick != 0 &&
                 ticks_to_ms(rt_tick_get() - result_hold_start_tick) <
                     DP_RESULT_HOLD_MS)
        {
            local.status = latched_status;
        }
        else
        {
            local.status = DUAL_PRESSURE_BALANCED;
            latched_status = DUAL_PRESSURE_BALANCED;
            result_hold_start_tick = 0;
        }

        dual_pressure_publish(&local);

        if (pressure_stream_enabled &&
            (last_stream_tick == 0 ||
             ticks_to_ms(rt_tick_get() - last_stream_tick) >= DP_STREAM_PERIOD_MS))
        {
            rt_kprintf("PRESS,med=%lu,lat=%lu,dm=%ld,dl=%ld,diff=%ld,status=%s,motion=%u\n",
                       (unsigned long)local.medial_filtered,
                       (unsigned long)local.lateral_filtered,
                       (long)local.medial_change,
                       (long)local.lateral_change,
                       (long)local.pressure_difference,
                       dual_pressure_status_short(
                           pressure_effective_status(local.status)),
                       (unsigned)local.motion_active);
            last_stream_tick = rt_tick_get();
        }

        rt_thread_mdelay(DP_SAMPLE_PERIOD_MS);
    }
}

rt_err_t dual_pressure_init(void)
{
    rt_err_t result;

    rt_memset(&pressure_snapshot, 0, sizeof(pressure_snapshot));
    pressure_snapshot.status = DUAL_PRESSURE_OFFLINE;

    pressure_adc = (rt_adc_device_t)rt_device_find(DP_ADC_DEVICE_NAME);
    if (pressure_adc == RT_NULL)
    {
        pressure_snapshot.status = DUAL_PRESSURE_ERROR;
        rt_kprintf("PRESS,ERROR,adc1_not_found\n");
        pressure_emit_standard_frame(&pressure_snapshot);
        return -RT_ENOSYS;
    }

    result = rt_adc_enable(pressure_adc, DP_MEDIAL_ADC_CHANNEL);
    if (result != RT_EOK)
    {
        pressure_snapshot.status = DUAL_PRESSURE_ERROR;
        rt_kprintf("PRESS,ERROR,enable_channel_10=%d\n", result);
        pressure_emit_standard_frame(&pressure_snapshot);
        return result;
    }
    result = rt_adc_enable(pressure_adc, DP_LATERAL_ADC_CHANNEL);
    if (result != RT_EOK)
    {
        rt_adc_disable(pressure_adc, DP_MEDIAL_ADC_CHANNEL);
        pressure_snapshot.status = DUAL_PRESSURE_ERROR;
        rt_kprintf("PRESS,ERROR,enable_channel_11=%d\n", result);
        pressure_emit_standard_frame(&pressure_snapshot);
        return result;
    }

    pressure_thread = rt_thread_create("pressure", dual_pressure_thread_entry,
                                       RT_NULL, DP_THREAD_STACK,
                                       DP_THREAD_PRIORITY,
                                       DP_THREAD_TIMESLICE);
    if (pressure_thread == RT_NULL)
    {
        rt_adc_disable(pressure_adc, DP_MEDIAL_ADC_CHANNEL);
        rt_adc_disable(pressure_adc, DP_LATERAL_ADC_CHANNEL);
        pressure_snapshot.status = DUAL_PRESSURE_ERROR;
        pressure_emit_standard_frame(&pressure_snapshot);
        return -RT_ENOMEM;
    }

    rt_thread_startup(pressure_thread);
    rt_kprintf("PRESS,READY,adc=adc1,medial=PC0/ch10,lateral=PC1/ch11,period=%ums\n",
               (unsigned)DP_SAMPLE_PERIOD_MS);
    return RT_EOK;
}

#ifdef FINSH_USING_MSH
static void pressure_show(int argc, char **argv)
{
    dual_pressure_snapshot_t snapshot;
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    dual_pressure_get_snapshot(&snapshot);
    rt_kprintf("PRESS,online=%u,cal=%u,med_raw=%lu,lat_raw=%lu,med_f=%lu,lat_f=%lu," \
               "med_base=%lu,lat_base=%lu,dm=%ld,dl=%ld,diff=%ld,status=%s," \
               "motion=%u,polarity=%s,threshold=%lu\n",
               (unsigned)snapshot.online,
               (unsigned)snapshot.calibrated,
               (unsigned long)snapshot.medial_raw,
               (unsigned long)snapshot.lateral_raw,
               (unsigned long)snapshot.medial_filtered,
               (unsigned long)snapshot.lateral_filtered,
               (unsigned long)snapshot.medial_baseline,
               (unsigned long)snapshot.lateral_baseline,
               (long)snapshot.medial_change,
               (long)snapshot.lateral_change,
               (long)snapshot.pressure_difference,
               dual_pressure_status_short(snapshot.status),
               (unsigned)snapshot.motion_active,
               snapshot.force_raises_adc ? "up" : "down",
               (unsigned long)snapshot.asymmetry_threshold);
}
MSH_CMD_EXPORT(pressure_show, show one dual-pressure snapshot);

static void pressure_recal(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    pressure_recalibrate_requested = RT_TRUE;
    rt_kprintf("PRESS,recalibration_requested\n");
}
MSH_CMD_EXPORT(pressure_recal, recalibrate pressure baseline while standing);

static void pressure_stream(int argc, char **argv)
{
    if (argc != 2 ||
        (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0))
    {
        rt_kprintf("usage: pressure_stream on|off\n");
        return;
    }
    pressure_stream_enabled = strcmp(argv[1], "on") == 0 ? RT_TRUE : RT_FALSE;
    rt_kprintf("PRESS,stream=%s\n", pressure_stream_enabled ? "on" : "off");
    if (pressure_stream_enabled)
    {
        dual_pressure_snapshot_t snapshot;
        dual_pressure_get_snapshot(&snapshot);
        pressure_emit_standard_frame(&snapshot);
    }
}
MSH_CMD_EXPORT(pressure_stream, enable or disable 500ms pressure output);

static void pressure_polarity(int argc, char **argv)
{
    if (argc != 2 ||
        (strcmp(argv[1], "up") != 0 && strcmp(argv[1], "down") != 0))
    {
        rt_kprintf("usage: pressure_polarity up|down\n");
        return;
    }
    pressure_force_raises_adc = strcmp(argv[1], "up") == 0 ? RT_TRUE : RT_FALSE;
    rt_kprintf("PRESS,polarity=%s\n", pressure_force_raises_adc ? "up" : "down");
}
MSH_CMD_EXPORT(pressure_polarity, set whether more force raises or lowers ADC);

static void pressure_threshold(int argc, char **argv)
{
    long value;
    if (argc != 2)
    {
        rt_kprintf("usage: pressure_threshold 20..1000\n");
        return;
    }
    value = strtol(argv[1], RT_NULL, 10);
    if (value < 20 || value > 1000)
    {
        rt_kprintf("pressure threshold must be 20..1000\n");
        return;
    }
    pressure_asym_threshold = (rt_uint32_t)value;
    rt_kprintf("PRESS,threshold=%ld\n", value);
}
MSH_CMD_EXPORT(pressure_threshold, set raw ADC asymmetry threshold);
#endif
