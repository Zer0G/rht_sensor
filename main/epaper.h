#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define EPAPER_CHART_MAX_POINTS 31

typedef struct {
    time_t timestamp;
    float temperature_c;
    float humidity_pct;
    float dew_point_c;
    float day_min_c;
    float day_max_c;
    float delta_c;
    float battery_v;
    uint8_t battery_pct;
    int8_t rssi;
    const char *delta_label;
    float chart_values[EPAPER_CHART_MAX_POINTS];
    uint8_t chart_count;
    bool delta_valid;
    bool time_valid;
    bool wifi_connected;
} epaper_view_t;

esp_err_t epaper_show(const epaper_view_t *view);
esp_err_t epaper_show_provisioning(const char *ap_ssid, const char *ap_password);
void epaper_shutdown(void);
