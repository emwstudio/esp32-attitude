#include "madgwick.h"

#include <math.h>

static float inv_sqrt(float x)
{
    return 1.0f / sqrtf(x);
}

void madgwick_init(madgwick_t *f, float beta)
{
    f->beta = beta;
    f->q0 = 1.0f;
    f->q1 = f->q2 = f->q3 = 0.0f;
}

void madgwick_update(madgwick_t *f, float gx, float gy, float gz,
                     float ax, float ay, float az, float dt)
{
    float q0 = f->q0, q1 = f->q1, q2 = f->q2, q3 = f->q3;
    float norm;
    float s0, s1, s2, s3;
    float qdot0, qdot1, qdot2, qdot3;

    qdot0 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
    qdot1 = 0.5f * (q0 * gx + q2 * gz - q3 * gy);
    qdot2 = 0.5f * (q0 * gy - q1 * gz + q3 * gx);
    qdot3 = 0.5f * (q0 * gz + q1 * gy - q2 * gx);

    if (!(ax == 0.0f && ay == 0.0f && az == 0.0f)) {
        norm = inv_sqrt(ax * ax + ay * ay + az * az);
        ax *= norm;
        ay *= norm;
        az *= norm;

        float _2q0 = 2.0f * q0, _2q1 = 2.0f * q1, _2q2 = 2.0f * q2, _2q3 = 2.0f * q3;
        float _4q0 = 4.0f * q0, _4q1 = 4.0f * q1, _4q2 = 4.0f * q2;
        float _8q1 = 8.0f * q1, _8q2 = 8.0f * q2;
        float q0q0 = q0 * q0, q1q1 = q1 * q1, q2q2 = q2 * q2, q3q3 = q3 * q3;

        s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
        s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay - _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
        s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
        s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;

        norm = inv_sqrt(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
        s0 *= norm;
        s1 *= norm;
        s2 *= norm;
        s3 *= norm;

        qdot0 -= f->beta * s0;
        qdot1 -= f->beta * s1;
        qdot2 -= f->beta * s2;
        qdot3 -= f->beta * s3;
    }

    q0 += qdot0 * dt;
    q1 += qdot1 * dt;
    q2 += qdot2 * dt;
    q3 += qdot3 * dt;

    norm = inv_sqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    f->q0 = q0 * norm;
    f->q1 = q1 * norm;
    f->q2 = q2 * norm;
    f->q3 = q3 * norm;
}

void madgwick_euler(const madgwick_t *f, float *roll, float *pitch, float *yaw)
{
    float q0 = f->q0, q1 = f->q1, q2 = f->q2, q3 = f->q3;

    float sinp = 2.0f * (q0 * q2 - q3 * q1);
    if (sinp > 1.0f) sinp = 1.0f;
    if (sinp < -1.0f) sinp = -1.0f;

    if (roll)  *roll  = atan2f(2.0f * (q0 * q1 + q2 * q3), 1.0f - 2.0f * (q1 * q1 + q2 * q2)) * 180.0f / M_PI;
    if (pitch) *pitch = asinf(sinp) * 180.0f / M_PI;
    if (yaw)   *yaw   = atan2f(2.0f * (q1 * q2 + q0 * q3), 1.0f - 2.0f * (q2 * q2 + q3 * q3)) * 180.0f / M_PI;
}

void madgwick_up(const madgwick_t *f, float *ux, float *uy, float *uz)
{
    float q0 = f->q0, q1 = f->q1, q2 = f->q2, q3 = f->q3;
    if (ux) *ux = 2.0f * (q1 * q3 - q0 * q2);
    if (uy) *uy = 2.0f * (q0 * q1 + q2 * q3);
    if (uz) *uz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;
}
