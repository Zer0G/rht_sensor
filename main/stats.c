#include "stats.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"

#define STATS_RUNTIME_MAGIC 0x52544832U
#define STATS_RECORD_MAGIC  0x52544852U
#define STATS_LEGACY_VERSION 1U
#define STATS_VERSION        2U
#define STATS_PARTITION      "stats"
#define STATS_NVS_NAMESPACE  "rht_stats"
#define STATS_MIGRATION_KEY  "temp_mig"
#define STATS_MIGRATION_VERSION 1U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sequence;
    uint32_t period_start;
    uint32_t sample_count;
    uint8_t kind;
    uint8_t version;
    uint16_t battery_avg_mv;
    int16_t temp_avg_centi;
    int16_t humidity_avg_centi;
    int16_t temp_min_centi;
    int16_t temp_max_centi;
    uint32_t crc32;
} stats_record_t;

_Static_assert(sizeof(stats_record_t) == 32, "flash record must be 32 bytes");

static const char *TAG = "stats";
static const esp_partition_t *s_partition;

static uint32_t crc32_bytes(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static bool record_valid(const stats_record_t *record)
{
    if (record->magic != STATS_RECORD_MAGIC ||
        (record->version != STATS_LEGACY_VERSION && record->version != STATS_VERSION) ||
        record->kind < STATS_HOUR || record->kind > STATS_YEAR || record->sample_count == 0) {
        return false;
    }
    stats_record_t copy = *record;
    const uint32_t expected = copy.crc32;
    copy.crc32 = 0;
    return crc32_bytes(&copy, sizeof(copy)) == expected;
}

static time_t period_start(time_t timestamp, stats_kind_t kind)
{
    struct tm local;
    localtime_r(&timestamp, &local);
    local.tm_sec = 0;
    if (kind >= STATS_HOUR) {
        local.tm_min = 0;
    }
    if (kind >= STATS_DAY) {
        local.tm_hour = 0;
    }
    if (kind == STATS_WEEK) {
        local.tm_mday -= (local.tm_wday + 6) % 7; /* Monday */
    } else if (kind == STATS_MONTH) {
        local.tm_mday = 1;
    } else if (kind == STATS_YEAR) {
        local.tm_mon = 0;
        local.tm_mday = 1;
    }
    local.tm_isdst = -1;
    return mktime(&local);
}

static void bucket_reset(stats_bucket_t *bucket, time_t start)
{
    memset(bucket, 0, sizeof(*bucket));
    bucket->period_start = start;
    bucket->temp_min = INFINITY;
    bucket->temp_max = -INFINITY;
}

static void bucket_add(stats_bucket_t *bucket, float temperature, float humidity,
                       float battery_mv, uint32_t count, float minimum, float maximum)
{
    bucket->temp_sum += (double)temperature * count;
    bucket->humidity_sum += (double)humidity * count;
    if (battery_mv > 0) bucket->battery_mv_sum += (double)battery_mv * count;
    bucket->sample_count += count;
    bucket->temp_min = fminf(bucket->temp_min, minimum);
    bucket->temp_max = fmaxf(bucket->temp_max, maximum);
}

static stats_record_t record_from_bucket(const stats_bucket_t *bucket, stats_kind_t kind,
                                         uint32_t sequence)
{
    stats_record_t record = {
        .magic = STATS_RECORD_MAGIC,
        .sequence = sequence,
        .period_start = (uint32_t)bucket->period_start,
        .sample_count = bucket->sample_count,
        .kind = (uint8_t)kind,
        .version = STATS_VERSION,
        .temp_avg_centi = (int16_t)lround(bucket->temp_sum * 100.0 / bucket->sample_count),
        .humidity_avg_centi = (int16_t)lround(bucket->humidity_sum * 100.0 / bucket->sample_count),
        .battery_avg_mv = bucket->battery_mv_sum > 0 ?
            (uint16_t)lround(bucket->battery_mv_sum / bucket->sample_count) : 0,
        .temp_min_centi = (int16_t)lroundf(bucket->temp_min * 100.0f),
        .temp_max_centi = (int16_t)lroundf(bucket->temp_max * 100.0f),
    };
    record.crc32 = crc32_bytes(&record, sizeof(record));
    return record;
}

static float record_temperature(const stats_record_t *record)
{
    return (float)record->temp_avg_centi / 100.0f;
}

static int16_t add_centi_clamped(int16_t value, int16_t delta)
{
    const int32_t adjusted = (int32_t)value + delta;
    if (adjusted > INT16_MAX) return INT16_MAX;
    if (adjusted < INT16_MIN) return INT16_MIN;
    return (int16_t)adjusted;
}

esp_err_t stats_migrate_temperature_offset(int16_t delta_centi)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(STATS_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open stats migration NVS");
    uint8_t migration_version = 0;
    if (nvs_get_u8(nvs, STATS_MIGRATION_KEY, &migration_version) == ESP_OK &&
        migration_version >= STATS_MIGRATION_VERSION) {
        nvs_close(nvs);
        return ESP_OK;
    }

    if (!s_partition) {
        s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40,
                                               STATS_PARTITION);
    }
    if (!s_partition) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *sector = malloc(s_partition->erase_size);
    if (!sector) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_OK;
    uint32_t migrated = 0;
    for (size_t sector_offset = 0; sector_offset < s_partition->size;
         sector_offset += s_partition->erase_size) {
        result = esp_partition_read(s_partition, sector_offset, sector,
                                    s_partition->erase_size);
        if (result != ESP_OK) break;

        bool changed = false;
        for (size_t record_offset = 0;
             record_offset + sizeof(stats_record_t) <= s_partition->erase_size;
             record_offset += sizeof(stats_record_t)) {
            stats_record_t record;
            memcpy(&record, sector + record_offset, sizeof(record));
            if (record.version != STATS_LEGACY_VERSION || !record_valid(&record)) {
                continue;
            }
            record.temp_avg_centi = add_centi_clamped(record.temp_avg_centi, delta_centi);
            record.temp_min_centi = add_centi_clamped(record.temp_min_centi, delta_centi);
            record.temp_max_centi = add_centi_clamped(record.temp_max_centi, delta_centi);
            record.version = STATS_VERSION;
            record.crc32 = 0;
            record.crc32 = crc32_bytes(&record, sizeof(record));
            memcpy(sector + record_offset, &record, sizeof(record));
            changed = true;
            ++migrated;
        }
        if (!changed) continue;

        result = esp_partition_erase_range(s_partition, sector_offset,
                                           s_partition->erase_size);
        if (result != ESP_OK) break;
        result = esp_partition_write(s_partition, sector_offset, sector,
                                     s_partition->erase_size);
        if (result != ESP_OK) break;
    }
    free(sector);

    if (result == ESP_OK) {
        result = nvs_set_u8(nvs, STATS_MIGRATION_KEY, STATS_MIGRATION_VERSION);
        if (result == ESP_OK) result = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "temperature history migration: +%.2f C applied to %lu records",
                 (double)delta_centi / 100.0, (unsigned long)migrated);
    }
    return result;
}

