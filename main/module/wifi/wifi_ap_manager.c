#include "wifi_ap_manager.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "system_config_manager.h"

#define MUSIC_UPLOAD_CHUNK_SIZE 1024
#define MUSIC_MAX_NAME_LEN 96
#define MUSIC_MAX_PATH_LEN 192
#define MUSIC_AUTHOR_LEN 128
#define MUSIC_LOGO_MIME_LEN 40
#define WIFI_CONFIG_MAX_BODY_SIZE 4096
#define WIFI_FILE_STREAM_CHUNK_SIZE 512

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static const char *s_tag = "wifi_ap";
static const char *s_mount_point = "/sdcard";
static httpd_handle_t s_http_server;

typedef struct {
    char author[MUSIC_AUTHOR_LEN];
    char logo_mime[MUSIC_LOGO_MIME_LEN];
    long logo_data_offset;
    size_t logo_data_size;
} music_metadata_t;

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

static bool is_unreserved_filename_char(char c)
{
    return isalnum((unsigned char)c) || c == ' ' || c == '.' || c == '_' || c == '-';
}

static int from_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_inplace(char *s)
{
    char *dst = s;
    char *src = s;

    while (*src) {
        if (src[0] == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            *dst++ = (char)((from_hex(src[1]) << 4) | from_hex(src[2]));
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool is_valid_music_filename(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }

    size_t len = strlen(name);
    if (len > MUSIC_MAX_NAME_LEN || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }

    if (len <= 4 || strcasecmp(name + len - 4, ".mp3") != 0) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (!is_unreserved_filename_char(name[i])) {
            return false;
        }
    }

    return true;
}

static esp_err_t make_music_path(const char *name, char *path, size_t path_len)
{
    if (!is_valid_music_filename(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(path, path_len, "%s/%s", s_mount_point, name);
    if (written < 0 || written >= (int)path_len) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t get_music_name_from_request(httpd_req_t *req, char *name, size_t name_len, bool allow_body)
{
    char query[160] = {0};
    char value[MUSIC_MAX_NAME_LEN + 1] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "nome", value, sizeof(value)) != ESP_OK &&
            httpd_query_key_value(query, "filename", value, sizeof(value)) != ESP_OK &&
            httpd_query_key_value(query, "name", value, sizeof(value)) != ESP_OK) {
            value[0] = '\0';
        }

        if (value[0] != '\0') {
            url_decode_inplace(value);
            if (!is_valid_music_filename(value)) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(name, value, name_len);
            return ESP_OK;
        }
    }

    if (httpd_req_get_hdr_value_str(req, "X-Filename", value, sizeof(value)) == ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-Music-Name", value, sizeof(value)) == ESP_OK) {
        url_decode_inplace(value);
        if (!is_valid_music_filename(value)) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(name, value, name_len);
        return ESP_OK;
    }

    if (allow_body && req->content_len > 0 && req->content_len < 256) {
        char body[256] = {0};
        size_t received = 0;

        while (received < req->content_len) {
            int ret = httpd_req_recv(req, body + received, req->content_len - received);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (ret <= 0) {
                return ESP_FAIL;
            }
            received += (size_t)ret;
        }

        cJSON *root = cJSON_ParseWithLength(body, received);
        if (!root) {
            return ESP_ERR_INVALID_ARG;
        }

        const cJSON *json_name = cJSON_GetObjectItemCaseSensitive(root, "nome");
        if (!cJSON_IsString(json_name) || !is_valid_music_filename(json_name->valuestring)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        strlcpy(name, json_name->valuestring, name_len);
        cJSON_Delete(root);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t send_error_json(httpd_req_t *req, int status, const char *message)
{
    char response[96];

    system_config_append_error(s_mount_point, s_tag, message);
    snprintf(response, sizeof(response), "{\"erro\":\"%s\"}", message);
    httpd_resp_set_status(req, status == 400 ? "400 Bad Request" :
                               status == 404 ? "404 Not Found" :
                               status == 409 ? "409 Conflict" :
                               status == 500 ? "500 Internal Server Error" :
                                               "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t stream_system_file(httpd_req_t *req, const char *file_name, const char *content_type)
{
    char path[MUSIC_MAX_PATH_LEN];
    char buffer[WIFI_FILE_STREAM_CHUNK_SIZE];

    esp_err_t ret = system_config_build_path(s_mount_point, file_name, path, sizeof(path));
    if (ret != ESP_OK) {
        return send_error_json(req, 500, "falha montando caminho do sistema");
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return send_error_json(req, 404, "arquivo do sistema nao encontrado");
    }

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    while (true) {
        size_t read = fread(buffer, 1, sizeof(buffer), file);
        if (read > 0) {
            ret = httpd_resp_send_chunk(req, buffer, read);
            if (ret != ESP_OK) {
                fclose(file);
                return ret;
            }
        }

        if (read < sizeof(buffer)) {
            if (ferror(file)) {
                fclose(file);
                return ESP_FAIL;
            }
            break;
        }
    }

    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t get_configs_handler(httpd_req_t *req)
{
    return stream_system_file(req, SYSTEM_CONFIG_FILE_NAME, "application/json");
}

static esp_err_t get_errors_handler(httpd_req_t *req)
{
    return stream_system_file(req, SYSTEM_ERROR_LOG_FILE_NAME, "application/x-ndjson");
}

static esp_err_t update_configs_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > WIFI_CONFIG_MAX_BODY_SIZE) {
        return send_error_json(req, 400, "json de config ausente ou muito grande");
    }

    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        return send_error_json(req, 500, "memoria insuficiente");
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            free(body);
            return send_error_json(req, 500, "falha recebendo config");
        }
        received += (size_t)ret;
    }

    esp_err_t ret = system_config_replace_json(s_mount_point, body, received, s_tag);
    free(body);

    if (ret == ESP_ERR_INVALID_ARG) {
        return send_error_json(req, 400, "json de config invalido");
    }
    if (ret != ESP_OK) {
        return send_error_json(req, 500, "falha salvando config");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t put_music_handler(httpd_req_t *req)
{
    char name[MUSIC_MAX_NAME_LEN + 1] = {0};
    char path[MUSIC_MAX_PATH_LEN];
    char temp_path[MUSIC_MAX_PATH_LEN + 8];
    char buffer[MUSIC_UPLOAD_CHUNK_SIZE];
    size_t total_received = 0;

    esp_err_t err = get_music_name_from_request(req, name, sizeof(name), false);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error_json(req, 400, "nome do arquivo ausente");
    }
    if (err != ESP_OK || make_music_path(name, path, sizeof(path)) != ESP_OK) {
        return send_error_json(req, 400, "nome de arquivo invalido");
    }

    int written = snprintf(temp_path, sizeof(temp_path), "%s.part", path);
    if (written < 0 || written >= (int)sizeof(temp_path)) {
        return send_error_json(req, 400, "caminho muito longo");
    }

    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        ESP_LOGE(s_tag, "Falha criando %s: errno=%d", temp_path, errno);
        return send_error_json(req, 500, "falha ao abrir arquivo no SD");
    }

    ESP_LOGI(s_tag, "Recebendo upload PUT: %s", name);

    while (true) {
        int ret = httpd_req_recv(req, buffer, sizeof(buffer));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            break;
        }

        if (fwrite(buffer, 1, (size_t)ret, file) != (size_t)ret) {
            ESP_LOGE(s_tag, "Falha escrevendo %s: errno=%d", temp_path, errno);
            fclose(file);
            remove(temp_path);
            return send_error_json(req, 500, "falha ao gravar arquivo no SD");
        }

        total_received += (size_t)ret;
        if (req->content_len > 0 && total_received >= req->content_len) {
            break;
        }
    }

    if (fclose(file) != 0) {
        remove(temp_path);
        return send_error_json(req, 500, "falha ao finalizar arquivo");
    }

    remove(path);
    if (rename(temp_path, path) != 0) {
        ESP_LOGE(s_tag, "Falha renomeando %s para %s: errno=%d", temp_path, path, errno);
        remove(temp_path);
        return send_error_json(req, 500, "falha ao confirmar upload");
    }

    ESP_LOGI(s_tag, "Upload concluido: %s (%u bytes)", name, (unsigned)total_received);

    char response[160];
    snprintf(response, sizeof(response), "{\"ok\":true,\"nome\":\"%s\",\"bytes\":%u}", name, (unsigned)total_received);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static uint32_t read_syncsafe_u32(const uint8_t *buf)
{
    return ((uint32_t)(buf[0] & 0x7F) << 21) |
           ((uint32_t)(buf[1] & 0x7F) << 14) |
           ((uint32_t)(buf[2] & 0x7F) << 7) |
           (uint32_t)(buf[3] & 0x7F);
}

static uint32_t read_be_u32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           (uint32_t)buf[3];
}

static void decode_id3_text(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    if (!data || len < 2 || !out || out_len == 0) {
        return;
    }

    uint8_t encoding = data[0];
    size_t src = 1;
    size_t dst = 0;

    if ((encoding == 1 || encoding == 2) && len >= 3) {
        if ((data[1] == 0xFF && data[2] == 0xFE) || (data[1] == 0xFE && data[2] == 0xFF)) {
            src = 3;
        }

        for (; src + 1 < len && dst + 1 < out_len; src += 2) {
            uint8_t ch = encoding == 2 ? data[src + 1] : data[src];
            if (ch == 0) {
                break;
            }
            out[dst++] = (char)(isprint(ch) ? ch : '?');
        }
    } else {
        for (; src < len && dst + 1 < out_len; src++) {
            if (data[src] == 0) {
                break;
            }
            out[dst++] = (char)data[src];
        }
    }

    while (dst > 0 && isspace((unsigned char)out[dst - 1])) {
        dst--;
    }
    out[dst] = '\0';
}

static void parse_apic_frame(FILE *file, long frame_data_offset, size_t frame_size, music_metadata_t *meta)
{
    uint8_t head[96];
    size_t to_read = frame_size < sizeof(head) ? frame_size : sizeof(head);

    if (fseek(file, frame_data_offset, SEEK_SET) != 0 || fread(head, 1, to_read, file) != to_read || to_read < 5) {
        return;
    }

    size_t pos = 1;
    size_t mime_start = pos;
    while (pos < to_read && head[pos] != '\0') {
        pos++;
    }
    if (pos >= to_read || pos == mime_start || pos + 2 >= frame_size) {
        return;
    }

    size_t mime_len = pos - mime_start;
    if (mime_len >= sizeof(meta->logo_mime)) {
        mime_len = sizeof(meta->logo_mime) - 1;
    }
    memcpy(meta->logo_mime, head + mime_start, mime_len);
    meta->logo_mime[mime_len] = '\0';

    uint8_t encoding = head[0];
    pos += 2; // NUL do MIME + picture type.
    while (pos < frame_size) {
        if (fseek(file, frame_data_offset + (long)pos, SEEK_SET) != 0) {
            return;
        }
        int ch = fgetc(file);
        if (ch == EOF) {
            return;
        }
        pos++;
        if ((encoding == 1 || encoding == 2) && ch == '\0') {
            int next = fgetc(file);
            if (next == EOF) {
                return;
            }
            pos++;
            if (next == '\0') {
                break;
            }
        } else if (ch == '\0') {
            break;
        }
    }

    if (pos < frame_size) {
        meta->logo_data_offset = frame_data_offset + (long)pos;
        meta->logo_data_size = frame_size - pos;
    }
}

static void read_music_metadata(const char *path, music_metadata_t *meta)
{
    uint8_t header[10];

    memset(meta, 0, sizeof(*meta));
    meta->logo_data_offset = -1;

    FILE *file = fopen(path, "rb");
    if (!file) {
        return;
    }

    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        fclose(file);
        return;
    }

    uint8_t version = header[3];
    uint32_t tag_size = read_syncsafe_u32(&header[6]);
    long tag_end = 10L + (long)tag_size;

    while (ftell(file) + 10 <= tag_end) {
        uint8_t frame_header[10];
        long frame_start = ftell(file);

        if (fread(frame_header, 1, sizeof(frame_header), file) != sizeof(frame_header)) {
            break;
        }
        if (frame_header[0] == 0) {
            break;
        }

        uint32_t frame_size = version == 4 ? read_syncsafe_u32(&frame_header[4]) : read_be_u32(&frame_header[4]);
        long frame_data_offset = frame_start + 10;
        if (frame_size == 0 || frame_data_offset + (long)frame_size > tag_end) {
            break;
        }

        if (memcmp(frame_header, "TPE1", 4) == 0 && meta->author[0] == '\0') {
            uint8_t text_buf[MUSIC_AUTHOR_LEN + 8];
            size_t to_read = frame_size < sizeof(text_buf) ? frame_size : sizeof(text_buf);

            if (fread(text_buf, 1, to_read, file) == to_read) {
                decode_id3_text(text_buf, to_read, meta->author, sizeof(meta->author));
            }
        } else if (memcmp(frame_header, "APIC", 4) == 0 && meta->logo_data_offset < 0) {
            parse_apic_frame(file, frame_data_offset, frame_size, meta);
        }

        if (meta->author[0] != '\0' && meta->logo_data_offset >= 0) {
            break;
        }

        if (fseek(file, frame_data_offset + (long)frame_size, SEEK_SET) != 0) {
            break;
        }
    }

    fclose(file);
}

static esp_err_t send_json_escaped_chunk(httpd_req_t *req, const char *value)
{
    char chunk[96];
    size_t used = 0;

    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        const char *escaped = NULL;
        char escape_buf[7];
        size_t escaped_len = 0;

        switch (*p) {
        case '"': escaped = "\\\""; escaped_len = 2; break;
        case '\\': escaped = "\\\\"; escaped_len = 2; break;
        case '\b': escaped = "\\b"; escaped_len = 2; break;
        case '\f': escaped = "\\f"; escaped_len = 2; break;
        case '\n': escaped = "\\n"; escaped_len = 2; break;
        case '\r': escaped = "\\r"; escaped_len = 2; break;
        case '\t': escaped = "\\t"; escaped_len = 2; break;
        default:
            if (*p < 0x20) {
                snprintf(escape_buf, sizeof(escape_buf), "\\u%04x", *p);
                escaped = escape_buf;
                escaped_len = 6;
            } else {
                escape_buf[0] = (char)*p;
                escaped = escape_buf;
                escaped_len = 1;
            }
            break;
        }

        if (used + escaped_len >= sizeof(chunk)) {
            esp_err_t ret = httpd_resp_send_chunk(req, chunk, used);
            if (ret != ESP_OK) {
                return ret;
            }
            used = 0;
        }

        memcpy(chunk + used, escaped, escaped_len);
        used += escaped_len;
    }

    if (used > 0) {
        return httpd_resp_send_chunk(req, chunk, used);
    }
    return ESP_OK;
}

