#include "settings.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "sdkconfig.h"

#define SETTINGS_NAMESPACE "settings"
#define T_SAMPLE_KEY       "t_sample"
#define UPDATE_FREQ_KEY    "update_freq"
#define T_SAMPLE_MIN       10U
#define T_SAMPLE_MAX       86400U
#define UPDATE_FREQ_MAX    255U

static uint32_t s_t_sample = CONFIG_RHT_T_SAMPLE;
static uint32_t s_update_freq = CONFIG_RHT_UPDATE_FREQ;

static esp_err_t save_u32(const char *key, uint32_t value)
{
    nvs_handle_t nvs;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs);
    if (result != ESP_OK) return result;
    result = nvs_set_u32(nvs, key, value);
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

esp_err_t settings_init(void)
{
    nvs_handle_t nvs;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    (void)nvs_get_u32(nvs, T_SAMPLE_KEY, &s_t_sample);
    (void)nvs_get_u32(nvs, UPDATE_FREQ_KEY, &s_update_freq);
    nvs_close(nvs);
    if (s_t_sample < T_SAMPLE_MIN || s_t_sample > T_SAMPLE_MAX) s_t_sample = CONFIG_RHT_T_SAMPLE;
    if (s_update_freq > UPDATE_FREQ_MAX) s_update_freq = CONFIG_RHT_UPDATE_FREQ;
    return ESP_OK;
}

uint32_t settings_get_t_sample(void) { return s_t_sample; }
uint32_t settings_get_update_freq(void) { return s_update_freq; }

esp_err_t settings_set_t_sample(uint32_t value)
{
    if (value < T_SAMPLE_MIN || value > T_SAMPLE_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t result = save_u32(T_SAMPLE_KEY, value);
    if (result == ESP_OK) s_t_sample = value;
    return result;
}

esp_err_t settings_set_update_freq(uint32_t value)
{
    if (value > UPDATE_FREQ_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t result = save_u32(UPDATE_FREQ_KEY, value);
    if (result == ESP_OK) s_update_freq = value;
    return result;
}

bool settings_apply_command(const char *command)
{
    if (!command) return false;
    const char *equals = strchr(command, '=');
    if (!equals || equals == command) return false;
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(equals + 1, &end, 10);
    if (errno || end == equals + 1 || *end != '\0' || value > UINT32_MAX) return false;
    if (!strncmp(command, "t_sample=", 9)) {
        return settings_set_t_sample((uint32_t)value) == ESP_OK;
    }
    if (!strncmp(command, "update_freq=", 12)) {
        return settings_set_update_freq((uint32_t)value) == ESP_OK;
    }
    return false;
}
