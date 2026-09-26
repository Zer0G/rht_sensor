#include "provisioning.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "epaper.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "network.h"

#define PROVISION_CONNECTED_BIT BIT0
#define PROVISION_FAILED_BIT    BIT1
#define PROVISION_DONE_BIT      BIT2
#define PROVISION_TIMEOUT_MS    (5 * 60 * 1000)
#define PROVISION_CONNECT_MS    20000
#define DNS_PORT                53
#define DNS_BUFFER_SIZE         528

static const char *TAG = "provisioning";
static EventGroupHandle_t s_events;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static httpd_handle_t s_server;
static TaskHandle_t s_dns_task_handle;
static volatile bool s_dns_running;
static volatile bool s_testing_credentials;
static int s_dns_socket = -1;
static unsigned s_connection_retries;

static const char s_portal_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>RHT Setup</title><style>"
    "body{font-family:sans-serif;background:#f4f4f4;margin:0;padding:24px;color:#222}"
    "main{max-width:420px;margin:auto;background:white;padding:24px;border-radius:14px;"
    "box-shadow:0 2px 14px #0002}h1{margin-top:0}label{display:block;margin-top:18px}"
    "input{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;font-size:16px}"
    "button{width:100%;padding:13px;margin-top:24px;font-size:16px;background:#111;color:white;"
    "border:0;border-radius:8px}</style></head><body><main><h1>Configure device</h1>"
    "<p>Enter the Wi-Fi network and Home Assistant MQTT broker.</p>"
    "<form method=post action=/save><label>Network name (SSID)"
    "<input name=ssid maxlength=32 required autocomplete='off'></label>"
    "<label>Password<input name=password type=password maxlength=64 autocomplete='off'></label>"
    "<label>Broker MQTT<input name=mqtt_uri maxlength=127 required "
    "value='mqtt://192.168.2.99:1883' autocapitalize=off autocomplete='off'></label>"
    "<label>MQTT username<input name=mqtt_username maxlength=64 "
    "autocapitalize=off autocomplete='off'></label>"
    "<label>Password MQTT<input name=mqtt_password type=password maxlength=64 "
    "autocomplete='off'></label>"
    "<button type=submit>Save and verify</button></form></main></body></html>";

static void provisioning_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && s_testing_credentials) {
        xEventGroupSetBits(s_events, PROVISION_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED &&
               s_testing_credentials) {
        if (++s_connection_retries < 4) {
            (void)esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, PROVISION_FAILED_BIT);
        }
    }
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = (char)tolower((unsigned char)value);
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static bool url_decode(char *output, size_t output_size, const char *input, size_t input_size)
{
    size_t written = 0;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned char value = (unsigned char)input[i];
        if (value == '+') {
            value = ' ';
        } else if (value == '%' && i + 2 < input_size) {
            const int high = hex_value(input[i + 1]);
            const int low = hex_value(input[i + 2]);
            if (high < 0 || low < 0) return false;
            value = (unsigned char)((high << 4) | low);
            i += 2;
        }
        if (written + 1 >= output_size || value == 0) return false;
        output[written++] = (char)value;
    }
    output[written] = '\0';
    return true;
}

static bool form_value(const char *body, const char *key, char *output, size_t output_size)
{
    const size_t key_length = strlen(key);
    const char *field = body;
    while (*field) {
        const char *end = strchr(field, '&');
        if (!end) end = field + strlen(field);
        const char *equals = memchr(field, '=', (size_t)(end - field));
        if (equals && (size_t)(equals - field) == key_length &&
            memcmp(field, key, key_length) == 0) {
            return url_decode(output, output_size, equals + 1,
                              (size_t)(end - equals - 1));
        }
        field = *end ? end + 1 : end;
    }
    return false;
}

static esp_err_t root_get(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_portal_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= 1024) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid data");
    }
    char body[1024];
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(request, body + received,
                                         request->content_len - received);
        if (count <= 0) {
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Failed to receive data");
        }
        received += (size_t)count;
    }
    body[received] = '\0';

    char ssid[33];
    char password[65];
    char mqtt_uri[NETWORK_MQTT_URI_SIZE];
    char mqtt_username[NETWORK_MQTT_USERNAME_SIZE];
    char mqtt_password[NETWORK_MQTT_PASSWORD_SIZE];
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || !ssid[0] ||
        !form_value(body, "password", password, sizeof(password)) ||
        !form_value(body, "mqtt_uri", mqtt_uri, sizeof(mqtt_uri)) ||
        !form_value(body, "mqtt_username", mqtt_username, sizeof(mqtt_username)) ||
        !form_value(body, "mqtt_password", mqtt_password, sizeof(mqtt_password)) ||
        (strncmp(mqtt_uri, "mqtt://", 7) != 0 &&
         strncmp(mqtt_uri, "mqtts://", 8) != 0)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid Wi-Fi or MQTT settings");
    }

    wifi_config_t station = {0};
    memcpy(station.sta.ssid, ssid, strlen(ssid));
    memcpy(station.sta.password, password, strlen(password));
    station.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    station.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    s_testing_credentials = false;
    (void)esp_wifi_disconnect();
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &station);
    s_connection_retries = 0;
    xEventGroupClearBits(s_events, PROVISION_CONNECTED_BIT | PROVISION_FAILED_BIT);
    s_testing_credentials = result == ESP_OK;
    if (s_testing_credentials) result = esp_wifi_connect();
    EventBits_t bits = 0;
    if (result == ESP_OK) {
        bits = xEventGroupWaitBits(s_events, PROVISION_CONNECTED_BIT | PROVISION_FAILED_BIT,
                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(PROVISION_CONNECT_MS));
    }
    s_testing_credentials = false;

    if (!(bits & PROVISION_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "credentials rejected for SSID '%s'", ssid);
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        return httpd_resp_send(request,
            "<html><body><h2>Connection failed</h2>"
            "<p>Check the SSID and password, then go back and try again.</p></body></html>",
            HTTPD_RESP_USE_STRLEN);
    }

    result = network_save_wifi_credentials(ssid, password);
    if (result == ESP_OK) {
        result = network_save_mqtt_config(mqtt_uri, mqtt_username, mqtt_password);
    }
    if (result != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to save settings");
    }
    ESP_LOGI(TAG, "Wi-Fi and MQTT configuration saved for SSID '%s'", ssid);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    result = httpd_resp_send(request,
        "<html><body><h2>Configuration complete</h2>"
        "<p>Wi-Fi and MQTT settings have been saved. You can close this page.</p></body></html>",
        HTTPD_RESP_USE_STRLEN);
    xEventGroupSetBits(s_events, PROVISION_DONE_BIT);
    return result;
}