static const char s_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t encode_base64_block(const uint8_t *src, size_t len, char *out)
{
    out[0] = s_b64[src[0] >> 2];
    out[1] = s_b64[((src[0] & 0x03) << 4) | (len > 1 ? (src[1] >> 4) : 0)];
    out[2] = len > 1 ? s_b64[((src[1] & 0x0F) << 2) | (len > 2 ? (src[2] >> 6) : 0)] : '=';
    out[3] = len > 2 ? s_b64[src[2] & 0x3F] : '=';
    return 4;
}

static esp_err_t send_logo_value(httpd_req_t *req, const char *path, const music_metadata_t *meta)
{
    if (meta->logo_data_offset < 0 || meta->logo_data_size == 0 || meta->logo_mime[0] == '\0') {
        return httpd_resp_send_chunk(req, "null", 4);
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return httpd_resp_send_chunk(req, "null", 4);
    }

    if (fseek(file, meta->logo_data_offset, SEEK_SET) != 0) {
        fclose(file);
        return httpd_resp_send_chunk(req, "null", 4);
    }

    char prefix[80];
    snprintf(prefix, sizeof(prefix), "\"data:%s;base64,", meta->logo_mime);
    esp_err_t ret = httpd_resp_send_chunk(req, prefix, strlen(prefix));
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    uint8_t in[192];
    char out[256];
    size_t remaining = meta->logo_data_size;

    while (remaining > 0) {
        size_t want = remaining < sizeof(in) ? remaining : sizeof(in);
        size_t got = fread(in, 1, want, file);
        if (got == 0) {
            break;
        }

        size_t out_len = 0;
        for (size_t i = 0; i < got; i += 3) {
            size_t block_len = got - i >= 3 ? 3 : got - i;
            out_len += encode_base64_block(in + i, block_len, out + out_len);
        }

        ret = httpd_resp_send_chunk(req, out, out_len);
        if (ret != ESP_OK) {
            fclose(file);
            return ret;
        }
        remaining -= got;
    }

    fclose(file);
    return httpd_resp_send_chunk(req, "\"", 1);
}

