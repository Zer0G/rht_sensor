#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

typedef enum {
    STATS_HOUR = 1,
    STATS_DAY,
    STATS_WEEK,
    STATS_MONTH,
    STATS_YEAR,
} stats_kind_t;

typedef struct {
    time_t period_start;
    double temp_sum;
    double humidity_sum;
    uint32_t sample_count;
    float temp_min;
    float temp_max;
} stats_bucket_t;

typedef struct {
    uint32_t magic;
    stats_bucket_t hour;
    stats_bucket_t day;
    stats_bucket_t week;
    stats_bucket_t month;
    stats_bucket_t year;
    uint32_t flash_head;
    uint32_t next_sequence;
    bool flash_index_valid;
} stats_runtime_t;

esp_err_t stats_init(stats_runtime_t *runtime, time_t now);
esp_err_t stats_add_sample(stats_runtime_t *runtime, time_t timestamp,
                           float temperature_c, float humidity_pct);
void stats_today_extremes(const stats_runtime_t *runtime, float current,
                          float *minimum, float *maximum);
bool stats_delta_reference(const stats_runtime_t *runtime, uint8_t page,
                           time_t now, float current_temperature,
                           float *delta, const char **label);
uint8_t stats_chart_series(const stats_runtime_t *runtime, uint8_t page,
                           time_t now, float *values, uint8_t capacity);
