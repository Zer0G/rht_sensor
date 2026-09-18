#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

typedef struct {
    time_t timestamp;
    float temperature_c;
    float humidity_pct;
    float dew_point_c;
    float day_min_c;
    float day_max_c;
    float battery_v;
    uint8_t battery_pct;
    int8_t rssi;
} network_measurement_t;

esp_err_t network_connect(int8_t *rssi);
esp_err_t network_sync_time(void);
esp_err_t network_publish(const network_measurement_t *measurement, bool send_discovery);
void network_disconnect(void);
bool network_is_configured(void);