static bool is_mp3_entry(const struct dirent *entry)
{
    size_t len = strlen(entry->d_name);
    return entry->d_type == DT_REG && len > 4 && strcasecmp(entry->d_name + len - 4, ".mp3") == 0;
}

static esp_err_t send_music_item(httpd_req_t *req, const char *name, bool first)
{
    char path[MUSIC_MAX_PATH_LEN];
    music_metadata_t meta;

    if (make_music_path(name, path, sizeof(path)) != ESP_OK) {
        return ESP_OK;
    }

    read_music_metadata(path, &meta);

    esp_err_t ret = httpd_resp_send_chunk(req, first ? "{\"logo\":" : ",{\"logo\":", first ? 8 : 9);
    if (ret != ESP_OK) return ret;

    ret = send_logo_value(req, path, &meta);
    if (ret != ESP_OK) return ret;

    ret = httpd_resp_send_chunk(req, ",\"nome\":\"", 9);
    if (ret != ESP_OK) return ret;
    ret = send_json_escaped_chunk(req, name);
    if (ret != ESP_OK) return ret;

    if (meta.author[0] == '\0') {
        ret = httpd_resp_send_chunk(req, "\",\"author\":null}", 16);
    } else {
        ret = httpd_resp_send_chunk(req, "\",\"author\":\"", 12);
        if (ret != ESP_OK) return ret;
        ret = send_json_escaped_chunk(req, meta.author);
        if (ret != ESP_OK) return ret;
        ret = httpd_resp_send_chunk(req, "\"}", 2);
    }

    return ret;
}

