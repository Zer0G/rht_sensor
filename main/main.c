#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "driver/gpio.h"
#include "epaper.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "stats.h"

#define STATE_MAGIC 0x52485435U
#define NTP_RETRY_SECONDS (15 * 60)
#define STARTUP_RETRY_SECONDS 60
#define DEEP_SLEEP_WAKE_MASK (1ULL << BOARD_BUTTON_POWER_GPIO)
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_POLL_MS 20

typedef struct {
    uint32_t magic;
    stats_runtime_t stats;
    float last_tx_temperature;
    time_t last_tx_hour;
    time_t last_ntp_sync;
    time_t last_ntp_attempt;
    time_t last_discovery;
    int8_t last_rssi;
    uint8_t page;
    bool has_transmitted;
} persistent_state_t;

RTC_DATA_ATTR static persistent_state_t s_state;
static const char *TAG = "rht_sensor";

static time_t hour_start(time_t timestamp)
{
    struct tm local;
    localtime_r(&timestamp, &local);
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return mktime(&local);
}

static float dew_point(float temperature, float humidity)
{
    const float a = 17.62f;
    const float b = 243.12f;
    humidity = fmaxf(0.1f, fminf(100.0f, humidity));
    const float gamma = logf(humidity / 100.0f) + a * temperature / (b + temperature);
    return b * gamma / (a - gamma);
}

static void init_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

static bool is_time_valid(time_t value)
{
    struct tm utc;
    gmtime_r(&value, &utc);
    return utc.tm_year + 1900 >= 2024;
}

static void rotate_page_for_wakeup(void)
{
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        s_state.page = (s_state.page + 1U) % 5U;
    }
}

static void refresh_delta_page(const epaper_view_t *base_view)
{
    if (!base_view) {
        return;
    }

    epaper_view_t view = *base_view;
    float delta = 0;
    const char *delta_label = "DELTA";
    view.delta_valid = view.time_valid &&
        stats_delta_reference(&s_state.stats, s_state.page, view.timestamp,
                              view.temperature_c, &delta, &delta_label);
    view.delta_c = delta;
    view.delta_label = delta_label;
    view.chart_count = stats_chart_series(&s_state.stats, s_state.page, view.timestamp,
                                          view.chart_values, EPAPER_CHART_MAX_POINTS);
    view.wifi_connected = false;

    ESP_LOGI(TAG, "GP9: delta page %u/5 (%s)",
             (unsigned)(s_state.page + 1U), delta_label);
    ESP_ERROR_CHECK_WITHOUT_ABORT(epaper_show(&view));
    epaper_shutdown();
}

static void wait_for_next_cycle(time_t now, const epaper_view_t *current_view)
{
    epaper_shutdown();
    network_disconnect();
    board_prepare_for_sleep();

    const uint32_t interval = CONFIG_RHT_SAMPLE_INTERVAL_SECONDS;
    uint32_t sleep_seconds = interval;
    if (is_time_valid(now)) {
        const uint32_t remainder = (uint32_t)(now % interval);
        sleep_seconds = remainder ? interval - remainder : interval;
    }

#if CONFIG_RHT_DEEP_SLEEP
    /* Avoid an immediate level-triggered wake while the user still holds a key. */
    for (int i = 0; i < 30; ++i) {
        if (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) && gpio_get_level(BOARD_BUTTON_POWER_GPIO)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "deep sleep for %lu seconds", (unsigned long)sleep_seconds);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)sleep_seconds * 1000000ULL));
    /* ESP32-C6 only supports GPIO0..7 as deep-sleep GPIO wake sources.
     * POWER is GPIO2; BOOT/GPIO9 remains usable while the CPU is awake. */
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(DEEP_SLEEP_WAKE_MASK,
                                                    ESP_EXT1_WAKEUP_ANY_LOW));
    esp_deep_sleep_start();
#else
    ESP_LOGI(TAG, "development mode: staying awake for %lu seconds; GP9 changes delta page",
             (unsigned long)sleep_seconds);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(sleep_seconds * 1000U);
    bool button_armed = true;
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        const bool button_pressed = gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0;
        if (button_armed && button_pressed) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0) {
                s_state.page = (s_state.page + 1U) % 5U;
                refresh_delta_page(current_view);
                button_armed = false;
            }
        } else if (!button_pressed) {
            button_armed = true;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
    s_state.page = (s_state.page + 1U) % 5U;
#endif
}

static void enter_startup_failure_sleep(esp_err_t error)
{
    ESP_LOGE(TAG, "board initialization failed: %s", esp_err_to_name(error));
#if CONFIG_RHT_DEEP_SLEEP
    ESP_LOGE(TAG, "retrying in %d seconds; POWER can wake the board sooner",
             STARTUP_RETRY_SECONDS);
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_sleep_enable_timer_wakeup((uint64_t)STARTUP_RETRY_SECONDS * 1000000ULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_sleep_enable_ext1_wakeup_io(DEEP_SLEEP_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW));
    esp_deep_sleep_start();
#else
    ESP_LOGE(TAG, "deep sleep is disabled; USB remains active for diagnostics");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(STARTUP_RETRY_SECONDS * 1000));
    }
#endif
}