static uint32_t stored_record_count(const stats_runtime_t *runtime, uint32_t capacity)
{
    if (!runtime || runtime->next_sequence <= 1U) {
        return 0;
    }
    const uint32_t written = runtime->next_sequence - 1U;
    return written < capacity ? written : capacity;
}

static esp_err_t storage_append(stats_runtime_t *runtime, const stats_bucket_t *bucket,
                                stats_kind_t kind)
{
    const uint32_t capacity = s_partition->size / sizeof(stats_record_t);
    if (capacity == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t slot = runtime->flash_head % capacity;
    const size_t offset = (size_t)slot * sizeof(stats_record_t);
    uint32_t marker = 0;
    ESP_RETURN_ON_ERROR(esp_partition_read(s_partition, offset, &marker, sizeof(marker)), TAG, "read slot");
    if (marker != UINT32_MAX) {
        if ((offset % s_partition->erase_size) != 0) {
            ESP_LOGE(TAG, "unexpected occupied log slot at %u", (unsigned)slot);
            return ESP_ERR_INVALID_STATE;
        }
        ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_partition, offset, s_partition->erase_size),
                            TAG, "erase log sector");
    }

    stats_record_t record = record_from_bucket(bucket, kind, runtime->next_sequence++);
    ESP_RETURN_ON_ERROR(esp_partition_write(s_partition, offset, &record, sizeof(record)), TAG, "append record");
    runtime->flash_head = (slot + 1U) % capacity;
    ESP_LOGI(TAG, "stored kind=%u start=%lu samples=%lu", (unsigned)kind,
             (unsigned long)record.period_start, (unsigned long)record.sample_count);
    return ESP_OK;
}

