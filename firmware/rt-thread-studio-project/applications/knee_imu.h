/*
 * Two-MPU6050 frontal-plane knee posture estimator.
 *
 * The existing 0x68 MPU6050 remains the thigh/motion sensor.  A second
 * MPU6050 is mounted on the proximal shank and uses an independent software
 * I2C bus, so AD0 may stay low at the default address 0x68.
 * This module has no thread and allocates no heap memory: it is fed by the
 * existing 20 ms MPU sampling thread.
 */

#ifndef __KNEE_IMU_H__
#define __KNEE_IMU_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    KNEE_IMU_OFFLINE = 0,
    KNEE_IMU_CALIBRATING,
    KNEE_IMU_NEUTRAL,
    KNEE_IMU_VALGUS,
    KNEE_IMU_OUTWARD,
    KNEE_IMU_ERROR,
} knee_imu_status_t;

typedef struct
{
    rt_int16_t ax;
    rt_int16_t ay;
    rt_int16_t az;
    rt_int16_t gx;
    rt_int16_t gy;
    rt_int16_t gz;
} knee_imu_sample_t;

typedef struct
{
    rt_bool_t online;
    rt_bool_t calibrated;
    rt_bool_t motion_active;
    rt_uint8_t shank_i2c_address;
    rt_uint8_t thigh_long_axis;
    rt_uint8_t shank_long_axis;
    rt_int8_t direction_sign;
    rt_int32_t thigh_angle_x10;
    rt_int32_t shank_angle_x10;
    rt_int32_t knee_angle_x10;       /* Positive is valgus after sign setup. */
    rt_uint32_t peak_valgus_x10;
    rt_uint32_t peak_outward_x10;
    rt_uint32_t valgus_threshold_x10;
    rt_uint32_t outward_threshold_x10;
    knee_imu_status_t status;
} knee_imu_snapshot_t;

rt_err_t knee_imu_init(void);
void knee_imu_process(const knee_imu_sample_t *thigh, rt_bool_t motion_active);
void knee_imu_get_snapshot(knee_imu_snapshot_t *snapshot);
const char *knee_imu_status_short(knee_imu_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* __KNEE_IMU_H__ */
