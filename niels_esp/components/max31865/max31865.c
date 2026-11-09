#include "max31865.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_CONFIG 0x00
#define REG_RTD_MSB 0x01
#define REG_FAULT_STATUS 0x07

#define CONFIG_BIAS 0x80
#define CONFIG_AUTO_CONVERT 0x40
#define CONFIG_FAULT_CLEAR 0x02
#define CONFIG_FILTER_50HZ 0x01

static const char *TAG = "max31865";

static esp_err_t write_register(max31865_t *sensor, uint8_t reg, uint8_t value)
{
    uint8_t tx_data[2] = {(uint8_t)(reg | 0x80U), value};
    spi_transaction_t transaction = {
        .length = sizeof(tx_data) * 8,
        .tx_buffer = tx_data,
    };
    return spi_device_transmit(sensor->spi_device, &transaction);
}

static esp_err_t read_register(max31865_t *sensor, uint8_t reg, uint8_t *value)
{
    uint8_t tx_data[2] = {(uint8_t)(reg & 0x7FU), 0};
    uint8_t rx_data[2] = {0};
    spi_transaction_t transaction = {
        .length = sizeof(tx_data) * 8,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };
    esp_err_t err = spi_device_transmit(sensor->spi_device, &transaction);
    if (err == ESP_OK) {
        *value = rx_data[1];
    }
    return err;
}

static esp_err_t read_rtd(max31865_t *sensor, uint16_t *raw_rtd)
{
    uint8_t tx_data[3] = {REG_RTD_MSB, 0, 0};
    uint8_t rx_data[3] = {0};
    spi_transaction_t transaction = {
        .length = sizeof(tx_data) * 8,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };
    esp_err_t err = spi_device_transmit(sensor->spi_device, &transaction);
    if (err == ESP_OK) {
        *raw_rtd = (uint16_t)(((uint16_t)rx_data[1] << 8) | rx_data[2]);
    }
    return err;
}

static esp_err_t clear_fault(max31865_t *sensor)
{
    uint8_t config;
    esp_err_t err = read_register(sensor, REG_CONFIG, &config);
    if (err != ESP_OK) {
        return err;
    }

    err = write_register(sensor, REG_CONFIG, (uint8_t)(config | CONFIG_FAULT_CLEAR));
    if (err != ESP_OK) {
        return err;
    }

    return write_register(sensor, REG_CONFIG, (uint8_t)(config & ~CONFIG_FAULT_CLEAR));
}

static void log_fault_status(uint8_t fault)
{
    ESP_LOGE(TAG, "MAX31865-foutstatus 0x%02X:", fault);
    if (fault & 0x80U) {
        ESP_LOGE(TAG, "RTD boven bovengrens");
    }
    if (fault & 0x40U) {
        ESP_LOGE(TAG, "RTD onder ondergrens");
    }
    if (fault & 0x20U) {
        ESP_LOGE(TAG, "REFIN- > 0.85 x bias");
    }
    if (fault & 0x10U) {
        ESP_LOGE(TAG, "REFIN- < 0.85 x bias (mogelijk FORCE- open)");
    }
    if (fault & 0x08U) {
        ESP_LOGE(TAG, "RTDIN- < 0.85 x bias (sensor-/bedradingsfout mogelijk)");
    }
    if (fault & 0x04U) {
        ESP_LOGE(TAG, "Over- of onderspanning");
    }
}

static esp_err_t check_fault(max31865_t *sensor, bool *has_fault)
{
    uint8_t fault;
    esp_err_t err = read_register(sensor, REG_FAULT_STATUS, &fault);
    if (err != ESP_OK) {
        return err;
    }

    *has_fault = fault != 0U;
    if (*has_fault) {
        log_fault_status(fault);
        return clear_fault(sensor);
    }
    return ESP_OK;
}

esp_err_t max31865_init(max31865_t *sensor, const max31865_config_t *config)
{
    if (sensor == NULL || config == NULL ||
        config->rtd_nominal_ohms <= 0.0f ||
        config->reference_resistor_ohms <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    *sensor = (max31865_t){0};
    spi_bus_config_t bus_config = {
        .mosi_io_num = config->pin_mosi,
        .miso_io_num = config->pin_miso,
        .sclk_io_num = config->pin_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 3,
    };
    esp_err_t err = spi_bus_initialize(config->spi_host, &bus_config, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 1000000,
        .mode = 1,
        .spics_io_num = config->pin_cs,
        .queue_size = 1,
    };
    err = spi_bus_add_device(config->spi_host, &device_config, &sensor->spi_device);
    if (err != ESP_OK) {
        (void)spi_bus_free(config->spi_host);
        return err;
    }

    sensor->spi_host = config->spi_host;
    sensor->rtd_nominal_ohms = config->rtd_nominal_ohms;
    sensor->reference_resistor_ohms = config->reference_resistor_ohms;
    sensor->temperature_offset_c = config->temperature_offset_c;
    sensor->initialized = true;

    uint8_t register_config = CONFIG_BIAS | CONFIG_AUTO_CONVERT;
    if (config->filter_50hz) {
        register_config |= CONFIG_FILTER_50HZ;
    }
    err = write_register(sensor, REG_CONFIG, register_config);
    if (err != ESP_OK) {
        (void)max31865_deinit(sensor);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t max31865_deinit(max31865_t *sensor)
{
    if (sensor == NULL || !sensor->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = spi_bus_remove_device(sensor->spi_device);
    if (err != ESP_OK) {
        return err;
    }
    err = spi_bus_free(sensor->spi_host);
    if (err == ESP_OK) {
        sensor->initialized = false;
        sensor->spi_device = NULL;
    }
    return err;
}

esp_err_t max31865_read_temperature(max31865_t *sensor, float *temperature_c)
{
    if (sensor == NULL || temperature_c == NULL || !sensor->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    bool has_fault;
    esp_err_t err = check_fault(sensor, &has_fault);
    if (err != ESP_OK) {
        return err;
    }
    if (has_fault) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t raw_rtd;
    err = read_rtd(sensor, &raw_rtd);
    if (err != ESP_OK) {
        return err;
    }
    if ((raw_rtd & 0x0001U) != 0U) {
        err = check_fault(sensor, &has_fault);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    const float resistance =
        ((float)(raw_rtd >> 1) * sensor->reference_resistor_ohms) / 32768.0f;
    const float a = 0.0039083f;
    const float b = -0.0000005775f;
    const float discriminant =
        (a * a) - (4.0f * b * (1.0f - resistance / sensor->rtd_nominal_ohms));
    if (discriminant < 0.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *temperature_c =
        ((-a + sqrtf(discriminant)) / (2.0f * b)) + sensor->temperature_offset_c;
    return isfinite(*temperature_c) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