static void reconstruct_add(stats_bucket_t *bucket, const stats_record_t *record)
{
    bucket_add(bucket, record_temperature(record), (float)record->humidity_avg_centi / 100.0f,
               (float)record->battery_avg_mv, record->sample_count,
               (float)record->temp_min_centi / 100.0f,
               (float)record->temp_max_centi / 100.0f);
}

static esp_err_t storage_scan(stats_runtime_t *runtime, time_t now, bool reconstruct)
{
    const uint32_t capacity = s_partition->size / sizeof(stats_record_t);
    uint32_t newest_sequence = 0;
    uint32_t newest_slot = capacity - 1U;
    bool found = false;

    for (uint32_t slot = 0; slot < capacity; ++slot) {
        stats_record_t record;
        ESP_RETURN_ON_ERROR(esp_partition_read(s_partition, (size_t)slot * sizeof(record),
                                               &record, sizeof(record)), TAG, "scan log");
        if (!record_valid(&record)) {
            continue;
        }
        if (!found || record.sequence > newest_sequence) {
            found = true;
            newest_sequence = record.sequence;
            newest_slot = slot;
        }
        if (!reconstruct || record.kind != STATS_HOUR) {
            continue;
        }
        const time_t record_time = (time_t)record.period_start;
        if (period_start(record_time, STATS_DAY) == runtime->day.period_start) {
            reconstruct_add(&runtime->day, &record);
        }
        if (period_start(record_time, STATS_WEEK) == runtime->week.period_start) {
            reconstruct_add(&runtime->week, &record);
        }
        if (period_start(record_time, STATS_MONTH) == runtime->month.period_start) {
            reconstruct_add(&runtime->month, &record);
        }
        if (period_start(record_time, STATS_YEAR) == runtime->year.period_start) {
            reconstruct_add(&runtime->year, &record);
        }
    }
    runtime->flash_head = found ? (newest_slot + 1U) % capacity : 0;
    runtime->next_sequence = found ? newest_sequence + 1U : 1U;
    runtime->flash_index_valid = true;
    ESP_LOGI(TAG, "log records=%lu next_slot=%lu", (unsigned long)capacity,
             (unsigned long)runtime->flash_head);
    (void)now;
    return ESP_OK;
}

static esp_err_t accept_hour_for_rollup(stats_runtime_t *runtime, const stats_record_t *hour)
{
    stats_bucket_t *buckets[] = {&runtime->day, &runtime->week, &runtime->month, &runtime->year};
    const stats_kind_t kinds[] = {STATS_DAY, STATS_WEEK, STATS_MONTH, STATS_YEAR};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        const time_t expected = period_start((time_t)hour->period_start, kinds[i]);
        if (buckets[i]->sample_count && buckets[i]->period_start != expected) {
            ESP_RETURN_ON_ERROR(storage_append(runtime, buckets[i], kinds[i]), TAG, "rollup append");
            bucket_reset(buckets[i], expected);
        } else if (!buckets[i]->sample_count) {
            buckets[i]->period_start = expected;
        }
        reconstruct_add(buckets[i], hour);
    }
    return ESP_OK;
}

