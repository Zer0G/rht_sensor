#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t settings_init(void);
uint32_t settings_get_t_sample(void);
uint32_t settings_get_update_freq(void);
esp_err_t settings_set_t_sample(uint32_t value);
esp_err_t settings_set_update_freq(uint32_t value);
bool settings_apply_command(const char *command);
