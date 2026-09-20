#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define NETWORK_HISTORY_COUNT      5
#define NETWORK_MQTT_URI_SIZE      128
#define NETWORK_MQTT_USERNAME_SIZE 65
#define NETWORK_MQTT_PASSWORD_SIZE 65

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
    float history_reference_c[NETWORK_HISTORY_COUNT];
    float history_delta_c[NETWORK_HISTORY_COUNT];
    bool history_valid[NETWORK_HISTORY_COUNT];
} network_measurement_t;

esp_err_t network_connect(int8_t *rssi);
esp_err_t network_sync_time(void);
esp_err_t network_publish(const network_measurement_t *measurement, bool send_discovery);
esp_err_t network_save_wifi_credentials(const char *ssid, const char *password);
esp_err_t network_save_mqtt_config(const char *uri, const char *username,
                                   const char *password);
esp_err_t network_clear_wifi_credentials(void);
esp_err_t network_clear_mqtt_config(void);
bool network_get_wifi_credentials(char *ssid, size_t ssid_size,
                                  char *password, size_t password_size);
bool network_get_mqtt_config(char *uri, size_t uri_size,
                             char *username, size_t username_size,
                             char *password, size_t password_size);
void network_disconnect(void);
bool network_has_wifi_credentials(void);
bool network_is_configured(void);
