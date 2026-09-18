#include "network.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1
#define MQTT_CONNECTED_BIT BIT0
#define MQTT_FAILED_BIT    BIT1

static const char *TAG = "network";
static EventGroupHandle_t s_wifi_events;
static EventGroupHandle_t s_mqtt_events;
static esp_netif_t *s_station;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static int s_wifi_retries;
static volatile int s_last_published_id;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_wifi_retries < 4) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

bool network_is_configured(void)
{
    return CONFIG_RHT_WIFI_SSID[0] != '\0' && CONFIG_RHT_MQTT_URI[0] != '\0';
}

esp_err_t network_connect(int8_t *rssi)
{
    if (CONFIG_RHT_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID is empty");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_station || s_wifi_events) {
        network_disconnect();
    }
    s_wifi_retries = 0;
    s_wifi_handler = NULL;
    s_ip_handler = NULL;
    esp_err_t result = ESP_OK;

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }
    s_station = esp_netif_create_default_wifi_sta();
    if (!s_station) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((result = esp_wifi_init(&init)) != ESP_OK) goto fail;
    if ((result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       wifi_event, NULL, &s_wifi_handler)) != ESP_OK) goto fail;
    if ((result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       wifi_event, NULL, &s_ip_handler)) != ESP_OK) goto fail;

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, CONFIG_RHT_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_RHT_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = CONFIG_RHT_WIFI_PASSWORD[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if ((result = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) goto fail;
    if ((result = esp_wifi_set_config(WIFI_IF_STA, &config)) != ESP_OK) goto fail;
    if ((result = esp_wifi_set_ps(WIFI_PS_MAX_MODEM)) != ESP_OK) goto fail;
    if ((result = esp_wifi_start()) != ESP_OK) goto fail;

    const EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                                  WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                                  pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Wi-Fi connection failed");
        result = ESP_ERR_TIMEOUT;
        goto fail;
    }
    wifi_ap_record_t access_point;
    if (rssi && esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        *rssi = access_point.rssi;
    }
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Wi-Fi setup failed: %s", esp_err_to_name(result));
    network_disconnect();
    return result;
}

esp_err_t network_sync_time(void)
{
    const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_RHT_NTP_SERVER);
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&config), TAG, "SNTP init");
    esp_err_t result = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < 8; ++attempt) {
        result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
        if (result == ESP_OK) {
            break;
        }
    }
    esp_netif_sntp_deinit();
    if (result == ESP_OK) {
        time_t now;
        time(&now);
        ESP_LOGI(TAG, "NTP synchronized: %" PRIi64, (int64_t)now);
    } else {
        ESP_LOGW(TAG, "NTP synchronization timed out");
    }
    return result;
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = data;
    if (id == MQTT_EVENT_CONNECTED) {
        xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
    } else if (id == MQTT_EVENT_ERROR) {
        xEventGroupSetBits(s_mqtt_events, MQTT_FAILED_BIT);
    } else if (id == MQTT_EVENT_PUBLISHED) {
        s_last_published_id = event->msg_id;
    }
}