static esp_err_t redirect_404(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, "RHT Wi-Fi setup", HTTPD_RESP_USE_STRLEN);
}

static void dns_server_task(void *argument)
{
    (void)argument;
    uint8_t packet[DNS_BUFFER_SIZE];
    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "cannot create DNS socket");
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_dns_socket, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        ESP_LOGE(TAG, "cannot bind DNS socket");
        close(s_dns_socket);
        s_dns_socket = -1;
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_dns_running) {
        struct sockaddr_storage client;
        socklen_t client_length = sizeof(client);
        const int length = recvfrom(s_dns_socket, packet, DNS_BUFFER_SIZE - 16, 0,
                                    (struct sockaddr *)&client, &client_length);
        if (length < 17 || !s_dns_running) continue;
        if (packet[4] != 0 || packet[5] != 1) continue;
        size_t end = 12;
        while (end < (size_t)length && packet[end] != 0) {
            end += (size_t)packet[end] + 1U;
        }
        if (end + 5U > (size_t)length || end + 21U > sizeof(packet)) continue;
        end += 5U;
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = 1;
        packet[8] = packet[9] = packet[10] = packet[11] = 0;
        const uint8_t answer[] = {
            0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
            0x00, 0x3c, 0x00, 0x04, 192, 168, 4, 1
        };
        memcpy(packet + end, answer, sizeof(answer));
        (void)sendto(s_dns_socket, packet, end + sizeof(answer), 0,
                     (struct sockaddr *)&client, client_length);
    }
    if (s_dns_socket >= 0) close(s_dns_socket);
    s_dns_socket = -1;
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_portal(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = 6;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "HTTP server");
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root), TAG, "HTTP root");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &save), TAG, "HTTP save");
    ESP_RETURN_ON_ERROR(httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_404),
                        TAG, "HTTP redirect");

    s_dns_running = true;
    if (xTaskCreate(dns_server_task, "provision_dns", 3072, NULL, 4,
                    &s_dns_task_handle) != pdPASS) {
        s_dns_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void stop_provisioning(void)
{
    if (s_server) {
        (void)httpd_stop(s_server);
        s_server = NULL;
    }
    s_dns_running = false;
    if (s_dns_socket >= 0) {
        shutdown(s_dns_socket, SHUT_RDWR);
    }
    for (int i = 0; s_dns_task_handle && i < 20; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_events) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
}

esp_err_t provisioning_run(void)
{
    network_disconnect();
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "read MAC");
    char ap_ssid[20];
    char ap_password[20];
    snprintf(ap_ssid, sizeof(ap_ssid), "RHT-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(ap_password, sizeof(ap_password), "RHT%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);

    ESP_RETURN_ON_ERROR(epaper_show_provisioning(ap_ssid, ap_password),
                        TAG, "provisioning display");
    epaper_shutdown();

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_ap_netif || !s_sta_netif) {
        stop_provisioning();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_OK;
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((result = esp_wifi_init(&init)) != ESP_OK) goto done;
    if ((result = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) goto done;
    if ((result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       provisioning_event, NULL,
                                                       &s_wifi_handler)) != ESP_OK) goto done;
    if ((result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       provisioning_event, NULL,
                                                       &s_ip_handler)) != ESP_OK) goto done;

    wifi_config_t access_point = {0};
    strlcpy((char *)access_point.ap.ssid, ap_ssid, sizeof(access_point.ap.ssid));
    strlcpy((char *)access_point.ap.password, ap_password, sizeof(access_point.ap.password));
    access_point.ap.ssid_len = strlen(ap_ssid);
    access_point.ap.channel = 1;
    access_point.ap.max_connection = 4;
    access_point.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if ((result = esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK) goto done;
    if ((result = esp_wifi_set_config(WIFI_IF_AP, &access_point)) != ESP_OK) goto done;
    if ((result = esp_wifi_start()) != ESP_OK) goto done;

    static const char portal_uri[] = "http://192.168.4.1/";
    (void)esp_netif_dhcps_stop(s_ap_netif);
    (void)esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_CAPTIVEPORTAL_URI,
                                 (void *)portal_uri, strlen(portal_uri));
    (void)esp_netif_dhcps_start(s_ap_netif);
    if ((result = start_portal()) != ESP_OK) goto done;

    ESP_LOGI(TAG, "provisioning AP '%s' active for five minutes", ap_ssid);
    const EventBits_t bits = xEventGroupWaitBits(s_events, PROVISION_DONE_BIT,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(PROVISION_TIMEOUT_MS));
    result = (bits & PROVISION_DONE_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(750));
    }

done:
    stop_provisioning();
    if (result == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "provisioning timed out");
    } else if (result != ESP_OK) {
        ESP_LOGE(TAG, "provisioning failed: %s", esp_err_to_name(result));
    }
    return result;
}