static esp_err_t get_musics_handler(httpd_req_t *req)
{
    DIR *dir = opendir(s_mount_point);
    if (!dir) {
        return send_error_json(req, 500, "falha ao abrir SD");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t ret = httpd_resp_send_chunk(req, "{\"musicas\":[", 12);
    bool first = true;
    struct dirent *entry;

    while (ret == ESP_OK && (entry = readdir(dir)) != NULL) {
        if (!is_mp3_entry(entry) || !is_valid_music_filename(entry->d_name)) {
            continue;
        }

        ret = send_music_item(req, entry->d_name, first);
        first = false;
    }

    closedir(dir);

    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }

    return ret;
}

static esp_err_t delete_music_handler(httpd_req_t *req)
{
    char name[MUSIC_MAX_NAME_LEN + 1] = {0};
    char path[MUSIC_MAX_PATH_LEN];

    esp_err_t err = get_music_name_from_request(req, name, sizeof(name), true);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error_json(req, 400, "nome do arquivo ausente");
    }
    if (err != ESP_OK || make_music_path(name, path, sizeof(path)) != ESP_OK) {
        return send_error_json(req, 400, "nome de arquivo invalido");
    }

    if (remove(path) != 0) {
        if (errno == ENOENT) {
            return send_error_json(req, 404, "musica nao encontrada");
        }
        ESP_LOGE(s_tag, "Falha removendo %s: errno=%d", path, errno);
        return send_error_json(req, 500, "falha ao remover musica");
    }

    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":true,\"nome\":\"%s\"}", name);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

