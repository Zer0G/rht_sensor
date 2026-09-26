#pragma once

#include "esp_err.h"

esp_err_t ota_confirm_running_image(void);
esp_err_t ota_install_from_github(void);
