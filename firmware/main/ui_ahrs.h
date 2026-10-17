#pragma once

#include "esp_err.h"
#include "flight_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call with bsp_display_lock held */
esp_err_t ui_ahrs_init(void);
void ui_ahrs_update(const flight_state_t *s);

/* 对话状态点（右上角），可从任意任务调用（内部持锁）：0 隐藏 / 1 红=录音 / 2 黄=处理 / 3 绿=播报 */
void ui_ahrs_set_voice_state(int state);

#ifdef __cplusplus
}
#endif
