#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 按住 KEY(GPIO18) 录音、松开发送到 Mac 语音管线，收到回复 PCM 后播放 */
esp_err_t voice_link_init(void);

/* 对话会话进行中（录音/等待/播报），告警语音应让路 */
bool voice_link_active(void);
