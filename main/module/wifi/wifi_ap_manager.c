#include "wifi_ap_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static const char *s_tag = "wifi_ap";
static httpd_handle_t s_http_server;

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t html_len = (size_t)(index_html_end - index_html_start);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, html_len);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    static const char response[] = "{\"mode\":\"wifi_ap\"}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server(void)
{
    if (s_http_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_http_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(s_tag, "Falha ao iniciar HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &status));

    return ESP_OK;
}

esp_err_t wifi_ap_manager_start(const wifi_ap_manager_cfg_t *cfg)
{
    if (!cfg || !cfg->ssid || cfg->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->tag) {
        s_tag = cfg->tag;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(s_tag, "Falha esp_wifi_init: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.ap.ssid, cfg->ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(cfg->ssid);
    wifi_config.ap.channel = cfg->channel;
    wifi_config.ap.max_connection = cfg->max_connections;

    const char *password = cfg->password ? cfg->password : "";
    if (password[0] == '\0') {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        strlcpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password));
        wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(s_tag, "Falha esp_wifi_set_mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(s_tag, "Falha esp_wifi_set_config: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(s_tag, "Falha esp_wifi_start: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(s_tag, "WiFi AP ativo: SSID=%s, canal=%u, IP=http://192.168.4.1/",
             cfg->ssid,
             (unsigned)cfg->channel);

    return start_http_server();
}
