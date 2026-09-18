#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define BOARD_BUTTON_BOOT_GPIO  9
#define BOARD_BUTTON_POWER_GPIO 2

typedef struct {
    float temperature_c;
    float humidity_pct;
} board_sample_t;

esp_err_t board_init(void);
esp_err_t board_read_environment(board_sample_t *sample);
esp_err_t board_read_battery(float *voltage, uint8_t *percent);

bool board_rtc_get(time_t *utc);
esp_err_t board_rtc_set(time_t utc);

esp_err_t board_epaper_power(bool enabled);
esp_err_t board_battery_measure_power(bool enabled);
void board_prepare_for_sleep(void);

