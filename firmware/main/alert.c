#include "alert.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "flight_state.h"
#include "audio_out.h"
#include "voice_link.h"
#include "driver/gpio.h"

/* 校准按键：BOOT(GPIO0)；KEY(GPIO18) 已改为语音对话 PTT（见 voice_link.c） */
#define KEY_CALIB_B GPIO_NUM_0

static const char *TAG = "alert";

typedef enum {
    ALERT_NONE = 0,
    ALERT_PITCH,      /* excessive pitch */
    ALERT_BANK,       /* bank angle */
    ALERT_SINK,       /* sink rate */
    ALERT_STALL,      /* low airspeed */
    ALERT_OVERSPEED,  /* overspeed */
    ALERT_PULL_UP,    /* whoop whoop pull up */
} alert_id_t;

typedef struct {
    alert_id_t id;
    const char *sound;
    int repeat_ms;      /* re-announce interval while active */
    bool active;
    int64_t last_play;  /* us */
} alert_rule_t;

/* evaluate() 中靠前的先返回 = 优先级更高（pull_up > overspeed > stall > sink > bank > pitch） */
static alert_rule_t s_rules[] = {
    {ALERT_PITCH,     "pitch_high",  8000, false, 0},
    {ALERT_BANK,      "bank_angle",  5000, false, 0},
    {ALERT_SINK,      "sink_rate",   4000, false, 0},
    {ALERT_STALL,     "stall",       3000, false, 0},
    {ALERT_OVERSPEED, "boverspeed",  3000, false, 0},
    {ALERT_PULL_UP,   "pull_up",     2500, false, 0},
};
#define N_RULES (sizeof(s_rules) / sizeof(s_rules[0]))

static alert_id_t evaluate(const flight_state_t *s)
{
    if (s->vspeed_ms < -20.0f && s->altitude_m < 300.0f) return ALERT_PULL_UP;
    if (s->airspeed_kmh > 200.0f)                        return ALERT_OVERSPEED;
    if (s->airspeed_kmh < 70.0f)                         return ALERT_STALL;
    if (s->vspeed_ms < -16.0f && s->altitude_m < 800.0f) return ALERT_SINK;
    if (fabsf(s->roll) > 45.0f)                          return ALERT_BANK;
    if (s->pitch > 30.0f || s->pitch < -25.0f)           return ALERT_PITCH;
    return ALERT_NONE;
}

/* 波音进近高度喊话：下降穿越阈值时播报（波音 737 GPWS 标准点位） */
static const struct {
    int alt_ft;
    const char *sound;
} s_callouts[] = {
    {2500, "b2500"}, {1000, "b1000"}, {500, "b500"}, {400, "b400"},
    {300, "b300"}, {200, "bminimums"}, {100, "b100"}, {50, "b50"},
    {40, "b40"}, {30, "b30"}, {20, "b20"}, {10, "b10"}, {5, "b5"},
};

/* 下降穿越检测；prev<0 表示未初始化 */
static void check_alt_callouts(float alt_ft, bool descending, bool alert_active)
{
    static float s_prev_ft = -1.0f;
    if (alert_active || audio_out_busy()) {
        s_prev_ft = alt_ft;
        return;
    }
    if (descending && s_prev_ft > 0.0f) {
        for (size_t i = 0; i < sizeof(s_callouts) / sizeof(s_callouts[0]); i++) {
            int th = s_callouts[i].alt_ft;
            if (s_prev_ft > (float)th && alt_ft <= (float)th) {
                if (audio_out_play(s_callouts[i].sound) == ESP_OK) {
                    ESP_LOGI(TAG, "callout %d ft -> %s.wav", th, s_callouts[i].sound);
                }
                break;  /* 一次穿越只报最靠近的一个 */
            }
        }
    }
    s_prev_ft = alt_ft;
}

static alert_rule_t *find_rule(alert_id_t id)
{
    for (size_t i = 0; i < N_RULES; i++) {
        if (s_rules[i].id == id) return &s_rules[i];
    }
    return NULL;
}

static void alert_task(void *arg)
{
    (void)arg;

    gpio_config_t key_cfg = {
        .pin_bit_mask = 1ULL << KEY_CALIB_B,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&key_cfg);
    int key_last_b = 1;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        int kb = gpio_get_level(KEY_CALIB_B);
        if (key_last_b == 1 && kb == 0) {
            audio_out_play("bap-disconnect");   /* 断开自动驾驶警报声：接管手飞重新校准 */
            attitude_recalibrate();
            ESP_LOGI(TAG, "recalibrate triggered (key0=%d)", kb);
        }
        key_last_b = kb;

        flight_state_t s;
        flight_state_snapshot(&s);

        alert_id_t cur = evaluate(&s);
        for (size_t i = 0; i < N_RULES; i++) {
            s_rules[i].active = (s_rules[i].id == cur);
        }

        /* 高度喊话：下降穿越阈值时播报（仅最紧急的告警压制喊话，其余排队播放；对话中让路） */
        bool suppress = (cur == ALERT_PULL_UP || cur == ALERT_OVERSPEED || cur == ALERT_STALL);
        check_alt_callouts(s.altitude_m * 3.28084f, s.vspeed_ms < -0.5f, suppress || voice_link_active());

        if (cur == ALERT_NONE) continue;
        if (voice_link_active()) continue;   /* 对话会话中告警语音让路 */

        alert_rule_t *r = find_rule(cur);
        int64_t now = esp_timer_get_time();

        /* PULL_UP gets the whoop siren first, then voice, alternating via the wav itself */
        if (r->last_play == 0 || (now - r->last_play) > (int64_t)r->repeat_ms * 1000) {
            if (!audio_out_busy()) {
                if (audio_out_play(r->sound) == ESP_OK) {
                    r->last_play = now;
                    ESP_LOGI(TAG, "alert %d -> %s.wav", cur, r->sound);
                } else {
                    r->last_play = now; /* file missing: don't spam retries */
                }
            }
        }
    }
}

esp_err_t alert_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(alert_task, "alert", 3072, NULL, 4, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
