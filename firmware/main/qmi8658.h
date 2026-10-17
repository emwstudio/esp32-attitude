#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float ax, ay, az;   /* g */
    float gx, gy, gz;   /* deg/s */
    float temp;         /* deg C */
} qmi8658_data_t;

esp_err_t qmi8658_init(void);
bool qmi8658_present(void);
esp_err_t qmi8658_read(qmi8658_data_t *out);

#ifdef __cplusplus
}
#endif
