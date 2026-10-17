#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "qmi8658.h"
#include "flight_state.h"
#include "ui_ahrs.h"
#include "audio_out.h"
#include "alert.h"
#include "voice_link.h"

static const char *TAG = "app";
static volatile bool s_ui_ready;

/* BSP 2.0.1 的 bsp_display_lock 把 esp_err_t 当 bool 返回（0=成功），语义与注释相反 */
static bool ui_lock(uint32_t timeout_ms)
{
    return bsp_display_lock(timeout_ms) == 0;
}

static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ui task alive");
    while (!s_ui_ready) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "ui ready, entering loop");
    int lock_fail = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(66)); /* 15 fps */
        flight_state_t s;
        flight_state_snapshot(&s);
        if (ui_lock(200)) {
            ui_ahrs_update(&s);
            bsp_display_unlock();
            lock_fail = 0;
        } else {
            lock_fail++;
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    flight_state_init();

    /* 先初始化低速外设，再开显示（减少 I2C 总线竞争窗口） */
    if (qmi8658_init() != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 init failed, running demo sweep");
    }

    if (bsp_spiffs_mount() != ESP_OK) {
        ESP_LOGW(TAG, "spiffs mount failed (voice fallback unavailable)");
    }
    /* 不挂载 SD 卡：省 SDMMC/FATFS 的内部 RAM（语音全部走 SPIFFS） */

    if (audio_out_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio unavailable");
    }

    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }

    if (ui_lock(-1)) {
        esp_err_t ui_ret = ui_ahrs_init();
        bsp_display_unlock();
        ESP_LOGI(TAG, "ui_ahrs_init -> %s", esp_err_to_name(ui_ret));
        s_ui_ready = (ui_ret == ESP_OK);
    } else {
        ESP_LOGE(TAG, "display lock failed in app_main");
    }

    imu_task_start();
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL, 3, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed");
    }
    alert_task_start();
    if (voice_link_init() != ESP_OK) {
        ESP_LOGW(TAG, "voice link init failed");
    }

    audio_out_play("system_ready");

    ESP_LOGI(TAG, "esp32-attitude started");
}
