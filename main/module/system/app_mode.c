#include "app_mode.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#define APP_MODE_NVS_NAMESPACE "app_mode"
#define APP_MODE_WIFI_AP_KEY "wifi_ap"

static const char *TAG = "app_mode";

esp_err_t app_mode_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t app_mode_wifi_ap_is_enabled(bool *enabled)
{
    if (!enabled) {
        return ESP_ERR_INVALID_ARG;
    }

    *enabled = false;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(APP_MODE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t stored = 0;
    ret = nvs_get_u8(handle, APP_MODE_WIFI_AP_KEY, &stored);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret == ESP_OK) {
        *enabled = stored != 0;
    }
    return ret;
}

esp_err_t app_mode_wifi_ap_set_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(APP_MODE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(handle, APP_MODE_WIFI_AP_KEY, enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    return ret;
}

void app_mode_toggle_wifi_ap_and_restart(void)
{
    bool enabled = false;
    esp_err_t ret = app_mode_wifi_ap_is_enabled(&enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha lendo modo WiFi AP: %s", esp_err_to_name(ret));
        return;
    }

    ret = app_mode_wifi_ap_set_enabled(!enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha salvando modo WiFi AP: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGW(TAG, "Modo WiFi AP %s; reiniciando", enabled ? "desativado" : "ativado");
    esp_restart();
}
