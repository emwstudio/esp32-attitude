#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float roll;          /* deg, >0 = right wing down */
    float pitch;         /* deg, >0 = nose up */
    float heading;       /* deg 0..360 (gyro-integrated, drifts) */
    float airspeed_kmh;  /* simulated */
    float altitude_m;    /* simulated */
    float vspeed_ms;     /* simulated, >0 = climbing */
    float ax, ay, az;    /* raw accel (g), debug */
    bool  imu_ok;
} flight_state_t;

void flight_state_init(void);
void flight_state_update(const flight_state_t *s);
void flight_state_snapshot(flight_state_t *out);

/* 把当前姿态设为零点（开机自动做一次；之后点触屏幕可重新校准） */
void attitude_recalibrate(void);


/* IMU + Madgwick + demo flight model task (100 Hz) */
esp_err_t imu_task_start(void);

#ifdef __cplusplus
}
#endif
