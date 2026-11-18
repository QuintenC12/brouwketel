#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t pin_sck;
    gpio_num_t pin_mosi;
    gpio_num_t pin_miso;
    gpio_num_t pin_cs;
    float rtd_nominal_ohms;
    float reference_resistor_ohms;
    float temperature_offset_c;
    bool filter_50hz;
} max31865_config_t;

typedef struct {
    spi_device_handle_t spi_device;
    float rtd_nominal_ohms;
    float reference_resistor_ohms;
    float temperature_offset_c;
    spi_host_device_t spi_host;
    bool initialized;
} max31865_t;

esp_err_t max31865_init(max31865_t *sensor, const max31865_config_t *config);
esp_err_t max31865_deinit(max31865_t *sensor);
esp_err_t max31865_read_temperature(max31865_t *sensor, float *temperature_c);
