#ifndef WIFI_AP_MANAGER_H
#define WIFI_AP_MANAGER_H

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *tag;
    const char *ssid;
    const char *password;
    uint8_t channel;
    uint8_t max_connections;
} wifi_ap_manager_cfg_t;

esp_err_t wifi_ap_manager_start(const wifi_ap_manager_cfg_t *cfg);

#endif
