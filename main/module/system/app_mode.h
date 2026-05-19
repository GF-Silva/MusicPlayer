#ifndef APP_MODE_H
#define APP_MODE_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t app_mode_nvs_init(void);
esp_err_t app_mode_wifi_ap_is_enabled(bool *enabled);
esp_err_t app_mode_wifi_ap_set_enabled(bool enabled);
void app_mode_toggle_wifi_ap_and_restart(void);

#endif
