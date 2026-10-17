#include "qmi8658.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

#define QMI8658_ADDR        0x6B    /* SA0 low -> 0x6A, high -> 0x6B; probed */
#define QMI8658_ADDR_ALT    0x6A

#define REG_WHO_AM_I   0x00
#define REG_CTRL1      0x02
#define REG_CTRL2      0x03
#define REG_CTRL3      0x04
#define REG_CTRL7      0x08
#define REG_RESET      0x60
#define REG_TEMP_L     0x33
#define REG_AX_L       0x35

/* CTRL2: aFs | aODR ; CTRL3: gFs | gODR */
#define ACC_FS_4G      (0x01 << 4)   /* 8192 LSB/g */
#define ACC_ODR_250HZ  0x06
#define GYR_FS_512DPS  (0x05 << 4)   /* 64 LSB/dps */
#define GYR_ODR_250HZ  0x06

#define ACC_LSB_PER_G   8192.0f
#define GYR_LSB_PER_DPS 64.0f

static const char *TAG = "qmi8658";
static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t probe_addr(i2c_master_bus_handle_t bus, uint8_t addr, i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, dev);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t who = 0;
    ret = i2c_master_transmit_receive(*dev, (uint8_t[]){REG_WHO_AM_I}, 1, &who, 1, 100);
    if (ret != ESP_OK || who != 0x05) {
        i2c_master_bus_rm_device(*dev);
        *dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "QMI8658 found at 0x%02x (WHO_AM_I=0x%02x)", addr, who);
    return ESP_OK;
}

esp_err_t qmi8658_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init failed");
        bus = bsp_i2c_get_handle();
    }
    ESP_RETURN_ON_FALSE(bus, ESP_FAIL, TAG, "no i2c bus");

    esp_err_t ret = probe_addr(bus, QMI8658_ADDR, &s_dev);
    if (ret != ESP_OK) {
        ret = probe_addr(bus, QMI8658_ADDR_ALT, &s_dev);
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "QMI8658 not found");

    write_reg(REG_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL1, 0x60), TAG, "ctrl1");   /* INT push-pull, big-endian off */
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL2, ACC_FS_4G | ACC_ODR_250HZ), TAG, "ctrl2");
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL3, GYR_FS_512DPS | GYR_ODR_250HZ), TAG, "ctrl3");
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL7, 0x03), TAG, "ctrl7");    /* enable acc + gyro */

    return ESP_OK;
}

bool qmi8658_present(void)
{
    return s_dev != NULL;
}

esp_err_t qmi8658_read(qmi8658_data_t *out)
{
    uint8_t buf[14]; /* temp(2) + acc(6) + gyro(6) from 0x33 */
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not init");
    ESP_RETURN_ON_ERROR(read_regs(REG_TEMP_L, buf, sizeof(buf)), TAG, "read");

    int16_t t  = (int16_t)(buf[1] << 8 | buf[0]);
    int16_t ax = (int16_t)(buf[3] << 8 | buf[2]);
    int16_t ay = (int16_t)(buf[5] << 8 | buf[4]);
    int16_t az = (int16_t)(buf[7] << 8 | buf[6]);
    int16_t gx = (int16_t)(buf[9] << 8 | buf[8]);
    int16_t gy = (int16_t)(buf[11] << 8 | buf[10]);
    int16_t gz = (int16_t)(buf[13] << 8 | buf[12]);

    out->temp = t / 256.0f;
    out->ax = ax / ACC_LSB_PER_G;
    out->ay = ay / ACC_LSB_PER_G;
    out->az = az / ACC_LSB_PER_G;
    out->gx = gx / GYR_LSB_PER_DPS;
    out->gy = gy / GYR_LSB_PER_DPS;
    out->gz = gz / GYR_LSB_PER_DPS;
    return ESP_OK;
}
