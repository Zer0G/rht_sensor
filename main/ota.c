#include "ota.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "sdkconfig.h"

#define OTA_MANIFEST_MAX_SIZE 2048
#define OTA_URL_MAX_SIZE      512
#define OTA_SHA256_HEX_SIZE   65
#define OTA_HTTP_BUFFER_SIZE  4096

static const char *TAG = "ota";

static bool json_string(const char *json, const char *key,
                        char *output, size_t output_size)
{
    char needle[48];
    const int needle_length = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_length <= 0) return false;
    const char *cursor = strstr(json, needle);
    if (!cursor) return false;
    cursor = strchr(cursor + needle_length, ':');
    if (!cursor) return false;
    ++cursor;
    while (*cursor && isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor++ != '"') return false;
    size_t length = 0;
    while (cursor[length] && cursor[length] != '"') {
        if (cursor[length] == '\\' || length + 1U >= output_size) return false;
        ++length;
    }
    if (cursor[length] != '"') return false;
    memcpy(output, cursor, length);
    output[length] = '\0';
    return true;
}

static bool valid_sha256(const char *value)
{
    if (strlen(value) != OTA_SHA256_HEX_SIZE - 1U) return false;
    for (size_t i = 0; i < OTA_SHA256_HEX_SIZE - 1U; ++i) {
        if (!isxdigit((unsigned char)value[i])) return false;
    }
    return true;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool parse_manifest(const char *json, char *version, char *url,
                           char *sha256)
{
    return json_string(json, "version", version, OTA_VERSION_MAX_SIZE) &&
           json_string(json, "url", url, OTA_URL_MAX_SIZE) &&
           json_string(json, "sha256", sha256, OTA_SHA256_HEX_SIZE) &&
           valid_sha256(sha256) && strncmp(url, "https://", 8) == 0;
}

static esp_err_t fetch_manifest(char *manifest, size_t manifest_size)
{
    const esp_http_client_config_t config = {
        .url = CONFIG_RHT_OTA_MANIFEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_HTTP_BUFFER_SIZE,
        .keep_alive_enable = false,
        /* GitHub may emit multiple redirects. Handle each one explicitly so
         * the connection is reopened after the URL changes. */
        .disable_auto_redirect = true,
        .max_redirection_count = 5,
    };
    ESP_LOGI(TAG, "manifest URL: %s", CONFIG_RHT_OTA_MANIFEST_URL);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "manifest init failed: %s", esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = esp_http_client_open(client, 0);
    ESP_LOGI(TAG, "manifest open: %s errno=%d", esp_err_to_name(result),
             esp_http_client_get_errno(client));
    for (int redirect = 0; result == ESP_OK && redirect <= 5; ++redirect) {
        /* fetch_headers() returns the content length (int64_t), not an
         * esp_err_t. Only negative values represent an error. */
        const int64_t header_result = esp_http_client_fetch_headers(client);
        result = header_result < 0 ? (esp_err_t)header_result : ESP_OK;
        const int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "manifest headers: %s status=%d errno=%d length=%" PRId64,
                 esp_err_to_name(result), status, esp_http_client_get_errno(client),
                 esp_http_client_get_content_length(client));
        if (result != ESP_OK || status < 300 || status >= 400) break;
        if (redirect == 5) {
            result = ESP_ERR_HTTP_MAX_REDIRECT;
            break;
        }
        result = esp_http_client_set_redirection(client);
        ESP_LOGI(TAG, "manifest redirect %d: %s", redirect + 1,
                 esp_err_to_name(result));
        if (result == ESP_OK) {
            /* set_redirection() closes automatically only when the host
             * changes. Close also for same-host redirects before issuing the
             * request for the new path. */
            (void)esp_http_client_close(client);
            result = esp_http_client_open(client, 0);
            ESP_LOGI(TAG, "manifest redirect open %d: %s errno=%d",
                     redirect + 1, esp_err_to_name(result),
                     esp_http_client_get_errno(client));
        }
    }
    char final_url[OTA_URL_MAX_SIZE];
    if (esp_http_client_get_url(client, final_url, sizeof(final_url)) == ESP_OK) {
        ESP_LOGI(TAG, "manifest final URL: %s", final_url);
    }
    size_t used = 0;
    while (result == ESP_OK && used + 1U < manifest_size) {
        const int read = esp_http_client_read(client, manifest + used,
                                              (int)(manifest_size - used - 1U));
        if (read < 0) {
            ESP_LOGE(TAG, "manifest read failed: read=%d errno=%d status=%d",
                     read, esp_http_client_get_errno(client),
                     esp_http_client_get_status_code(client));
            result = ESP_FAIL;
            break;
        }
        if (read == 0) break;
        used += (size_t)read;
    }
    if (result == ESP_OK && esp_http_client_get_status_code(client) != 200) {
        result = ESP_ERR_HTTP_BASE + esp_http_client_get_status_code(client);
    }
    ESP_LOGI(TAG, "manifest result: %s status=%d bytes=%u errno=%d",
             esp_err_to_name(result), esp_http_client_get_status_code(client),
             (unsigned)used, esp_http_client_get_errno(client));
    manifest[used] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

esp_err_t ota_get_latest_version(char *version, size_t version_size)
{
    if (!version || version_size == 0) return ESP_ERR_INVALID_ARG;
    char manifest[OTA_MANIFEST_MAX_SIZE];
    char url[OTA_URL_MAX_SIZE];
    char sha256[OTA_SHA256_HEX_SIZE];
    if (fetch_manifest(manifest, sizeof(manifest)) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!parse_manifest(manifest, version, url, sha256) ||
        strlen(version) >= version_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static bool digest_matches(const uint8_t *digest, const char *expected)
{
    for (size_t i = 0; i < 32; ++i) {
        const int high = hex_value(expected[i * 2]);
        const int low = hex_value(expected[i * 2 + 1]);
        if (digest[i] != (uint8_t)((high << 4) | low)) return false;
    }
    return true;
}

esp_err_t ota_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t result = esp_ota_get_state_partition(running, &state);
    if (result == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        result = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed: %s", esp_err_to_name(result));
    }
    return result == ESP_ERR_NOT_FOUND ? ESP_OK : result;
}

esp_err_t ota_install_from_github(void)
{
    char manifest[OTA_MANIFEST_MAX_SIZE];
    char version[OTA_VERSION_MAX_SIZE];
    char url[OTA_URL_MAX_SIZE];
    char expected_sha256[OTA_SHA256_HEX_SIZE];
    esp_err_t result = fetch_manifest(manifest, sizeof(manifest));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "download manifest failed: %s", esp_err_to_name(result));
        return result;
    }
    if (!parse_manifest(manifest, version, url, expected_sha256)) {
        ESP_LOGE(TAG, "invalid GitHub OTA manifest");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const esp_app_desc_t *current = esp_app_get_description();
    if (!strcmp(current->version, version)) {
        ESP_LOGI(TAG, "firmware %s is already installed", version);
        return ESP_ERR_NOT_FOUND;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) return ESP_ERR_NOT_FOUND;
    const esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_HTTP_BUFFER_SIZE,
        .keep_alive_enable = false,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
    };
    const esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .partition = {
            .staging = target,
            .final = target,
            .finalize_with_copy = false,
        },
    };
    esp_https_ota_handle_t handle = NULL;
    result = esp_https_ota_begin(&ota_config, &handle);
    if (result != ESP_OK) return result;

    esp_app_desc_t image_desc;
    result = esp_https_ota_get_img_desc(handle, &image_desc);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "downloading firmware %s to %s", image_desc.version, target->label);
        do {
            result = esp_https_ota_perform(handle);
        } while (result == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    }
    if (result == ESP_OK && !esp_https_ota_is_complete_data_received(handle)) {
        result = ESP_ERR_INVALID_SIZE;
    }
    if (result == ESP_OK) {
        uint8_t digest[32];
        result = esp_partition_get_sha256(target, digest);
        if (result == ESP_OK && !digest_matches(digest, expected_sha256)) {
            ESP_LOGE(TAG, "GitHub image SHA-256 mismatch");
            result = ESP_ERR_INVALID_CRC;
        }
    }
    if (result == ESP_OK) {
        result = esp_https_ota_finish(handle);
    } else {
        (void)esp_https_ota_abort(handle);
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "OTA image installed; rebooting into %s", version);
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }
    return result;
}
