#include "network.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
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
#include "nvs.h"
#include "sdkconfig.h"
#include "settings.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1
#define MQTT_CONNECTED_BIT BIT0
#define MQTT_FAILED_BIT    BIT1
#define MQTT_DATA_BIT      BIT2

static const char *TAG = "network";
static EventGroupHandle_t s_wifi_events;
static EventGroupHandle_t s_mqtt_events;
static esp_netif_t *s_station;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static int s_wifi_retries;
static volatile int s_last_published_id;
static volatile bool s_ota_requested;

#define WIFI_NVS_NAMESPACE "rht_wifi"
#define WIFI_NVS_SSID_KEY  "ssid"
#define WIFI_NVS_PASS_KEY  "password"
#define MQTT_NVS_NAMESPACE "rht_mqtt"
#define MQTT_NVS_URI_KEY   "uri"
#define MQTT_NVS_USER_KEY  "username"
#define MQTT_NVS_PASS_KEY  "password"

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

bool network_get_wifi_credentials(char *ssid, size_t ssid_size,
                                  char *password, size_t password_size)
{
    if (!ssid || ssid_size == 0 || !password || password_size == 0) {
        return false;
    }
    ssid[0] = '\0';
    password[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t stored_ssid_size = ssid_size;
        size_t stored_password_size = password_size;
        const esp_err_t ssid_result = nvs_get_str(nvs, WIFI_NVS_SSID_KEY,
                                                  ssid, &stored_ssid_size);
        const esp_err_t password_result = nvs_get_str(nvs, WIFI_NVS_PASS_KEY,
                                                      password, &stored_password_size);
        nvs_close(nvs);
        if (ssid_result == ESP_OK && password_result == ESP_OK && ssid[0]) {
            return true;
        }
        ssid[0] = '\0';
        password[0] = '\0';
    }

    strlcpy(ssid, CONFIG_RHT_WIFI_SSID, ssid_size);
    strlcpy(password, CONFIG_RHT_WIFI_PASSWORD, password_size);
    return ssid[0] != '\0';
}

esp_err_t network_save_wifi_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !password || strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open Wi-Fi NVS");
    esp_err_t result = nvs_set_str(nvs, WIFI_NVS_SSID_KEY, ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(nvs, WIFI_NVS_PASS_KEY, password);
    }
    if (result == ESP_OK) {
        result = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return result;
}

