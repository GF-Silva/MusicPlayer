#include "wifi_ap_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "cJSON.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static const char *s_tag = "wifi_ap";
static httpd_handle_t s_http_server;

static char *format_json(int count, ...)
{
    va_list args;
    va_start(args, count);

    cJSON *root = cJSON_CreateObject();

    for (int i = 0; i < count; i++) {
        const char *key   = va_arg(args, const char *);
        const char *value = va_arg(args, const char *);
        cJSON_AddStringToObject(root, key, value);
    }

    va_end(args);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller faz cJSON_free()
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t html_len = (size_t)(index_html_end - index_html_start);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, html_len);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char *response = format_json(1, "mode", "wifi_ap");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    cJSON_free(response);
    return ret;
}

static esp_err_t put_music_handler(httpd_req_t *req)
{
    char buffer[1024]; // Aumente um pouco o buffer de leitura
    int ret;
    int total_received = 0;

    ESP_LOGI(s_tag, "Iniciando recebimento de stream PUT...");

    // Loop de recepção
    while (1) {
        // httpd_req_recv lê os dados decodificando o chunked automaticamente para você
        ret = httpd_req_recv(req, buffer, sizeof(buffer));

        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                // Timeout no socket, tenta de novo se necessário
                continue;
            }
            // Se ret == 0, o stream acabou corretamente
            break;
        }

        total_received += ret;
        
        // Aqui você processaria o dado (ex: gravar no SD Card)
        // Para teste, apenas logamos
        ESP_LOGI(s_tag, "Recebido: %d bytes | Total: %d | Status: Processado", ret, total_received);
    }

    ESP_LOGI(s_tag, "Stream finalizado. Total recebido: %d bytes", total_received);

    // IMPORTANTE: Responder apenas DEPOIS que o loop terminar
    httpd_resp_sendstr(req, "Upload concluído com sucesso!");
    return ESP_OK;
}

esp_err_t stream_handler(httpd_req_t *req) {
    char chunk[32];
    
    // Opcional: Definir o tipo de conteúdo
    httpd_resp_set_type(req, "text/plain");

    for (int i = 0; i < 10; i++) {
        sprintf(chunk, "Dados do sensor: %d\n", i);
        
        // Envia uma parte. O esp_http_server cuida do Chunked Encoding
        // se você não definir o Content-Length antes.
        if (httpd_resp_send_chunk(req, chunk, strlen(chunk)) != ESP_OK) {
            return ESP_FAIL; // Se a conexão cair, interrompe
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000)); // Espera 1s
    }

    // O ENVIO FINAL: Um chunk vazio encerra a transmissão (envia o chunk '0')
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    if (s_http_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.recv_wait_timeout = 10; // Aumente o timeout
    config.send_wait_timeout = 10;
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
    const httpd_uri_t put_musics = {
        .uri = "/put-musics",
        .method = HTTP_PUT,
        .handler = put_music_handler,
    };
    const httpd_uri_t stream = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &put_musics));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &stream));

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
    esp_wifi_set_max_tx_power(84);

    ESP_LOGI(s_tag, "WiFi AP ativo: SSID=%s, canal=%u, IP=http://192.168.4.1/",
             cfg->ssid,
             (unsigned)cfg->channel);

    return start_http_server();
}
