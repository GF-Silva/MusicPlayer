#include "bt_manager.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#define DISCOVERY_INQ_LEN 10

static void bt_apply_default_pt_filter(void)
{
    esp_avrc_psth_bit_mask_t cmd_set = {0};
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PLAY);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PAUSE);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_STOP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_BACKWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_VOL_UP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_VOL_DOWN);
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_set);
}

esp_err_t bt_manager_init(bt_manager_t *mgr)
{
    if (!mgr || !mgr->tag || !mgr->bt_initialized || !mgr->gap_callback || !mgr->a2dp_callback ||
        !mgr->avrc_ct_callback || !mgr->avrc_tg_callback || !mgr->source_data_callback ||
        !mgr->set_bt_connecting || !mgr->log_bt_state) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(mgr->tag, "Inicializando Bluetooth...");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(mgr->tag, "BT controller init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(mgr->tag, "BT controller enable: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(mgr->tag, "Bluedroid init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(mgr->tag, "Bluedroid enable: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_gap_set_device_name("ESP32_MP3");
    if (ret != ESP_OK) {
        ESP_LOGW(mgr->tag, "BT set device name falhou: %s", esp_err_to_name(ret));
    }

    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    esp_bt_gap_register_callback(mgr->gap_callback);

    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(mgr->avrc_ct_callback);
    esp_avrc_tg_init();
    esp_avrc_tg_register_callback(mgr->avrc_tg_callback);
    bt_apply_default_pt_filter();

    esp_a2d_register_callback(mgr->a2dp_callback);
    esp_a2d_source_register_data_callback(mgr->source_data_callback);
    esp_a2d_source_init();

    *mgr->bt_initialized = true;
    return ESP_OK;
}

esp_err_t bt_manager_search_and_connect(bt_manager_t *mgr)
{
    if (!mgr || !mgr->bt_connected || !mgr->bt_connecting || !mgr->device_found ||
        !mgr->connect_after_discovery_stop || !mgr->discovery_stop_pending ||
        !mgr->set_bt_connecting || !mgr->discovery_active || !mgr->log_bt_state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (*mgr->bt_connected || *mgr->bt_connecting) {
        return ESP_OK;
    }
    if (*mgr->discovery_active) {
        return ESP_OK;
    }

    ESP_LOGI(mgr->tag, "Iniciando busca Bluetooth...");

    *mgr->device_found = false;
    *mgr->connect_after_discovery_stop = false;
    *mgr->discovery_stop_pending = false;

    mgr->set_bt_connecting(true);

    esp_err_t disc_err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                                    DISCOVERY_INQ_LEN,
                                                    0);
    if (disc_err != ESP_OK) {
        ESP_LOGW(mgr->tag, "Falha ao iniciar discovery: %s", esp_err_to_name(disc_err));
        mgr->set_bt_connecting(false);
        return disc_err;
    }

    if (mgr->discovery_timer) {
        xTimerStart(mgr->discovery_timer, 0);
    }
    mgr->log_bt_state("search_start");

    return ESP_OK;
}
