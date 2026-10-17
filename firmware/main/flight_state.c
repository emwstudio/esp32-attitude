#include "flight_state.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "qmi8658.h"
#include "madgwick.h"
#include "audio_out.h"

static const char *TAG = "imu";

static flight_state_t s_state;
static SemaphoreHandle_t s_mtx;

static volatile bool s_calib_pending = true; /* 开机即校准 */

void attitude_recalibrate(void)
{
    s_calib_pending = true;
}

void flight_state_init(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
    }
}

void flight_state_update(const flight_state_t *s)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state = *s;
    xSemaphoreGive(s_mtx);
}

void flight_state_snapshot(flight_state_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mtx);
}

static float wrap360(float deg)
{
    while (deg < 0.0f) deg += 360.0f;
    while (deg >= 360.0f) deg -= 360.0f;
    return deg;
}

static float wrap180(float deg)
{
    while (deg > 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

#define RAD2DEG(x) ((x) * 180.0f / (float)M_PI)

static void imu_task(void *arg)
{
    madgwick_t filter;
    madgwick_init(&filter, 0.08f);

    flight_state_t st = {
        .airspeed_kmh = 140.0f,
        .altitude_m = 800.0f,
    };
    st.imu_ok = qmi8658_present();

    const float dt = 0.01f; /* 100 Hz */
    float heading = 0.0f;

    /* 校准状态：开机/触屏后采 100 个样本的平均"上"向量作为平飞基准，
       并据此自动识别持握方式（平放=uz 大；竖拿=uy 大）。
       同一窗口顺便测量陀螺仪零偏（校准时设备静止），之后永久扣除 */
    static float s_uref[3];
    static int s_mode; /* 0=未定 1=平放 2=竖拿 */
    static float s_gbias[3];
    int calib_n = 0;
    float calib_u[3] = {0, 0, 0};
    float calib_g[3] = {0, 0, 0};

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));

        qmi8658_data_t d;
        if (st.imu_ok && qmi8658_read(&d) == ESP_OK) {
            st.ax = d.ax; st.ay = d.ay; st.az = d.az;
            /* 扣除陀螺仪零偏（校准窗口测得） */
            float gx = d.gx - s_gbias[0];
            float gy = d.gy - s_gbias[1];
            float gz = d.gz - s_gbias[2];
            madgwick_update(&filter,
                            gx * (float)M_PI / 180.0f,
                            gy * (float)M_PI / 180.0f,
                            gz * (float)M_PI / 180.0f,
                            d.ax, d.ay, d.az, dt);

            float ux, uy, uz;
            madgwick_up(&filter, &ux, &uy, &uz);

            /* 航向：陀螺仪角速度在世界竖直轴（"上"方向）上的投影积分（已扣零偏） */
            float hrate = gx * ux + gy * uy + gz * uz;

            /* 静止残余零偏学习（兜底）：无磁力计航向必漂，静止时慢速学习并扣除 */
            static float s_yaw_bias = 0.0f;
            bool still = fabsf(gx) < 3.5f && fabsf(gy) < 3.5f && fabsf(gz) < 3.5f;
            if (still) {
                s_yaw_bias += (hrate - s_yaw_bias) * dt * 0.3f;
            }
            heading = wrap360(heading - (hrate - s_yaw_bias) * dt);

            if (s_calib_pending) {
                calib_u[0] += ux; calib_u[1] += uy; calib_u[2] += uz;
                calib_g[0] += d.gx; calib_g[1] += d.gy; calib_g[2] += d.gz;  /* 原始陀螺，测零偏 */
                if (++calib_n >= 100) {
                    s_uref[0] = calib_u[0] / calib_n;
                    s_uref[1] = calib_u[1] / calib_n;
                    s_uref[2] = calib_u[2] / calib_n;
                    s_gbias[0] = calib_g[0] / calib_n;
                    s_gbias[1] = calib_g[1] / calib_n;
                    s_gbias[2] = calib_g[2] / calib_n;
                    s_mode = fabsf(s_uref[2]) > 0.7f ? 1 : 2;
                    s_calib_pending = false;
                    calib_n = 0;
                    calib_u[0] = calib_u[1] = calib_u[2] = 0.0f;
                    calib_g[0] = calib_g[1] = calib_g[2] = 0.0f;
                    audio_out_play("chime");  /* 校准完成提示音 */
                    ESP_LOGI(TAG, "calibrated: mode=%s uref=(%.2f,%.2f,%.2f) gbias=(%.2f,%.2f,%.2f)",
                             s_mode == 1 ? "flat" : "vertical",
                             s_uref[0], s_uref[1], s_uref[2],
                             s_gbias[0], s_gbias[1], s_gbias[2]);
                }
            }

            if (s_mode == 1) {
                /* 平放（屏幕朝上）：x_s=右，y_s=机头
                   俯仰=绕右翼轴横滚=绕机头轴，用上向量的屏幕分量 */
                st.pitch = RAD2DEG(asinf(clampf(uy, -1.0f, 1.0f)) - asinf(clampf(s_uref[1], -1.0f, 1.0f)));
                st.roll  = RAD2DEG(asinf(clampf(ux, -1.0f, 1.0f)) - asinf(clampf(s_uref[0], -1.0f, 1.0f)));
            } else {
                /* 竖拿（屏幕朝人）：x_s=右，y_s=上，z_s=朝人
                   横滚=上向量在屏幕平面内的倾角；俯仰=上向量沿屏幕法线的分量 */
                float roll_now  = RAD2DEG(atan2f(ux, uy));
                float roll_ref  = RAD2DEG(atan2f(s_uref[0], s_uref[1]));
                float pitch_now = RAD2DEG(asinf(clampf(uz, -1.0f, 1.0f)));
                float pitch_ref = RAD2DEG(asinf(clampf(s_uref[2], -1.0f, 1.0f)));
                st.roll  = wrap180(roll_now - roll_ref);
                st.pitch = -(pitch_now - pitch_ref);   /* 顶部外倒=俯冲=负=地平线上移 */
            }
            st.heading = heading;
        } else {
            /* No IMU: slow demo sweep so the UI is still alive */
            int64_t now = esp_timer_get_time();
            float t = (now / 1000) / 1000.0f;
            st.roll = 25.0f * sinf(t * 0.5f);
            st.pitch = 10.0f * sinf(t * 0.33f);
            st.heading = wrap360(st.heading + 5.0f * dt);
        }

        /* Demo flight model: pitch drives climb & speed */
        float target_speed = 140.0f - st.pitch * 2.5f - fabsf(st.roll) * 0.3f;
        if (target_speed < 40.0f) target_speed = 40.0f;
        st.airspeed_kmh += (target_speed - st.airspeed_kmh) * dt * 0.8f;
        st.vspeed_ms = st.pitch * 1.2f;
        st.altitude_m += st.vspeed_ms * dt;

        flight_state_update(&st);
    }
}

esp_err_t imu_task_start(void)
{
    flight_state_init();

    BaseType_t ok = xTaskCreatePinnedToCore(imu_task, "imu", 3072, NULL, 5, NULL, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "imu task started, sensor %s", qmi8658_present() ? "ok" : "MISSING (demo sweep)");
    return ESP_OK;
}