void app_main(void)
{
    if (s_state.magic != STATE_MAGIC) {
        memset(&s_state, 0, sizeof(s_state));
        s_state.magic = STATE_MAGIC;
        s_state.last_rssi = -127;
    }
    rotate_page_for_wakeup();
    setenv("TZ", CONFIG_RHT_TIMEZONE, 1);
    tzset();

    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_err_t board_result = board_init();
    if (board_result != ESP_OK) {
        enter_startup_failure_sleep(board_result);
        return;
    }

    while (true) {
    time_t now = 0;
    bool rtc_valid = board_rtc_get(&now);
    bool time_valid = rtc_valid && is_time_valid(now);

    /* Read before enabling the radio to avoid Wi-Fi self-heating biasing SHTC3. */
    board_sample_t sample;
    if (board_read_environment(&sample) != ESP_OK) {
        ESP_LOGE(TAG, "environment measurement failed");
        wait_for_next_cycle(now, NULL);
        continue;
    }
    float battery_voltage = 0;
    uint8_t battery_percent = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_read_battery(&battery_voltage, &battery_percent));

    bool wifi_connected = false;
    bool wifi_attempted = false;
    const bool ntp_resync_required = !time_valid || !is_time_valid(s_state.last_ntp_sync) ||
                                     now < s_state.last_ntp_sync ||
                                     now - s_state.last_ntp_sync >= CONFIG_RHT_NTP_RESYNC_HOURS * 3600;
    const bool ntp_retry_ready = !time_valid || !is_time_valid(s_state.last_ntp_attempt) ||
                                 now < s_state.last_ntp_attempt ||
                                 now - s_state.last_ntp_attempt >= NTP_RETRY_SECONDS;
    const bool ntp_due = ntp_resync_required && ntp_retry_ready;

    if (ntp_due && CONFIG_RHT_WIFI_SSID[0]) {
        wifi_attempted = true;
        if (time_valid) {
            s_state.last_ntp_attempt = now;
        }
        if (network_connect(&s_state.last_rssi) == ESP_OK) {
            wifi_connected = true;
            if (network_sync_time() == ESP_OK) {
                time(&now);
                time_valid = is_time_valid(now);
                if (time_valid) {
                    s_state.last_ntp_sync = now;
                    ESP_ERROR_CHECK_WITHOUT_ABORT(board_rtc_set(now));
                }
            }
        }
    }
    if (!time_valid) {
        time(&now);
        time_valid = is_time_valid(now);
    }

    if (time_valid) {
        ESP_ERROR_CHECK(stats_init(&s_state.stats, now));
        ESP_ERROR_CHECK_WITHOUT_ABORT(stats_add_sample(&s_state.stats, now,
                                                       sample.temperature_c, sample.humidity_pct));
    }
    float day_min = sample.temperature_c;
    float day_max = sample.temperature_c;
    if (time_valid) {
        stats_today_extremes(&s_state.stats, sample.temperature_c, &day_min, &day_max);
    }
    const float dew = dew_point(sample.temperature_c, sample.humidity_pct);

    const time_t current_hour = time_valid ? hour_start(now) : 0;
    const bool hour_trigger = time_valid &&
                              (!s_state.has_transmitted || s_state.last_tx_hour != current_hour);
    const bool delta_trigger = s_state.has_transmitted &&
                               fabsf(sample.temperature_c - s_state.last_tx_temperature) >
                               (float)CONFIG_RHT_TX_DELTA_C_X100 / 100.0f;
    const bool should_transmit = network_is_configured() && (hour_trigger || delta_trigger);

    if (should_transmit && !wifi_connected && !wifi_attempted) {
        wifi_attempted = true;
        wifi_connected = network_connect(&s_state.last_rssi) == ESP_OK;
    }
    if (should_transmit && wifi_connected) {
        const network_measurement_t measurement = {
            .timestamp = now,
            .temperature_c = sample.temperature_c,
            .humidity_pct = sample.humidity_pct,
            .dew_point_c = dew,
            .day_min_c = day_min,
            .day_max_c = day_max,
            .battery_v = battery_voltage,
            .battery_pct = battery_percent,
            .rssi = s_state.last_rssi,
        };
        const bool discovery_due = !is_time_valid(s_state.last_discovery) ||
                                   now < s_state.last_discovery ||
                                   now - s_state.last_discovery >= 24 * 3600;
        if (network_publish(&measurement, discovery_due) == ESP_OK) {
            s_state.has_transmitted = true;
            s_state.last_tx_temperature = sample.temperature_c;
            s_state.last_tx_hour = current_hour;
            if (discovery_due) {
                s_state.last_discovery = now;
            }
        } else {
            ESP_LOGW(TAG, "MQTT publish failed; it will be retried on the next wake");
        }
    }

    float delta = 0;
    const char *delta_label = "DELTA";
    const bool delta_valid = time_valid &&
        stats_delta_reference(&s_state.stats, s_state.page, now,
                              sample.temperature_c, &delta, &delta_label);
    epaper_view_t view = {
        .timestamp = now,
        .temperature_c = sample.temperature_c,
        .humidity_pct = sample.humidity_pct,
        .dew_point_c = dew,
        .day_min_c = day_min,
        .day_max_c = day_max,
        .delta_c = delta,
        .battery_v = battery_voltage,
        .battery_pct = battery_percent,
        .rssi = s_state.last_rssi,
        .delta_label = delta_label,
        .delta_valid = delta_valid,
        .time_valid = time_valid,
        .wifi_connected = wifi_connected,
    };
    if (time_valid) {
        view.chart_count = stats_chart_series(&s_state.stats, s_state.page, now,
                                              view.chart_values, EPAPER_CHART_MAX_POINTS);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(epaper_show(&view));

    time(&now);
    wait_for_next_cycle(now, &view);
    }
}
