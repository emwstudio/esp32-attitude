#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*audio_done_cb_t)(void);

esp_err_t audio_out_init(void);

/* Queue a WAV by bare name ("pull_up"); resolved against /sdcard/voice then /spiffs/voice.
   Returns ESP_ERR_NOT_FOUND if no file, ESP_ERR_INVALID_STATE if busy (caller decides to drop/wait). */
esp_err_t audio_out_play(const char *name);

bool audio_out_busy(void);

/* 流式 PCM 写入（16kHz 16bit mono 裸 PCM），块拷贝进 PSRAM 环形缓冲，
   由 audio 任务统一串行写 codec（esp_codec_dev_write 非线程安全，禁止旁路直写） */
esp_err_t audio_stream_write(const uint8_t *data, size_t len);

/* 流式队列是否已排空（全部 chunk 写完 codec；DMA 尾巴另计） */
bool audio_stream_idle(void);

/* Called from player callback when playback returns to idle */
void audio_out_set_done_cb(audio_done_cb_t cb);

#ifdef __cplusplus
}
#endif