esp_err_t stream_handler(httpd_req_t *req) {
    char chunk[32];

    httpd_resp_set_type(req, "text/plain");

    for (int i = 0; i < 10; i++) {
        sprintf(chunk, "Dados do sensor: %d\n", i);
        if (httpd_resp_send_chunk(req, chunk, strlen(chunk)) != ESP_OK) {
            return ESP_FAIL;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

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
    config.max_uri_handlers = 16;
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
    const httpd_uri_t get_configs = {
        .uri = "/get-configs",
        .method = HTTP_GET,
        .handler = get_configs_handler,
    };
    const httpd_uri_t put_configs = {
        .uri = "/configs",
        .method = HTTP_PUT,
        .handler = update_configs_handler,
    };
    const httpd_uri_t patch_configs = {
        .uri = "/configs",
        .method = HTTP_PATCH,
        .handler = update_configs_handler,
    };
    const httpd_uri_t get_errors = {
        .uri = "/get-errors",
        .method = HTTP_GET,
        .handler = get_errors_handler,
    };
    const httpd_uri_t put_musics = {
        .uri = "/put-musics",
        .method = HTTP_PUT,
        .handler = put_music_handler,
    };
    const httpd_uri_t put_music = {
        .uri = "/musics",
        .method = HTTP_PUT,
        .handler = put_music_handler,
    };
    const httpd_uri_t get_musics = {
        .uri = "/musics",
        .method = HTTP_GET,
        .handler = get_musics_handler,
    };
    const httpd_uri_t delete_music = {
        .uri = "/musics",
        .method = HTTP_DELETE,
        .handler = delete_music_handler,
    };
    const httpd_uri_t stream = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &get_configs));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &put_configs));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &patch_configs));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &get_errors));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &put_musics));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &put_music));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &get_musics));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &delete_music));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &stream));

    return ESP_OK;
}

esp_err_t wifi_ap_manager_start(const wifi_ap_manager_cfg_t *cfg)
{
    if (!cfg || !cfg->ssid || cfg->ssid[0] == '\0' || !cfg->mount_point || cfg->mount_point[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->tag) {
        s_tag = cfg->tag;
    }
    s_mount_point = cfg->mount_point;

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
