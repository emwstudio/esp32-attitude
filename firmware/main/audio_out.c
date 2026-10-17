#include "audio_out.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

static const char *TAG = "audio";

/* 极简 WAV 流式播放器：不用 esp-audio-player（helix 解码器吃内部 RAM，
   会把 SPI 刷屏需要的 DMA 弹跳缓冲挤爆）。队列化播放，互不抢占。 */
#define PLAY_QUEUE_LEN 4
#define STREAM_BUF_SIZE 2048

static QueueHandle_t s_play_q;
static volatile bool s_busy;
static audio_done_cb_t s_done_cb;

/* 流式 PCM 字节环形缓冲：rx 任务写入（满则背压短等），audio 任务连续读出写 codec。
   旧实现是"环形区+描述符队列"，网络快于播放时写指针绕回覆盖未播块，
   新旧描述符指向同一段内存 → 同一段音频播两遍（句中重复单词的根因） */
#define STREAM_RING_BYTES (192 * 1024)
static uint8_t *s_stream_ring;
static volatile size_t s_ring_r, s_ring_w;   /* 单调递增游标，取模寻址 */
static volatile bool s_stream_dirty;         /* 曾经背压丢过数据（日志用） */

static size_t stream_used(void) { return s_ring_w - s_ring_r; }

typedef struct {
    char name[32];
} play_req_t;

static bool resolve(const char *name, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/voice/%s.wav", BSP_SD_MOUNT_POINT, name);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return true; }

    snprintf(out, out_len, "%s/voice/%s.wav", BSP_SPIFFS_MOUNT_POINT, name);
    f = fopen(out, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static void play_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return;

    /* 跳过 WAV 头（管线产物为 16kHz 16bit mono，与 codec 默认格式一致） */
    uint8_t *buf = malloc(STREAM_BUF_SIZE);
    if (!buf) {
        fclose(fp);
        return;
    }

    size_t n = fread(buf, 1, 64, fp);   /* 读头找 data 块 */
    size_t data_off = 44;
    if (n >= 44 && memcmp(buf, "RIFF", 4) == 0) {
        for (size_t i = 12; i + 8 <= n; ) {
            uint32_t sz;
            memcpy(&sz, buf + i + 4, 4);
            if (memcmp(buf + i, "data", 4) == 0) {
                data_off = i + 8;
                break;
            }
            i += 8 + ((sz + 1) & ~1u);
        }
        fseek(fp, data_off, SEEK_SET);
    } else {
        fseek(fp, 44, SEEK_SET);
    }

    ESP_LOGI(TAG, "play %s", path);
    while ((n = fread(buf, 1, STREAM_BUF_SIZE, fp)) > 0) {
        size_t written = 0;
        bsp_extra_i2s_write(buf, n, &written, portMAX_DELAY);
    }
    free(buf);
    fclose(fp);
}

static void audio_task(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(STREAM_BUF_SIZE);
    while (1) {
        /* 优先排干流式环（语音回复） */
        if (stream_used() > 0) {
            s_busy = true;
            size_t n = stream_used();
            if (n > STREAM_BUF_SIZE) n = STREAM_BUF_SIZE;
            size_t off = s_ring_r % STREAM_RING_BYTES;
            size_t first = STREAM_RING_BYTES - off;
            if (first > n) first = n;
            memcpy(buf, s_stream_ring + off, first);
            if (n > first) memcpy(buf + first, s_stream_ring, n - first);
            size_t written = 0;
            bsp_extra_i2s_write(buf, n, &written, portMAX_DELAY);
            s_ring_r += written ? written : n;
            continue;
        }
        play_req_t req;
        if (xQueueReceive(s_play_q, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
            s_busy = false;
            continue;
        }
        char path[128];
        if (resolve(req.name, path, sizeof(path))) {
            s_busy = true;
            play_file(path);
            s_busy = false;
        } else {
            ESP_LOGW(TAG, "no audio file for '%s'", req.name);
        }
        if (s_done_cb) s_done_cb();
    }
}

/* 流式写入：拷进字节环（允许跨尾折返）；空间不足时背压等待（天然限速到播放速率），
   超时仍未腾出则放弃本次剩余写入（保实时性，不再覆盖未播数据） */
esp_err_t audio_stream_write(const uint8_t *data, size_t len)
{
    if (!s_stream_ring) return ESP_ERR_INVALID_STATE;
    if (len == 0 || len > STREAM_RING_BYTES / 2) return ESP_ERR_INVALID_ARG;
    int waited = 0;
    while (STREAM_RING_BYTES - stream_used() < len) {
        if (waited >= 3000) {   /* 播放卡死兜底：丢这块也不能覆盖未播数据 */
            s_stream_dirty = true;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    size_t off = s_ring_w % STREAM_RING_BYTES;
    size_t first = STREAM_RING_BYTES - off;
    if (first > len) first = len;
    memcpy(s_stream_ring + off, data, first);
    if (len > first) memcpy(s_stream_ring, data + first, len - first);
    s_ring_w += len;
    return ESP_OK;
}

bool audio_stream_idle(void)
{
    return stream_used() == 0;
}

esp_err_t audio_out_init(void)
{
    esp_err_t ret = bsp_extra_codec_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec init failed");
        return ret;
    }
    /* 管线产物为 16kHz 16bit 单声道；codec 默认双声道，不改会两倍速变声 */
    bsp_extra_codec_set_fs(16000, 16, 1);
    bsp_extra_codec_volume_set(CODEC_DEFAULT_VOLUME, NULL);

    s_play_q = xQueueCreate(PLAY_QUEUE_LEN, sizeof(play_req_t));
    if (!s_play_q) return ESP_ERR_NO_MEM;
    s_stream_ring = heap_caps_malloc(STREAM_RING_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_stream_ring) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 3072, NULL, 4, NULL, 1);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t audio_out_play(const char *name)
{
    if (!s_play_q) return ESP_ERR_INVALID_STATE;
    play_req_t req;
    strncpy(req.name, name, sizeof(req.name) - 1);
    req.name[sizeof(req.name) - 1] = '\0';
    if (xQueueSend(s_play_q, &req, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;   /* 队列满，丢弃 */
    }
    return ESP_OK;
}

bool audio_out_busy(void)
{
    return s_busy;
}

void audio_out_set_done_cb(audio_done_cb_t cb)
{
    s_done_cb = cb;
}
