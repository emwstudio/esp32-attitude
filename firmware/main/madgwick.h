#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float beta;
    float q0, q1, q2, q3;
} madgwick_t;

void madgwick_init(madgwick_t *f, float beta);

/* IMU-only update. gx/gy/gz in rad/s, ax/ay/az in g (any consistent unit), dt in seconds */
void madgwick_update(madgwick_t *f, float gx, float gy, float gz,
                     float ax, float ay, float az, float dt);

/* Euler angles in degrees: roll (about X), pitch (about Y), yaw (about Z, integrated, drifts) */
void madgwick_euler(const madgwick_t *f, float *roll, float *pitch, float *yaw);

/* 世界"上"方向在机体坐标系中的单位向量（Madgwick 对加速度的估计） */
void madgwick_up(const madgwick_t *f, float *ux, float *uy, float *uz);

#ifdef __cplusplus
}
#endif