esp_err_t network_clear_wifi_credentials(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open Wi-Fi NVS");
    esp_err_t result = nvs_erase_all(nvs);
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

bool network_get_mqtt_config(char *uri, size_t uri_size,
                             char *username, size_t username_size,
                             char *password, size_t password_size)
{
    if (!uri || uri_size == 0 || !username || username_size == 0 ||
        !password || password_size == 0) {
        return false;
    }
    uri[0] = '\0';
    username[0] = '\0';
    password[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t stored_uri_size = uri_size;
        size_t stored_username_size = username_size;
        size_t stored_password_size = password_size;
        const esp_err_t uri_result =
            nvs_get_str(nvs, MQTT_NVS_URI_KEY, uri, &stored_uri_size);
        const esp_err_t username_result =
            nvs_get_str(nvs, MQTT_NVS_USER_KEY, username, &stored_username_size);
        const esp_err_t password_result =
            nvs_get_str(nvs, MQTT_NVS_PASS_KEY, password, &stored_password_size);
        nvs_close(nvs);
        if (uri_result == ESP_OK && username_result == ESP_OK &&
            password_result == ESP_OK && uri[0]) {
            return true;
        }
        uri[0] = '\0';
        username[0] = '\0';
        password[0] = '\0';
    }

    strlcpy(uri, CONFIG_RHT_MQTT_URI, uri_size);
    strlcpy(username, CONFIG_RHT_MQTT_USERNAME, username_size);
    strlcpy(password, CONFIG_RHT_MQTT_PASSWORD, password_size);
    return uri[0] != '\0';
}

esp_err_t network_save_mqtt_config(const char *uri, const char *username,
                                   const char *password)
{
    if (!uri || !uri[0] || strlen(uri) >= NETWORK_MQTT_URI_SIZE ||
        !username || strlen(username) >= NETWORK_MQTT_USERNAME_SIZE ||
        !password || strlen(password) >= NETWORK_MQTT_PASSWORD_SIZE ||
        (strncmp(uri, "mqtt://", 7) != 0 && strncmp(uri, "mqtts://", 8) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open MQTT NVS");
    esp_err_t result = nvs_set_str(nvs, MQTT_NVS_URI_KEY, uri);
    if (result == ESP_OK) result = nvs_set_str(nvs, MQTT_NVS_USER_KEY, username);
    if (result == ESP_OK) result = nvs_set_str(nvs, MQTT_NVS_PASS_KEY, password);
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

esp_err_t network_clear_mqtt_config(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open MQTT NVS");
    esp_err_t result = nvs_erase_all(nvs);
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

bool network_has_wifi_credentials(void)
{
    char ssid[33];
    char password[65];
    return network_get_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));
}

bool network_is_configured(void)
{
    char uri[NETWORK_MQTT_URI_SIZE];
    char username[NETWORK_MQTT_USERNAME_SIZE];
    char password[NETWORK_MQTT_PASSWORD_SIZE];
    return network_has_wifi_credentials() &&
           network_get_mqtt_config(uri, sizeof(uri), username, sizeof(username),
                                   password, sizeof(password));
}

esp_err_t network_connect(int8_t *rssi)
{
    char ssid[33];
    char password[65];
    if (!network_get_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
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
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
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
    } else if (id == MQTT_EVENT_DATA) {
        ESP_LOGI(TAG, "MQTT RX topic='%.*s' payload='%.*s'",
                 event->topic_len, event->topic,
                 event->data_len, event->data);
        xEventGroupSetBits(s_mqtt_events, MQTT_DATA_BIT);
        char command[128] = {0};
        if (event->data && event->data_len > 0) {
            size_t command_length = (size_t)event->data_len;
            if (command_length >= sizeof(command)) command_length = sizeof(command) - 1U;
            memcpy(command, event->data, command_length);
            command[command_length] = '\0';
        }
        if (strstr(command, "ota_install") || strstr(command, "ota_check")) {
            s_ota_requested = strstr(command, "ota_install") != NULL;
            ESP_LOGI(TAG, "OTA command received: %s",
                     s_ota_requested ? "install" : "check");
        } else if (settings_apply_command(command)) {
            ESP_LOGI(TAG, "runtime setting updated: %s", command);
        }
    }
}

bool network_take_ota_request(void)
{
    const bool requested = s_ota_requested;
    s_ota_requested = false;
    return requested;
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
    /* Discovery runs synchronously from the main task. Keep the large JSON
     * buffer out of its 4 KiB stack. */
    static char payload[768];
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

static esp_err_t publish_firmware_discovery(esp_mqtt_client_handle_t client)
{
    char topic[192];
    static char payload[768];
    snprintf(topic, sizeof(topic), "%s/firmware_version/config",
             CONFIG_RHT_MQTT_BASE_TOPIC);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Firmware version\",\"unique_id\":\"%s_firmware_version\"," 
             "\"state_topic\":\"%s/firmware_version/state\","
             "\"icon\":\"mdi:chip\",\"device\":{\"identifiers\":[\"%s\"],"
             "\"name\":\"RHT ePaper\",\"manufacturer\":\"Waveshare\","
             "\"model\":\"ESP32-C6-ePaper-1.54\"}}",
             CONFIG_RHT_DEVICE_ID, CONFIG_RHT_MQTT_BASE_TOPIC,
             CONFIG_RHT_DEVICE_ID);
    return publish_wait(client, topic, payload, 1, true);
}

static esp_err_t publish_update_discovery(esp_mqtt_client_handle_t client)
{
    char topic[192];
    static char payload[768];
    snprintf(topic, sizeof(topic), "%s/firmware_update/config",
             CONFIG_RHT_MQTT_BASE_TOPIC);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Firmware update\",\"unique_id\":\"%s_firmware_update\","
             "\"state_topic\":\"%s/firmware_update/state\","
             "\"value_template\":\"{{ value_json.installed_version }}\","
             "\"latest_version_topic\":\"%s/firmware_update/state\","
             "\"latest_version_template\":\"{{ value_json.latest_version }}\","
             "\"command_topic\":\"%s/command\",\"payload_install\":\"ota_install\","
             "\"device_class\":\"firmware\",\"release_url\":\"https://github.com/zer0g/climacarta/releases\","
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"RHT ePaper\","
             "\"manufacturer\":\"Waveshare\",\"model\":\"ESP32-C6-ePaper-1.54\"}}",
             CONFIG_RHT_DEVICE_ID, CONFIG_RHT_MQTT_BASE_TOPIC,
             CONFIG_RHT_MQTT_BASE_TOPIC, CONFIG_RHT_MQTT_BASE_TOPIC,
             CONFIG_RHT_DEVICE_ID);
    return publish_wait(client, topic, payload, 1, true);
}

static esp_err_t publish_runtime_settings_discovery(esp_mqtt_client_handle_t client)
{
    char topic[192];
    static char payload[768];
    snprintf(topic, sizeof(topic), "%s/t_sample/config", CONFIG_RHT_MQTT_BASE_TOPIC);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Sample interval\",\"unique_id\":\"%s_t_sample\","
             "\"state_topic\":\"%s/settings/state\",\"value_template\":\"{{ value_json.t_sample }}\","
             "\"command_topic\":\"%s/command\",\"command_template\":\"t_sample={{ value }}\","
             "\"unit_of_measurement\":\"s\",\"min\":10,\"max\":86400,\"step\":1,\"mode\":\"box\","
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"RHT ePaper\"}}",
             CONFIG_RHT_DEVICE_ID, CONFIG_RHT_MQTT_BASE_TOPIC,
             CONFIG_RHT_MQTT_BASE_TOPIC, CONFIG_RHT_DEVICE_ID);
    if (publish_wait(client, topic, payload, 1, true) != ESP_OK) return ESP_FAIL;
    snprintf(topic, sizeof(topic), "%s/update_freq/config", CONFIG_RHT_MQTT_BASE_TOPIC);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"MQTT update frequency\",\"unique_id\":\"%s_update_freq\","
             "\"state_topic\":\"%s/settings/state\",\"value_template\":\"{{ value_json.update_freq }}\","
             "\"command_topic\":\"%s/command\",\"command_template\":\"update_freq={{ value }}\","
             "\"unit_of_measurement\":\"samples\",\"min\":0,\"max\":255,\"step\":1,\"mode\":\"box\","
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"RHT ePaper\"}}",
             CONFIG_RHT_DEVICE_ID, CONFIG_RHT_MQTT_BASE_TOPIC,
             CONFIG_RHT_MQTT_BASE_TOPIC, CONFIG_RHT_DEVICE_ID);
    return publish_wait(client, topic, payload, 1, true);
}