static esp_err_t advance_rollups(stats_runtime_t *runtime, time_t timestamp)
{
    stats_bucket_t *buckets[] = {&runtime->day, &runtime->week, &runtime->month, &runtime->year};
    const stats_kind_t kinds[] = {STATS_DAY, STATS_WEEK, STATS_MONTH, STATS_YEAR};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        const time_t expected = period_start(timestamp, kinds[i]);
        if (buckets[i]->period_start != expected) {
            if (buckets[i]->sample_count) {
                ESP_RETURN_ON_ERROR(storage_append(runtime, buckets[i], kinds[i]), TAG, "close rollup");
            }
            bucket_reset(buckets[i], expected);
        }
    }
    return ESP_OK;
}

esp_err_t stats_init(stats_runtime_t *runtime, time_t now)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_partition) {
        s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, STATS_PARTITION);
    }
    if (!s_partition) {
        ESP_LOGE(TAG, "partition '%s' not found", STATS_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }
    if (runtime->magic == STATS_RUNTIME_MAGIC && runtime->flash_index_valid) {
        return ESP_OK;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->magic = STATS_RUNTIME_MAGIC;
    bucket_reset(&runtime->hour, period_start(now, STATS_HOUR));
    bucket_reset(&runtime->day, period_start(now, STATS_DAY));
    bucket_reset(&runtime->week, period_start(now, STATS_WEEK));
    bucket_reset(&runtime->month, period_start(now, STATS_MONTH));
    bucket_reset(&runtime->year, period_start(now, STATS_YEAR));
    return storage_scan(runtime, now, true);
}

esp_err_t stats_add_sample_with_battery(stats_runtime_t *runtime, time_t timestamp,
                                        float temperature_c, float humidity_pct,
                                        float battery_mv)
{
    if (!runtime || !isfinite(temperature_c) || !isfinite(humidity_pct)) {
        return ESP_ERR_INVALID_ARG;
    }
    const time_t sample_hour = period_start(timestamp, STATS_HOUR);
    if (runtime->hour.period_start != sample_hour) {
        if (runtime->hour.sample_count) {
            stats_record_t hour = record_from_bucket(&runtime->hour, STATS_HOUR,
                                                     runtime->next_sequence);
            ESP_RETURN_ON_ERROR(storage_append(runtime, &runtime->hour, STATS_HOUR), TAG, "hour append");
            ESP_RETURN_ON_ERROR(accept_hour_for_rollup(runtime, &hour), TAG, "hour rollup");
        }
        ESP_RETURN_ON_ERROR(advance_rollups(runtime, timestamp), TAG, "advance periods");
        bucket_reset(&runtime->hour, sample_hour);
    }
    bucket_add(&runtime->hour, temperature_c, humidity_pct, battery_mv, 1,
               temperature_c, temperature_c);
    return ESP_OK;
}

esp_err_t stats_add_sample(stats_runtime_t *runtime, time_t timestamp,
                           float temperature_c, float humidity_pct)
{
    return stats_add_sample_with_battery(runtime, timestamp, temperature_c,
                                         humidity_pct, 0);
}

void stats_today_extremes(const stats_runtime_t *runtime, float current,
                          float *minimum, float *maximum)
{
    float low = current;
    float high = current;
    if (runtime) {
        if (runtime->day.sample_count) {
            low = fminf(low, runtime->day.temp_min);
            high = fmaxf(high, runtime->day.temp_max);
        }
        if (runtime->hour.sample_count) {
            low = fminf(low, runtime->hour.temp_min);
            high = fmaxf(high, runtime->hour.temp_max);
        }
    }
    if (minimum) {
        *minimum = low;
    }
    if (maximum) {
        *maximum = high;
    }
}

static bool storage_find(stats_kind_t kind, time_t exact_start, bool exact,
                         stats_record_t *result, const stats_runtime_t *runtime)
{
    if (!s_partition || !runtime || !result) {
        return false;
    }
    const uint32_t capacity = s_partition->size / sizeof(stats_record_t);
    const uint32_t record_count = stored_record_count(runtime, capacity);
    for (uint32_t age = 0; age < record_count; ++age) {
        const uint32_t slot = (runtime->flash_head + capacity - 1U - age) % capacity;
        stats_record_t record;
        if (esp_partition_read(s_partition, (size_t)slot * sizeof(record), &record, sizeof(record)) != ESP_OK) {
            return false;
        }
        if (!record_valid(&record) || record.kind != kind) {
            continue;
        }
        if (!exact || record.period_start == (uint32_t)exact_start) {
            *result = record;
            return true;
        }
    }
    return false;
}

static time_t previous_period_start(time_t now, stats_kind_t kind)
{
    const time_t current_start = period_start(now, kind);
    struct tm previous;
    localtime_r(&current_start, &previous);
    if (kind == STATS_WEEK) {
        previous.tm_mday -= 7;
    } else if (kind == STATS_MONTH) {
        previous.tm_mon -= 1;
    } else if (kind == STATS_YEAR) {
        previous.tm_year -= 1;
    }
    previous.tm_isdst = -1;
    return mktime(&previous);
}

bool stats_delta_reference(const stats_runtime_t *runtime, uint8_t page,
                           time_t now, float current_temperature,
                           float *delta, const char **label)
{
    if (!runtime || !delta || !label) {
        return false;
    }
    page %= 5;
    stats_record_t record;
    float reference = 0;
    bool found = false;
    bool record_found = false;

    if (page == 0) {
        *label = "PREV HOUR";
        struct tm previous;
        localtime_r(&now, &previous);
        previous.tm_hour -= 1;
        previous.tm_min = 0;
        previous.tm_sec = 0;
        previous.tm_isdst = -1;
        const time_t target = mktime(&previous);
        record_found = storage_find(STATS_HOUR, target, true, &record, runtime);
        found = record_found;
    } else if (page == 1) {
        *label = "YESTERDAY";
        struct tm previous;
        localtime_r(&now, &previous);
        previous.tm_mday -= 1;
        previous.tm_min = 0;
        previous.tm_sec = 0;
        previous.tm_isdst = -1;
        record_found = storage_find(STATS_HOUR, mktime(&previous), true, &record, runtime);
        found = record_found;
    } else {
        struct tm previous;
        localtime_r(&now, &previous);
        previous.tm_min = 0;
        previous.tm_sec = 0;
        previous.tm_isdst = -1;
        if (page == 2) {
            *label = "PREV WEEK";
            previous.tm_mday -= 7;
        } else if (page == 3) {
            *label = "PREV MONTH";
            previous.tm_mon -= 1;
        } else {
            *label = "PREV YEAR";
            previous.tm_year -= 1;
        }
        const time_t target = mktime(&previous);
        record_found = storage_find(STATS_HOUR, target, true, &record, runtime);
        found = record_found;
    }
    if (record_found) {
        reference = record_temperature(&record);
    }
    if (found) {
        *delta = current_temperature - reference;
        ESP_LOGI(TAG, "delta page=%u reference=%.2f current=%.2f delta=%+.2f",
                 (unsigned)page, reference, current_temperature, *delta);
    } else {
        ESP_LOGI(TAG, "delta page=%u: no completed reference period", (unsigned)page);
    }
    return found;
}

uint8_t stats_chart_series(const stats_runtime_t *runtime, uint8_t page,
                           time_t now, float *values, uint8_t capacity)
{
    if (!runtime || !values || !capacity || !s_partition) {
        return 0;
    }

    page %= 5;
    stats_kind_t kind = STATS_HOUR;
    time_t range_end = period_start(now, STATS_HOUR);
    time_t range_start = range_end;
    uint8_t limit = 24;
    struct tm start_tm;

    if (page == 0) {
        localtime_r(&range_end, &start_tm);
        start_tm.tm_hour -= 24;
        start_tm.tm_isdst = -1;
        range_start = mktime(&start_tm);
    } else if (page == 1) {
        kind = STATS_DAY;
        limit = 7;
        range_end = period_start(now, STATS_DAY);
        localtime_r(&range_end, &start_tm);
        start_tm.tm_mday -= 7;
        start_tm.tm_isdst = -1;
        range_start = mktime(&start_tm);
    } else if (page == 2) {
        kind = STATS_DAY;
        limit = 7;
        range_end = period_start(now, STATS_WEEK);
        range_start = previous_period_start(now, STATS_WEEK);
    } else if (page == 3) {
        kind = STATS_DAY;
        limit = 31;
        range_end = period_start(now, STATS_MONTH);
        range_start = previous_period_start(now, STATS_MONTH);
    } else {
        kind = STATS_MONTH;
        limit = 12;
        range_end = period_start(now, STATS_YEAR);
        range_start = previous_period_start(now, STATS_YEAR);
    }

    if (limit > capacity) {
        limit = capacity;
    }
    const uint32_t flash_capacity = s_partition->size / sizeof(stats_record_t);
    const uint32_t record_count = stored_record_count(runtime, flash_capacity);
    uint8_t count = 0;
    for (uint32_t age = 0; age < record_count && count < limit; ++age) {
        const uint32_t slot = (runtime->flash_head + flash_capacity - 1U - age) % flash_capacity;
        stats_record_t record;
        if (esp_partition_read(s_partition, (size_t)slot * sizeof(record),
                               &record, sizeof(record)) != ESP_OK) {
            break;
        }
        if (!record_valid(&record) || record.kind != kind) {
            continue;
        }
        const time_t record_start = (time_t)record.period_start;
        if (record_start >= range_start && record_start < range_end) {
            values[count++] = record_temperature(&record);
        }
    }

    for (uint8_t left = 0, right = count ? count - 1U : 0; left < right; ++left, --right) {
        const float temporary = values[left];
        values[left] = values[right];
        values[right] = temporary;
    }
    ESP_LOGI(TAG, "chart page=%u points=%u", (unsigned)page, (unsigned)count);
    return count;
}

uint8_t stats_battery_chart(const stats_runtime_t *runtime, time_t now,
                            float *values, uint8_t capacity)
{
    if (!runtime || !values || !capacity || !s_partition) return 0;
    const uint32_t flash_capacity = s_partition->size / sizeof(stats_record_t);
    const uint32_t record_count = stored_record_count(runtime, flash_capacity);
    struct tm start_tm;
    localtime_r(&now, &start_tm);
    start_tm.tm_hour = 0;
    start_tm.tm_min = 0;
    start_tm.tm_sec = 0;
    start_tm.tm_mday -= 30;
    start_tm.tm_isdst = -1;
    const time_t range_start = mktime(&start_tm);
    const time_t range_end = period_start(now, STATS_DAY);
    uint8_t count = 0;
    for (uint32_t age = 0; age < record_count && count < capacity; ++age) {
        const uint32_t slot = (runtime->flash_head + flash_capacity - 1U - age) % flash_capacity;
        stats_record_t record;
        if (esp_partition_read(s_partition, (size_t)slot * sizeof(record),
                               &record, sizeof(record)) != ESP_OK) break;
        if (!record_valid(&record) || record.kind != STATS_DAY ||
            record.battery_avg_mv == 0) continue;
        const time_t record_start = (time_t)record.period_start;
        if (record_start >= range_start && record_start < range_end) {
            values[count++] = (float)record.battery_avg_mv;
        }
    }
    for (uint8_t left = 0, right = count ? count - 1U : 0; left < right; ++left, --right) {
        const float temporary = values[left];
        values[left] = values[right];
        values[right] = temporary;
    }
    return count;
}
