/*
 * Dual pressure sensing for the smart knee brace.
 *
 * PC0 / ADC1_IN10: medial (inner-knee) pressure module.
 * PC1 / ADC1_IN11: lateral (outer-knee) pressure module.
 */

#ifndef __DUAL_PRESSURE_H__
#define __DUAL_PRESSURE_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DUAL_PRESSURE_OFFLINE = 0,
    DUAL_PRESSURE_CALIBRATING,
    DUAL_PRESSURE_BALANCED,
    DUAL_PRESSURE_MEDIAL_HIGH,
    DUAL_PRESSURE_LATERAL_HIGH,
    DUAL_PRESSURE_ERROR,
} dual_pressure_status_t;

typedef struct
{
    rt_bool_t online;
    rt_bool_t calibrated;
    rt_bool_t motion_active;
    rt_bool_t force_raises_adc;
    rt_uint32_t medial_raw;
    rt_uint32_t lateral_raw;
    rt_uint32_t medial_filtered;
    rt_uint32_t lateral_filtered;
    rt_uint32_t medial_baseline;
    rt_uint32_t lateral_baseline;
    rt_int32_t medial_change;
    rt_int32_t lateral_change;
    rt_int32_t pressure_difference;
    rt_uint32_t asymmetry_threshold;
    dual_pressure_status_t status;
} dual_pressure_snapshot_t;

rt_err_t dual_pressure_init(void);
void dual_pressure_set_motion_active(rt_bool_t active);
void dual_pressure_get_snapshot(dual_pressure_snapshot_t *snapshot);
const char *dual_pressure_status_short(dual_pressure_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* __DUAL_PRESSURE_H__ */
