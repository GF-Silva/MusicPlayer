#ifndef BT_MANAGER_H
#define BT_MANAGER_H

#include <stdbool.h>

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt_defs.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

typedef struct {
    const char *tag;

    bool *bt_initialized;
    bool *bt_connected;
    bool *bt_connecting;

    bool *device_found;
    bool *discovery_active;
    bool *connect_after_discovery_stop;
    bool *discovery_stop_pending;

    esp_bd_addr_t *target_device_addr;
    const esp_bd_addr_t *target_mac_addr;

    TimerHandle_t discovery_timer;

    void (*set_bt_connecting)(bool connecting);
    void (*log_bt_state)(const char *reason);

    esp_bt_gap_cb_t gap_callback;
    esp_a2d_cb_t a2dp_callback;
    esp_avrc_ct_cb_t avrc_ct_callback;
    esp_avrc_tg_cb_t avrc_tg_callback;
    esp_a2d_source_data_cb_t source_data_callback;
} bt_manager_t;

esp_err_t bt_manager_init(bt_manager_t *mgr);
esp_err_t bt_manager_search_and_connect(bt_manager_t *mgr);

#endif