static esp_err_t publish_wait(esp_mqtt_client_handle_t client, const char *topic,
                              const char *payload, int qos, bool retain)
{
    const int id = esp_mqtt_client_publish(client, topic, payload, 0, qos, retain);
    if (id < 0) {
        return ESP_FAIL;
    }
    if (qos == 0) {
        return ESP_OK;
    }
    for (int i = 0; i < 100; ++i) {
        if (s_last_published_id == id) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t publish_discovery(esp_mqtt_client_handle_t client, const char *key,
                                   const char *name, const char *unit,
                                   const char *device_class, const char *state_class)
{
    char topic[192];
    char payload[768];
    snprintf(topic, sizeof(topic), "%s/%s/config", CONFIG_RHT_MQTT_BASE_TOPIC, key);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
             "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.%s }}\","
             "\"unit_of_measurement\":\"%s\",\"device_class\":\"%s\","
             "\"state_class\":\"%s\",\"device\":{\"identifiers\":[\"%s\"],"
             "\"name\":\"RHT ePaper\",\"manufacturer\":\"Waveshare\","
             "\"model\":\"ESP32-C6-ePaper-1.54\"}}",
             name, CONFIG_RHT_DEVICE_ID, key, CONFIG_RHT_MQTT_BASE_TOPIC, key,
             unit, device_class, state_class, CONFIG_RHT_DEVICE_ID);
    return publish_wait(client, topic, payload, 1, true);
}

esp_err_t network_publish(const network_measurement_t *measurement, bool send_discovery)
{
    if (!measurement || CONFIG_RHT_MQTT_URI[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    s_mqtt_events = xEventGroupCreate();
    if (!s_mqtt_events) {
        return ESP_ERR_NO_MEM;
    }
    const esp_mqtt_client_config_t config = {
        .broker.address.uri = CONFIG_RHT_MQTT_URI,
        .credentials.client_id = CONFIG_RHT_DEVICE_ID,
        .credentials.username = CONFIG_RHT_MQTT_USERNAME[0] ? CONFIG_RHT_MQTT_USERNAME : NULL,
        .credentials.authentication.password = CONFIG_RHT_MQTT_PASSWORD[0] ? CONFIG_RHT_MQTT_PASSWORD : NULL,
        .network.disable_auto_reconnect = true,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (!client) {
        vEventGroupDelete(s_mqtt_events);
        s_mqtt_events = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_last_published_id = -1;
    esp_err_t result = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    bool started = false;
    if (result == ESP_OK) {
        result = esp_mqtt_client_start(client);
        started = result == ESP_OK;
    }
    if (result == ESP_OK) {
        const EventBits_t bits = xEventGroupWaitBits(s_mqtt_events,
                                                      MQTT_CONNECTED_BIT | MQTT_FAILED_BIT,
                                                      pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        result = (bits & MQTT_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    static const char celsius[] = "\xC2\xB0" "C";
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "temperature", "Temperature", celsius, "temperature", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "humidity", "Humidity", "%", "humidity", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "dew_point", "Dew point", celsius, "temperature", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "day_min", "Daily minimum", celsius, "temperature", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "day_max", "Daily maximum", celsius, "temperature", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "battery", "Battery", "%", "battery", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "battery_voltage", "Battery voltage", "V", "voltage", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "rssi", "Wi-Fi RSSI", "dBm", "signal_strength", "measurement");

    if (result == ESP_OK) {
        char topic[192];
        char payload[512];
        snprintf(topic, sizeof(topic), "%s/state", CONFIG_RHT_MQTT_BASE_TOPIC);
        snprintf(payload, sizeof(payload),
                 "{\"timestamp\":%" PRIi64 ",\"temperature\":%.2f,\"humidity\":%.2f,"
                 "\"dew_point\":%.2f,\"day_min\":%.2f,\"day_max\":%.2f,"
                 "\"battery\":%u,\"battery_voltage\":%.3f,\"rssi\":%d}",
                 (int64_t)measurement->timestamp, measurement->temperature_c,
                 measurement->humidity_pct, measurement->dew_point_c,
                 measurement->day_min_c, measurement->day_max_c,
                 measurement->battery_pct, measurement->battery_v, measurement->rssi);
        result = publish_wait(client, topic, payload, 1, true);
    }

    if (started) {
        (void)esp_mqtt_client_disconnect(client);
        (void)esp_mqtt_client_stop(client);
    }
    esp_mqtt_client_destroy(client);
    vEventGroupDelete(s_mqtt_events);
    s_mqtt_events = NULL;
    return result;
}

void network_disconnect(void)
{
    if (s_station) {
        if (s_ip_handler) {
            (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
            s_ip_handler = NULL;
        }
        if (s_wifi_handler) {
            (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
            s_wifi_handler = NULL;
        }
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        esp_netif_destroy_default_wifi(s_station);
        s_station = NULL;
    }
    if (s_wifi_events) {
        vEventGroupDelete(s_wifi_events);
        s_wifi_events = NULL;
    }
}