static bool json_append_optional_float(char *payload, size_t payload_size,
                                       size_t *used, const char *key,
                                       bool valid, float value)
{
    const int written = valid
        ? snprintf(payload + *used, payload_size - *used, ",\"%s\":%.2f", key, value)
        : snprintf(payload + *used, payload_size - *used, ",\"%s\":null", key);
    if (written < 0 || (size_t)written >= payload_size - *used) {
        return false;
    }
    *used += (size_t)written;
    return true;
}

esp_err_t network_publish(const network_measurement_t *measurement, bool send_discovery)
{
    char mqtt_uri[NETWORK_MQTT_URI_SIZE];
    char mqtt_username[NETWORK_MQTT_USERNAME_SIZE];
    char mqtt_password[NETWORK_MQTT_PASSWORD_SIZE];
    if (!measurement ||
        !network_get_mqtt_config(mqtt_uri, sizeof(mqtt_uri),
                                 mqtt_username, sizeof(mqtt_username),
                                 mqtt_password, sizeof(mqtt_password))) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mqtt_events = xEventGroupCreate();
    if (!s_mqtt_events) {
        return ESP_ERR_NO_MEM;
    }
    const esp_mqtt_client_config_t config = {
        .broker.address.uri = mqtt_uri,
        .credentials.client_id = CONFIG_RHT_DEVICE_ID,
        .credentials.username = mqtt_username[0] ? mqtt_username : NULL,
        .credentials.authentication.password = mqtt_password[0] ? mqtt_password : NULL,
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
    if (result == ESP_OK) {
        char command_topic[192];
        snprintf(command_topic, sizeof(command_topic), "%s/command",
                 CONFIG_RHT_MQTT_BASE_TOPIC);
        if (esp_mqtt_client_subscribe(client, command_topic, 1) < 0) {
            result = ESP_FAIL;
        }
    }
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "temperature", "Temperature", celsius, "temperature", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "humidity", "Humidity", "%", "humidity", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "dew_point", "Dew point", celsius, "temperature", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "day_min", "Daily minimum", celsius, "temperature", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "day_max", "Daily maximum", celsius, "temperature", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "battery", "Battery", "%", "battery", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "battery_voltage", "Battery voltage", "mV", "voltage", "measurement");
    if (result == ESP_OK && send_discovery) result = publish_discovery(client, "rssi", "Wi-Fi RSSI", "dBm", "signal_strength", "measurement");
    if (result == ESP_OK && send_discovery) {
        result = publish_firmware_discovery(client);
    }
    if (result == ESP_OK && send_discovery) {
        result = publish_update_discovery(client);
    }
    if (result == ESP_OK && send_discovery) {
        result = publish_runtime_settings_discovery(client);
    }
    static const char *history_keys[NETWORK_HISTORY_COUNT] = {
        "previous_hour", "same_hour_yesterday", "previous_week",
        "previous_month", "previous_year",
    };
    static const char *history_names[NETWORK_HISTORY_COUNT] = {
        "Previous hour", "Same hour yesterday", "Previous week",
        "Previous month", "Previous year",
    };
    if (result == ESP_OK && send_discovery) {
        for (size_t i = 0; i < NETWORK_HISTORY_COUNT && result == ESP_OK; ++i) {
            char delta_key[48];
            char delta_name[64];
            result = publish_discovery(client, history_keys[i], history_names[i],
                                       celsius, "temperature", "measurement");
            snprintf(delta_key, sizeof(delta_key), "%s_delta", history_keys[i]);
            snprintf(delta_name, sizeof(delta_name), "%s delta", history_names[i]);
            if (result == ESP_OK) {
                result = publish_discovery(client, delta_key, delta_name,
                                           celsius, "temperature", "measurement");
            }
        }
    }

    if (result == ESP_OK) {
        const esp_app_desc_t *app = esp_app_get_description();
        char firmware_topic[192];
        snprintf(firmware_topic, sizeof(firmware_topic),
                 "%s/firmware_version/state", CONFIG_RHT_MQTT_BASE_TOPIC);
        result = publish_wait(client, firmware_topic, app->version, 1, true);
    }
    if (result == ESP_OK) {
        char update_topic[192];
        static char update_payload[192];
        snprintf(update_topic, sizeof(update_topic),
                 "%s/firmware_update/state", CONFIG_RHT_MQTT_BASE_TOPIC);
        snprintf(update_payload, sizeof(update_payload),
                 "{\"installed_version\":\"%s\",\"latest_version\":\"%s\"}",
                 measurement->installed_version, measurement->latest_version);
        result = publish_wait(client, update_topic, update_payload, 1, true);
    }
    if (result == ESP_OK) {
        char settings_topic[192];
        static char settings_payload[96];
        snprintf(settings_topic, sizeof(settings_topic), "%s/settings/state",
                 CONFIG_RHT_MQTT_BASE_TOPIC);
        snprintf(settings_payload, sizeof(settings_payload),
                 "{\"t_sample\":%lu,\"update_freq\":%lu}",
                 (unsigned long)settings_get_t_sample(),
                 (unsigned long)settings_get_update_freq());
        result = publish_wait(client, settings_topic, settings_payload, 1, true);
    }
    if (result == ESP_OK) {
        char topic[192];
        /* MQTT publication is serialized, so a static buffer is safe and
         * avoids nesting another large allocation on the main task stack. */
        static char payload[1280];
        snprintf(topic, sizeof(topic), "%s/state", CONFIG_RHT_MQTT_BASE_TOPIC);
        const int base_length = snprintf(payload, sizeof(payload),
                 "{\"timestamp\":%" PRIi64 ",\"firmware_version\":\"%s\",\"temperature\":%.2f,\"humidity\":%.2f,"
                 "\"dew_point\":%.2f,\"day_min\":%.2f,\"day_max\":%.2f,"
                 "\"battery\":%u,\"battery_voltage\":%.0f,\"rssi\":%d",
                 (int64_t)measurement->timestamp, esp_app_get_description()->version,
                 measurement->temperature_c,
                 measurement->humidity_pct, measurement->dew_point_c,
                 measurement->day_min_c, measurement->day_max_c,
                 measurement->battery_pct, measurement->battery_v * 1000.0f,
                 measurement->rssi);
        size_t used = base_length > 0 ? (size_t)base_length : sizeof(payload);
        if (used >= sizeof(payload)) {
            result = ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < NETWORK_HISTORY_COUNT && result == ESP_OK; ++i) {
            char delta_key[48];
            snprintf(delta_key, sizeof(delta_key), "%s_delta", history_keys[i]);
            if (!json_append_optional_float(payload, sizeof(payload), &used,
                                            history_keys[i],
                                            measurement->history_valid[i],
                                            measurement->history_reference_c[i]) ||
                !json_append_optional_float(payload, sizeof(payload), &used,
                                            delta_key,
                                            measurement->history_valid[i],
                                            measurement->history_delta_c[i])) {
                result = ESP_ERR_INVALID_SIZE;
            }
        }
        if (result == ESP_OK && used + 2U <= sizeof(payload)) {
            payload[used++] = '}';
            payload[used] = '\0';
            result = publish_wait(client, topic, payload, 1, true);
        }
    }
    if (result == ESP_OK) {
        (void)xEventGroupWaitBits(s_mqtt_events, MQTT_DATA_BIT, pdTRUE, pdFALSE,
                                  pdMS_TO_TICKS(750));
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
