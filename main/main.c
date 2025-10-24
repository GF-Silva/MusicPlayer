#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "nvs_flash.h"

// Configurações do SD Card
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   4
#define MOUNT_POINT "/sdcard"

// Pinos de controle de volume
#define PIN_VOL_UP   21
#define PIN_VOL_DOWN 22
#define DEBOUNCE_TIME_MS 50
#define VOLUME_STEP 5  // Incremento de 5%

#define DOUBLE_CLICK_INTERVAL_MS 300  // Tempo máximo entre cliques (300ms = rápido)
#define LONG_CLICK_THRESHOLD_MS 400   // Acima disso é considerado clique longo

// Configurações otimizadas para RAM LIMITADA do ESP32
#define MAX_PATH_LEN 140

// BUFFER ÚNICO SIMPLIFICADO - Economia de ~40KB
#define AUDIO_READ_BUFFER_SIZE    16384     // 16KB - leituras maiores do SD
#define STREAM_BUFFER_SIZE        49152     // 48KB - buffer único principal
#define A2DP_CHUNK_SIZE           512        

// Total RAM buffers: ~65KB (deixa ~140KB para Bluetooth)
#define STREAM_REFILL_THRESHOLD   (STREAM_BUFFER_SIZE / 4)
#define TARGET_DEVICE_NAME "TWS V5.3"
#define CONNECTION_RETRY_MAX 5
#define FILE_READ_RETRY_MAX 3
#define DISCOVERY_TIMEOUT_SEC 45

// Sample rates suportados
#define SAMPLE_RATE_44K1 44100
#define SAMPLE_RATE_48K  48000

// Estrutura do header WAV
typedef struct {
    char riff_header[4];
    uint32_t wav_size;
    char wave_header[4];
    char fmt_header[4];
    uint32_t fmt_chunk_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t sample_alignment;
    uint16_t bit_depth;
    char data_header[4];
    uint32_t data_bytes;
} __attribute__((packed)) wav_header_t;

typedef struct {
    bool is_valid_wav;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_size;
    size_t data_start_offset;
    uint32_t duration_seconds;
    bool a2dp_compatible;
} wav_info_t;

static const char *TAG = "WAVPlayer";

typedef struct {
    bool sd_mounted;
    bool bt_initialized;
    bool bt_connected;
    bool audio_playing;
    bool system_ready;
    bool codec_configured;
    bool streaming_active;
    int connection_retries;
    int file_errors;
} system_status_t;

static system_status_t sys_status = {0};

// Controle de volume (0-100%)
static uint8_t current_volume = 30;  // Volume inicial 30%
static float volume_scale = 0.30f;   // = current_volume / 100.0

// Variáveis globais
static esp_bd_addr_t target_device_addr;
static bool device_found = false;
static int wav_count = 0;
static int current_track = 0;
static FILE *current_file = NULL;
static wav_info_t current_wav_info = {0};
static TaskHandle_t file_reader_task_handle = NULL;

// Ring buffer
typedef struct {
    uint8_t *data;
    size_t size;
    size_t write_pos;
    size_t read_pos;
    size_t available;
    SemaphoreHandle_t mutex;
    bool is_full;
    bool end_of_stream;
} ring_buffer_t;

static ring_buffer_t *stream_buffer = NULL;
static uint8_t *file_read_buffer = NULL;

static uint32_t total_bytes_streamed = 0;
static uint32_t underrun_count = 0;
static uint32_t callback_count = 0;
static bool stream_stabilized = false;

// Sincronização
static EventGroupHandle_t player_event_group;
#define BT_CONNECTED_BIT    BIT0
#define TRACK_FINISHED_BIT  BIT1
#define PLAY_NEXT_BIT       BIT2
#define ERROR_BIT           BIT3
#define CODEC_READY_BIT     BIT4
#define STREAM_READY_BIT    BIT5

static QueueHandle_t control_queue;
typedef enum {
    CMD_PLAY_NEXT,
    CMD_STOP,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_RETRY_CONNECTION,
    CMD_RESTART_DISCOVERY,
    CMD_FILL_BUFFERS,
    CMD_TOGGLE_PAUSE
} player_cmd_t;

static TimerHandle_t connection_timer;
static TimerHandle_t discovery_timer;
static TimerHandle_t buffer_monitor_timer;

// Detector de cliques múltiplos
static uint8_t click_count = 0;
static TickType_t last_click_time = 0;
#define CLICK_TIMEOUT_MS 500  // 500ms entre cliques
static bool first_command_received = false;

// Estado dos botões de volume
typedef struct {
    uint8_t pin;
    bool last_state;
    bool pressed;
    TickType_t last_change_time;
    TickType_t press_start_time;
    TickType_t last_click_time;
    uint8_t click_count;
    bool is_long_press;
} button_state_t;

static button_state_t vol_up_btn = {PIN_VOL_UP, true, false, 0, 0, 0, 0, false};
static button_state_t vol_down_btn = {PIN_VOL_DOWN, true, false, 0, 0, 0, 0, false};

// ============================================================================
// CONTROLE DE VOLUME
// ============================================================================

static void init_volume_buttons(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_VOL_UP) | (1ULL << PIN_VOL_DOWN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    ESP_LOGI(TAG, "Botões de volume inicializados (UP:%d DOWN:%d)", PIN_VOL_UP, PIN_VOL_DOWN);
    ESP_LOGI(TAG, "Volume inicial: %d%%", current_volume);
}

static void update_volume_scale(void)
{
    volume_scale = current_volume / 100.0f;
    ESP_LOGI(TAG, "Volume ajustado: %d%% (scale: %.2f)", current_volume, volume_scale);
}

static bool read_button_with_debounce(button_state_t *btn, bool *is_double_click, bool *is_single_click)
{
    bool current_state = gpio_get_level(btn->pin);
    TickType_t current_time = xTaskGetTickCount();
    
    *is_double_click = false;
    *is_single_click = false;
    
    // Detecta mudança de estado
    if (current_state != btn->last_state) {
        // Verifica debounce
        if ((current_time - btn->last_change_time) * portTICK_PERIOD_MS >= DEBOUNCE_TIME_MS) {
            btn->last_state = current_state;
            btn->last_change_time = current_time;
            
            // Botão PRESSIONADO (nível BAIXO)
            if (current_state == 0 && !btn->pressed) {
                btn->pressed = true;
                btn->press_start_time = current_time;
                btn->is_long_press = false;
                return true;  // Botão foi pressionado
            } 
            // Botão LIBERADO (nível ALTO)
            else if (current_state == 1 && btn->pressed) {
                btn->pressed = false;
                
                TickType_t press_duration = (current_time - btn->press_start_time) * portTICK_PERIOD_MS;
                
                // Se foi um clique CURTO (não foi segurado)
                if (press_duration < LONG_CLICK_THRESHOLD_MS && !btn->is_long_press) {
                    TickType_t time_since_last_click = (current_time - btn->last_click_time) * portTICK_PERIOD_MS;
                    
                    // Se clicou dentro do intervalo de duplo clique
                    if (time_since_last_click < DOUBLE_CLICK_INTERVAL_MS && btn->click_count == 1) {
                        btn->click_count = 0;  // Reset
                        *is_double_click = true;
                        ESP_LOGI(TAG, "🎵 DUPLO CLIQUE detectado! (intervalo: %lums)", (unsigned long)time_since_last_click);
                        return true;
                    } else {
                        // Primeiro clique ou clique isolado
                        btn->click_count = 1;
                        btn->last_click_time = current_time;
                    }
                }
            }
        }
    }
    
    // Verifica se está segurando o botão (clique longo)
    if (btn->pressed && !btn->is_long_press) {
        TickType_t hold_time = (current_time - btn->press_start_time) * portTICK_PERIOD_MS;
        if (hold_time >= LONG_CLICK_THRESHOLD_MS) {
            btn->is_long_press = true;
            btn->click_count = 0;  // Cancela detecção de duplo clique
            return true;  // Indica que é um clique longo
        }
    }
    
    // Timeout do primeiro clique (se passou muito tempo, é clique único)
    if (btn->click_count == 1) {
        TickType_t time_since_click = (current_time - btn->last_click_time) * portTICK_PERIOD_MS;
        if (time_since_click >= DOUBLE_CLICK_INTERVAL_MS) {
            btn->click_count = 0;
            *is_single_click = true;
            return true;
        }
    }
    
    return false;
}

void volume_control_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Task de controle de volume iniciada");
    
    while (1) {
        bool vol_up_double_click = false;
        bool vol_up_single_click = false;
        bool vol_down_double_click = false;
        bool vol_down_single_click = false;
        
        read_button_with_debounce(&vol_up_btn, &vol_up_double_click, &vol_up_single_click);
        read_button_with_debounce(&vol_down_btn, &vol_down_double_click, &vol_down_single_click);
        
        // ========== VOLUME UP ==========
        // DUPLO CLIQUE → Próxima música
        if (vol_up_double_click) {
            ESP_LOGI(TAG, "⏭️  PRÓXIMA MÚSICA (duplo clique)");
            player_cmd_t cmd = CMD_PLAY_NEXT;
            xQueueSend(control_queue, &cmd, 0);
        }
        // CLIQUE ÚNICO → Aumentar volume UMA VEZ
        else if (vol_up_single_click) {
            if (current_volume < 100) {
                current_volume += VOLUME_STEP;
                if (current_volume > 100) current_volume = 100;
                update_volume_scale();
            } else {
                ESP_LOGW(TAG, "Volume já está no máximo (100%%)");
            }
        }
        
        // ========== VOLUME DOWN ==========
        // CLIQUE ÚNICO → Diminuir volume UMA VEZ
        if (vol_down_single_click) {
            if (current_volume > 0) {
                if (current_volume >= VOLUME_STEP) {
                    current_volume -= VOLUME_STEP;
                } else {
                    current_volume = 0;
                }
                update_volume_scale();
            } else {
                ESP_LOGW(TAG, "Volume já está no mínimo (0%%)");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================================
// RING BUFFER
// ============================================================================

static ring_buffer_t* create_ring_buffer(size_t size)
{
    ring_buffer_t *rb = malloc(sizeof(ring_buffer_t));
    if (!rb) {
        ESP_LOGE(TAG, "Falha ao alocar ring buffer structure");
        return NULL;
    }
    
    rb->data = heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!rb->data) {
        free(rb);
        ESP_LOGE(TAG, "Falha ao alocar ring buffer data: %zu bytes", size);
        return NULL;
    }
    
    rb->size = size;
    rb->write_pos = 0;
    rb->read_pos = 0;
    rb->available = 0;
    rb->is_full = false;
    rb->end_of_stream = false;
    rb->mutex = xSemaphoreCreateMutex();
    
    if (!rb->mutex) {
        free(rb->data);
        free(rb);
        ESP_LOGE(TAG, "Falha ao criar mutex do ring buffer");
        return NULL;
    }
    
    memset(rb->data, 0, size);
    ESP_LOGI(TAG, "Ring buffer criado: %zu bytes", size);
    return rb;
}

static void destroy_ring_buffer(ring_buffer_t *rb)
{
    if (rb) {
        if (rb->mutex) vSemaphoreDelete(rb->mutex);
        if (rb->data) free(rb->data);
        free(rb);
    }
}

static size_t rb_write(ring_buffer_t *rb, const uint8_t *data, size_t len)
{
    if (!rb || !data || len == 0) return 0;
    
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return 0;
    }
    
    size_t space_available = rb->size - rb->available;
    size_t to_write = (len > space_available) ? space_available : len;
    size_t written = 0;
    
    while (written < to_write) {
        size_t chunk = to_write - written;
        size_t space_to_end = rb->size - rb->write_pos;
        
        if (chunk > space_to_end) {
            chunk = space_to_end;
        }
        
        memcpy(rb->data + rb->write_pos, data + written, chunk);
        rb->write_pos = (rb->write_pos + chunk) % rb->size;
        written += chunk;
    }
    
    rb->available += written;
    rb->is_full = (rb->available == rb->size);
    
    xSemaphoreGive(rb->mutex);
    return written;
}

static size_t rb_available(ring_buffer_t *rb)
{
    if (!rb) return 0;
    
    size_t available;
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(1)) == pdTRUE) {
        available = rb->available;
        xSemaphoreGive(rb->mutex);
    } else {
        available = 0;
    }
    return available;
}

static size_t rb_free_space(ring_buffer_t *rb)
{
    if (!rb) return 0;
    return rb->size - rb_available(rb);
}

static void rb_clear(ring_buffer_t *rb)
{
    if (!rb) return;
    
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        rb->read_pos = 0;
        rb->write_pos = 0;
        rb->available = 0;
        rb->is_full = false;
        rb->end_of_stream = false;
        xSemaphoreGive(rb->mutex);
    }
}

// ============================================================================
// FUNÇÕES WAV
// ============================================================================

esp_err_t count_wav_files(void)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) return ESP_ERR_NOT_FOUND;
    
    wav_count = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir))) {
        if (entry->d_type == DT_DIR || entry->d_name[0] == '.') continue;
        
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(entry->d_name + len - 4, ".wav") == 0) {
            wav_count++;
        }
    }
    
    closedir(dir);
    ESP_LOGI(TAG, "Total de WAVs encontrados: %d", wav_count);
    return ESP_OK;
}

char* get_wav_by_index(int index, char* filepath, size_t filepath_size)
{
    if (index < 0 || index >= wav_count) return NULL;
    
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) return NULL;
    
    int current_index = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir))) {
        if (entry->d_type == DT_DIR || entry->d_name[0] == '.') continue;
        
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(entry->d_name + len - 4, ".wav") == 0) {
            if (current_index == index) {
                snprintf(filepath, filepath_size, MOUNT_POINT "/%s", entry->d_name);
                closedir(dir);
                return filepath;
            }
            current_index++;
        }
    }
    
    closedir(dir);
    return NULL;
}

static bool is_sample_rate_supported(uint32_t sample_rate) 
{
    return (sample_rate == SAMPLE_RATE_44K1 || sample_rate == SAMPLE_RATE_48K);
}

static esp_err_t analyze_wav_file(const char *filepath, wav_info_t *info)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Erro ao abrir: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }
    
    memset(info, 0, sizeof(wav_info_t));
    
    uint8_t riff_header[12];
    if (fread(riff_header, 1, 12, f) != 12) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    
    if (memcmp(riff_header, "RIFF", 4) != 0 || memcmp(riff_header + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Header RIFF/WAVE inválido");
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    uint32_t sample_rate = 0, data_size = 0;
    uint16_t channels = 0, bits_per_sample = 0, audio_format = 0;
    size_t data_offset = 0;
    bool found_fmt = false, found_data = false;
    
    fseek(f, 12, SEEK_SET);
    
    while (!feof(f) && (!found_fmt || !found_data)) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, 8, f) != 8) break;
        
        uint32_t chunk_size = *((uint32_t*)(chunk_header + 4));
        size_t current_pos = ftell(f);
        
        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16) break;
            
            uint8_t fmt_data[16];
            if (fread(fmt_data, 1, 16, f) != 16) break;
            
            audio_format = *((uint16_t*)fmt_data);
            channels = *((uint16_t*)(fmt_data + 2));
            sample_rate = *((uint32_t*)(fmt_data + 4));
            bits_per_sample = *((uint16_t*)(fmt_data + 14));
            
            found_fmt = true;
            if (chunk_size > 16) {
                fseek(f, current_pos + chunk_size, SEEK_SET);
            }
            
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            data_size = chunk_size;
            data_offset = current_pos;
            found_data = true;
            fseek(f, current_pos + chunk_size, SEEK_SET);
            
        } else {
            fseek(f, current_pos + chunk_size, SEEK_SET);
        }
    }
    
    fclose(f);
    
    if (!found_fmt || !found_data || audio_format != 1) {
        ESP_LOGE(TAG, "WAV inválido ou não-PCM");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    info->is_valid_wav = true;
    info->sample_rate = sample_rate;
    info->channels = channels;
    info->bits_per_sample = bits_per_sample;
    info->data_size = data_size;
    info->data_start_offset = data_offset;
    
    info->a2dp_compatible = is_sample_rate_supported(sample_rate) &&
                           (channels == 1 || channels == 2) &&
                           (bits_per_sample == 16);
    
    if (sample_rate > 0 && channels > 0 && bits_per_sample > 0) {
        uint32_t bytes_per_second = sample_rate * channels * (bits_per_sample / 8);
        if (bytes_per_second > 0) {
            info->duration_seconds = data_size / bytes_per_second;
        }
    }
    
    return ESP_OK;
}

// ============================================================================
// CONVERSÃO DE ÁUDIO
// ============================================================================

static size_t convert_wav_to_a2dp_batch(const uint8_t *input_data, size_t input_len, 
                                        uint8_t *output_data, size_t max_output_len,
                                        const wav_info_t *wav_info)
{
    if (!wav_info || !wav_info->is_valid_wav || input_len == 0) {
        size_t copy_len = (input_len < max_output_len) ? input_len : max_output_len;
        memcpy(output_data, input_data, copy_len);
        return copy_len;
    }
    
    size_t output_len = 0;
    
    if (wav_info->bits_per_sample == 16 && wav_info->channels == 2) {
        // Estéreo - já está no formato correto, apenas aplica volume
        size_t samples = input_len / 4;
        size_t max_samples = max_output_len / 4;
        size_t process_samples = (samples < max_samples) ? samples : max_samples;
        
        const int16_t *input_samples = (const int16_t*)input_data;
        int16_t *output_samples = (int16_t*)output_data;
        
        for (size_t i = 0; i < process_samples * 2; i += 2) {
            int32_t left = (int32_t)(input_samples[i] * volume_scale);
            int32_t right = (int32_t)(input_samples[i + 1] * volume_scale);

            if (left > 32000) left = 32000;
            else if (left < -32000) left = -32000;
            if (right > 32000) right = 32000;
            else if (right < -32000) right = -32000;

            output_samples[i] = (int16_t)left;
            output_samples[i + 1] = (int16_t)right;
        }
        output_len = process_samples * 4;
        
    } else if (wav_info->bits_per_sample == 16 && wav_info->channels == 1) {
        // Mono - duplica canal e aplica volume
        size_t input_samples = input_len / 2;
        size_t max_output_samples = max_output_len / 4;
        size_t process_samples = (input_samples < max_output_samples) ? input_samples : max_output_samples;
        
        const int16_t *input = (const int16_t*)input_data;
        int16_t *output = (int16_t*)output_data;
        
        for (size_t i = 0; i < process_samples; i++) {
            int32_t sample = (int32_t)(input[i] * volume_scale);
            
            if (sample > 32000) sample = 32000;
            else if (sample < -32000) sample = -32000;
            
            output[i * 2] = (int16_t)sample;
            output[i * 2 + 1] = (int16_t)sample;
        }
        output_len = process_samples * 4;
        
    } else {
        // Formato não suportado → envia silêncio
        size_t fill_len = (input_len < max_output_len) ? input_len : max_output_len;
        memset(output_data, 0, fill_len);
        output_len = fill_len;
    }
    
    return output_len;
}

// ============================================================================
// TASK UNIFICADA DE LEITURA
// ============================================================================

void file_reader_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Task de leitura unificada iniciada");

    TickType_t last_stats_time = 0;
    uint32_t total_read = 0;

    uint8_t *convert_buffer = heap_caps_malloc(AUDIO_READ_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (!convert_buffer) {
        ESP_LOGE(TAG, "Falha ao alocar buffer de conversão");
        file_reader_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    const TickType_t LOOP_DELAY = pdMS_TO_TICKS(5);

    while (1) {
        if (!sys_status.streaming_active || !current_file || !stream_buffer) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t stream_available = rb_available(stream_buffer);
        size_t stream_free = rb_free_space(stream_buffer);
        float fill_percent = (stream_available * 100.0f) / stream_buffer->size;

        if (fill_percent > 85.0f) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        stream_free = rb_free_space(stream_buffer) & ~0x03;
        size_t read_size;

        if (fill_percent < 40.0f) {
            read_size = (AUDIO_READ_BUFFER_SIZE < stream_free) ? AUDIO_READ_BUFFER_SIZE : stream_free;
        } else {
            float fill_ratio = (fill_percent - 40.0f) / (85.0f - 40.0f);
            size_t min_read = 1024;
            size_t max_read = AUDIO_READ_BUFFER_SIZE;
            read_size = min_read + (size_t)((max_read - min_read) * (1.0f - fill_ratio));

            if (read_size > stream_free) read_size = stream_free;
        }

        read_size &= ~0x03;

        size_t bytes_read = fread(file_read_buffer, 1, read_size, current_file);
        if (bytes_read == 0) {
            if (feof(current_file)) {
                ESP_LOGI(TAG, "Fim do arquivo - próxima música");
                stream_buffer->end_of_stream = true;
                player_cmd_t cmd = CMD_PLAY_NEXT;
                xQueueSend(control_queue, &cmd, 0);
                free(convert_buffer);
                file_reader_task_handle = NULL;
                vTaskDelete(NULL);
                return;
            } else {
                ESP_LOGW(TAG, "Erro leitura SD");
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }

        total_read += bytes_read;

        size_t converted_len;
        const uint8_t *data_to_write;

        if (current_wav_info.is_valid_wav &&
            (current_wav_info.channels == 1 || current_wav_info.bits_per_sample != 16)) {
            converted_len = convert_wav_to_a2dp_batch(file_read_buffer, bytes_read,
                                                      convert_buffer, read_size,
                                                      &current_wav_info);
            data_to_write = convert_buffer;
        } else {
            converted_len = bytes_read;
            int16_t *samples = (int16_t*)file_read_buffer;
            size_t num_samples = bytes_read / 2;
            for (size_t i = 0; i < num_samples; i++) {
                int32_t sample = (int32_t)(samples[i] * volume_scale);
                if (sample > 32000) sample = 32000;
                else if (sample < -32000) sample = -32000;
                samples[i] = (int16_t)sample;
            }
            data_to_write = file_read_buffer;
        }

        size_t written = rb_write(stream_buffer, data_to_write, converted_len);
        if (written < converted_len) {
            ESP_LOGW(TAG, "Buffer cheio! Bytes perdidos: %zu (buf: %.0f%%)", converted_len - written, fill_percent);
        }

        TickType_t current_time = xTaskGetTickCount();
        if (current_time - last_stats_time > pdMS_TO_TICKS(10000)) {
            ESP_LOGI(TAG, "Leitura total: %lu KB, buf: %.0f%%, read_size: %zu, espaço livre: %zu",
                     total_read / 1024, fill_percent, read_size, stream_free);
            last_stats_time = current_time;
        }

        vTaskDelay(LOOP_DELAY);
    }
}

// ============================================================================
// CALLBACK A2DP OTIMIZADO
// ============================================================================

static int32_t bt_a2dp_source_data_cb(uint8_t *data, int32_t len)
{
    static uint8_t silence_packets = 0;
    callback_count++;
    
    if (!data || len <= 0) return 0;
    
    if (!sys_status.bt_connected || !sys_status.streaming_active) {
        memset(data, 0, len);
        return len;
    }
    
    if (!stream_stabilized) {
        if (silence_packets < 12) {
            memset(data, 0, len);
            silence_packets++;
            return len;
        }
        stream_stabilized = true;
        ESP_LOGI(TAG, "Stream estabilizado - buffer: %zu bytes", 
                rb_available(stream_buffer));
    }
    
    if (!stream_buffer) {
        memset(data, 0, len);
        return len;
    }
    
    BaseType_t taken = xSemaphoreTake(stream_buffer->mutex, 0);
    if (taken != pdTRUE) {
        taken = xSemaphoreTake(stream_buffer->mutex, 1);
        if (taken != pdTRUE) {
            memset(data, 0, len);
            underrun_count++;
            return len;
        }
    }
    
    size_t available = stream_buffer->available;
    
    if (available == 0) {
        xSemaphoreGive(stream_buffer->mutex);
        underrun_count++;
        memset(data, 0, len);
        return len;
    }
    
    size_t to_read = (len > available) ? available : len;
    size_t read_total = 0;
    
    while (read_total < to_read) {
        size_t chunk = to_read - read_total;
        size_t data_to_end = stream_buffer->size - stream_buffer->read_pos;
        
        if (chunk > data_to_end) chunk = data_to_end;
        
        memcpy(data + read_total, stream_buffer->data + stream_buffer->read_pos, chunk);
        stream_buffer->read_pos = (stream_buffer->read_pos + chunk) % stream_buffer->size;
        read_total += chunk;
    }
    
    stream_buffer->available -= read_total;
    stream_buffer->is_full = false;
    
    xSemaphoreGive(stream_buffer->mutex);
    
    total_bytes_streamed += read_total;
    
    if (read_total < (size_t)len) {
        memset(data + read_total, 0, len - read_total);
        underrun_count++;
    }
    
    if (callback_count % 5000 == 0) {
        ESP_LOGI(TAG, "A2DP #%lu: %lu KB, underruns: %lu, buf: %zu",
                callback_count, total_bytes_streamed / 1024, 
                underrun_count, available);
    }
    
    return len;
}

// ============================================================================
// CALLBACKS BLUETOOTH
// ============================================================================

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (!param) {
        ESP_LOGE(TAG, "Parâmetro GAP callback é NULL");
        return;
    }
    
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        {
            esp_bt_gap_dev_prop_t *prop = NULL;
            char bda_str[18] = {0};
            
            sprintf(bda_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                   param->disc_res.bda[0], param->disc_res.bda[1],
                   param->disc_res.bda[2], param->disc_res.bda[3],
                   param->disc_res.bda[4], param->disc_res.bda[5]);
            
            for (int i = 0; i < param->disc_res.num_prop; i++) {
                prop = param->disc_res.prop + i;
                if (prop->type == ESP_BT_GAP_DEV_PROP_EIR && prop->len > 0) {
                    uint8_t *eir = prop->val;
                    uint8_t *rmt_name = NULL;
                    uint8_t rmt_name_len = 0;
                    
                    rmt_name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_name_len);
                    
                    if (rmt_name != NULL && rmt_name_len > 0 && rmt_name_len < 64) {
                        char remote_name[rmt_name_len + 1];
                        memcpy(remote_name, rmt_name, rmt_name_len);
                        remote_name[rmt_name_len] = '\0';
                        
                        ESP_LOGI(TAG, "Dispositivo: %s [%s]", remote_name, bda_str);
                        
                        if (strstr(remote_name, TARGET_DEVICE_NAME) != NULL) {
                            ESP_LOGI(TAG, "Dispositivo alvo encontrado!");
                            memcpy(target_device_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
                            device_found = true;
                            
                            esp_err_t ret = esp_bt_gap_cancel_discovery();
                            if (ret != ESP_OK) {
                                ESP_LOGE(TAG, "Erro ao cancelar descoberta: %s", esp_err_to_name(ret));
                            }
                            
                            xTimerStop(discovery_timer, 0);
                        }
                    }
                }
            }
        }
        break;
        
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Descoberta parada");
            xTimerStop(discovery_timer, 0);
            
            if (sys_status.bt_connected) {
                ESP_LOGI(TAG, "Já conectado - ignorando discovery");
                return;
            }
            
            if (device_found) {
                ESP_LOGI(TAG, "Conectando ao dispositivo...");
                esp_err_t ret = esp_a2d_source_connect(target_device_addr);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Erro ao conectar A2DP: %s", esp_err_to_name(ret));
                    player_cmd_t cmd = CMD_RETRY_CONNECTION;
                    xQueueSend(control_queue, &cmd, 0);
                } else {
                    xTimerStart(connection_timer, 0);
                }
            } else {
                ESP_LOGW(TAG, "Dispositivo %s não encontrado", TARGET_DEVICE_NAME);
                if (sys_status.connection_retries < CONNECTION_RETRY_MAX) {
                    player_cmd_t cmd = CMD_RESTART_DISCOVERY;
                    xQueueSend(control_queue, &cmd, pdMS_TO_TICKS(100));
                }
            }
        }
        break;
        
    default:
        break;
    }
}

static void bt_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (!param) {
        ESP_LOGE(TAG, "Parâmetro A2DP callback é NULL");
        return;
    }
    
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP Conectado!");
            sys_status.bt_connected = true;
            sys_status.connection_retries = 0;
            sys_status.system_ready = true;
            xEventGroupSetBits(player_event_group, BT_CONNECTED_BIT);
            xTimerStop(connection_timer, 0);
            
            ESP_LOGI(TAG, "Iniciando stream de áudio...");
            esp_err_t start_ret = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            if (start_ret != ESP_OK) {
                ESP_LOGE(TAG, "Falha ao iniciar media stream: %s", esp_err_to_name(start_ret));
            }
            
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP Desconectado");
            first_command_received = false;
            sys_status.bt_connected = false;
            sys_status.audio_playing = false;
            sys_status.system_ready = false;
            xEventGroupClearBits(player_event_group, BT_CONNECTED_BIT);
            
            if (sys_status.connection_retries < CONNECTION_RETRY_MAX) {
                player_cmd_t cmd = CMD_RETRY_CONNECTION;
                xQueueSend(control_queue, &cmd, pdMS_TO_TICKS(1000));
            }
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            ESP_LOGI(TAG, "Conectando A2DP...");
        }
        break;
        
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            ESP_LOGI(TAG, "Áudio iniciado");
            sys_status.audio_playing = true;
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
            ESP_LOGI(TAG, "Áudio parado");
            sys_status.audio_playing = false;
            xEventGroupSetBits(player_event_group, TRACK_FINISHED_BIT);
        }
        break;
        
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG, "Config áudio recebida do sink");
        break;
        
    default:
        break;
    }
}

static void click_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "=== CLIQUES FINALIZADOS: %d ===", click_count);
    
    switch (click_count) {
        case 1:
            ESP_LOGI(TAG, "Ação: 1 clique - Toggle Pause");
            {
                player_cmd_t cmd = CMD_TOGGLE_PAUSE;
                xQueueSend(control_queue, &cmd, 0);
            }
            break;
            
        case 2:
            ESP_LOGI(TAG, "Ação: 2 cliques - Próxima música");
            {
                player_cmd_t cmd = CMD_PLAY_NEXT;
                xQueueSend(control_queue, &cmd, 0);
            }
            break;
            
        case 3:
            ESP_LOGI(TAG, "Ação: 3 cliques - Ação customizada");
            break;
            
        default:
            ESP_LOGI(TAG, "Ação: %d cliques - Não mapeado", click_count);
            break;
    }
    
    click_count = 0;
}

static void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    ESP_LOGI(TAG, ">>> AVRC TG EVENT: %d (param: %p) <<<", event, (void*)param);
    
    if (!param) {
        ESP_LOGE(TAG, "AVRC TG: param NULL no evento %d", event);
        return;
    }
    
    if (event == 0) {
        ESP_LOGI(TAG, "AVRC TG: %s", 
                param->conn_stat.connected ? "Conectado" : "Desconectado");
        return;
    }
    
    if (event == 1) {
        ESP_LOGI(TAG, "=== PASSTHROUGH RECEBIDO ===");
        
        uint8_t key = param->psth_cmd.key_code;
        uint8_t state = param->psth_cmd.key_state;
        
        ESP_LOGI(TAG, "Raw: key=0x%02X state=%d", key, state);
        
        const char* key_name = "UNKNOWN";
        switch(key) {
            case 0x44: key_name = "PLAY"; break;
            case 0x46: key_name = "PAUSE"; break;
            case 0x4B: key_name = "FORWARD"; break;
            case 0x4C: key_name = "BACKWARD"; break;
            case 0x45: key_name = "STOP"; break;
        }
        
        ESP_LOGI(TAG, "[%s] %s", key_name, state == 2 ? "RELEASED" : "UNKNOWN");
        
        if (!first_command_received) {
            ESP_LOGI(TAG, ">>> PRIMEIRO COMANDO - Fone sinalizou 'pronto' <<<");
            first_command_received = true;
            
            if (wav_count > 0) {
                ESP_LOGI(TAG, "Iniciando reproducao automatica...");
                player_cmd_t cmd = CMD_PLAY_NEXT;
                xQueueSend(control_queue, &cmd, 0);
            }
            return;
        }
        
        if (state == 1 || state == 2) {
            TickType_t current_time = xTaskGetTickCount();
            TickType_t time_diff = (current_time - last_click_time) * portTICK_PERIOD_MS;
            
            if (time_diff < CLICK_TIMEOUT_MS) {
                click_count++;
            } else {
                click_count = 1;
            }
            last_click_time = current_time;
            
            ESP_LOGI(TAG, ">>> Cliques: %d (intervalo: %lums) <<<", 
                    click_count, (unsigned long)time_diff);
            
            static TimerHandle_t click_timer = NULL;
            if (!click_timer) {
                click_timer = xTimerCreate("click_timer", 
                    pdMS_TO_TICKS(CLICK_TIMEOUT_MS + 50), 
                    pdFALSE, NULL, click_timeout_callback);
            }
            
            if (click_timer) {
                xTimerReset(click_timer, 0);
            }
            
            switch (key) {
                case 0x4B:
                    ESP_LOGI(TAG, "-> CMD_PLAY_NEXT enviado");
                    {
                        player_cmd_t cmd = CMD_PLAY_NEXT;
                        xQueueSend(control_queue, &cmd, 0);
                    }
                    break;
                    
                case 0x45:
                    ESP_LOGI(TAG, "-> CMD_STOP enviado");
                    {
                        player_cmd_t cmd = CMD_STOP;
                        xQueueSend(control_queue, &cmd, 0);
                    }
                    break;
            }
        }
        return;
    }
    
    if (event == 2) {
        ESP_LOGI(TAG, "AVRC TG: Volume command");
        return;
    }
    
    ESP_LOGI(TAG, "AVRC TG: Evento %d nao mapeado", event);
}

static void bt_avrc_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    ESP_LOGI(TAG, ">>> AVRC EVENT: %d <<<", event);
    
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (param && param->conn_stat.connected) {
            ESP_LOGI(TAG, "AVRC Conectado");
        } else {
            ESP_LOGI(TAG, "AVRC Desconectado");
        }
        break;
    
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        ESP_LOGI(TAG, "AVRC: Metadata recebido");
        break;
        
    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
        ESP_LOGI(TAG, "AVRC: Play status changed");
        break;
        
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        ESP_LOGI(TAG, "AVRC: Change notify - event_id: %d", param->change_ntf.event_id);
        break;
        
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        ESP_LOGI(TAG, "AVRC: Get capabilities response");
        break;

    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        if (param) {
            uint8_t key = param->psth_rsp.key_code;
            uint8_t state = param->psth_rsp.key_state;
            
            ESP_LOGI(TAG, "COMANDO RECEBIDO (CT): key=0x%02X state=%d", key, state);
            
            if (state == ESP_AVRC_PT_CMD_STATE_PRESSED) {
                TickType_t current_time = xTaskGetTickCount();
                TickType_t time_diff = (current_time - last_click_time) * portTICK_PERIOD_MS;
                
                if (time_diff < CLICK_TIMEOUT_MS) {
                    click_count++;
                } else {
                    click_count = 1;
                }
                last_click_time = current_time;
                
                ESP_LOGI(TAG, ">>> Cliques (CT): %d <<<", click_count);
                
                static TimerHandle_t click_timer = NULL;
                if (!click_timer) {
                    click_timer = xTimerCreate("click_timer", 
                        pdMS_TO_TICKS(CLICK_TIMEOUT_MS + 50), 
                        pdFALSE, NULL, click_timeout_callback);
                }
                xTimerReset(click_timer, 0);
            }
        }
        break;
        
    default:
        ESP_LOGI(TAG, "AVRC: Evento não tratado: %d", event);
        break;
    }
}

// ============================================================================
// SD CARD
// ============================================================================

char* get_current_track_path(void)
{
    static char filepath[MAX_PATH_LEN];
    return get_wav_by_index(current_track, filepath, sizeof(filepath));
}

void random_track(void)
{
    if (wav_count <= 1) return;
    
    int new_track;
    do {
        new_track = esp_random() % wav_count;
    } while (new_track == current_track && wav_count > 1);
    
    current_track = new_track;
    ESP_LOGI(TAG, "Track aleatória: %d/%d", current_track + 1, wav_count);
}

// ============================================================================
// BUFFERS
// ============================================================================

esp_err_t init_audio_buffers(void)
{
    ESP_LOGI(TAG, "Inicializando buffers de áudio...");
    
    file_read_buffer = heap_caps_malloc(AUDIO_READ_BUFFER_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!file_read_buffer) {
        ESP_LOGE(TAG, "Falha buffer leitura: %d bytes", AUDIO_READ_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }
    
    stream_buffer = create_ring_buffer(STREAM_BUFFER_SIZE);
    if (!stream_buffer) {
        ESP_LOGE(TAG, "Falha stream buffer: %d bytes", STREAM_BUFFER_SIZE);
        free(file_read_buffer);
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Buffers OK: Leitura=%dKB, Stream=%dKB", 
            AUDIO_READ_BUFFER_SIZE/1024, STREAM_BUFFER_SIZE/1024);
    
    return ESP_OK;
}

void cleanup_audio_buffers(void)
{
    if (file_read_buffer) {
        free(file_read_buffer);
        file_read_buffer = NULL;
    }
    if (stream_buffer) {
        destroy_ring_buffer(stream_buffer);
        stream_buffer = NULL;
    }
}

// ============================================================================
// BLUETOOTH
// ============================================================================

esp_err_t bluetooth_init(void)
{
    ESP_LOGI(TAG, "Inicializando Bluetooth...");
    
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Heap livre antes do BT: %zu bytes", free_heap);
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Apagando NVS e reinicializando...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    esp_bt_controller_status_t bt_status = esp_bt_controller_get_status();
    if (bt_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_LOGW(TAG, "Desabilitando Bluetooth anterior...");
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else if (bt_status == ESP_BT_CONTROLLER_STATUS_INITED) {
        ESP_LOGW(TAG, "Desinicializando controller BT anterior...");
        esp_bt_controller_deinit();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    ESP_LOGI(TAG, "Configurando controller Bluetooth...");
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    
    bt_cfg.mode = ESP_BT_MODE_CLASSIC_BT;
    bt_cfg.bt_max_acl_conn = 2;
    bt_cfg.bt_sco_datapath = ESP_SCO_DATA_PATH_HCI;
    
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar controller BT: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Controller BT inicializado");
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao habilitar controller BT: %s", esp_err_to_name(ret));
        esp_bt_controller_deinit();
        return ret;
    }
    ESP_LOGI(TAG, "Controller BT habilitado");
    
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar Bluedroid: %s", esp_err_to_name(ret));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }
    ESP_LOGI(TAG, "Bluedroid inicializado");
    
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao habilitar Bluedroid: %s", esp_err_to_name(ret));
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }
    ESP_LOGI(TAG, "Bluedroid habilitado");

    // Configurar potência máxima do Bluetooth
    esp_err_t power_ret = esp_bredr_tx_power_set(ESP_PWR_LVL_P9, ESP_PWR_LVL_P9);
    if (power_ret == ESP_OK) {
        ESP_LOGI(TAG, "Potência BT configurada para MÁXIMA (+9dBm)");
    } else {
        ESP_LOGW(TAG, "Falha ao configurar potência BT: %s", esp_err_to_name(power_ret));
    }
    // ===================================
    
    vTaskDelay(pdMS_TO_TICKS(100));

    ret = esp_bt_gap_register_callback(bt_gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar GAP callback: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "GAP callback registrado");
    
    ret = esp_avrc_ct_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar AVRC: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "AVRC inicializado");
    
    ret = esp_avrc_ct_register_callback(bt_avrc_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar AVRC callback: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "AVRC callback registrado");

    ret = esp_avrc_tg_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar AVRC TG: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "AVRC Target inicializado");

    ret = esp_avrc_tg_register_callback(bt_avrc_tg_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar AVRC TG callback: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "AVRC TG callback registrado");
    
    ret = esp_a2d_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar A2DP source: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "A2DP source inicializado");
    
    ret = esp_a2d_register_callback(bt_a2dp_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar A2DP callback: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "A2DP callback registrado");
    
    ret = esp_a2d_source_register_data_callback(bt_a2dp_source_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar data callback: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "A2DP data callback registrado");
    
    sys_status.bt_initialized = true;
    ESP_LOGI(TAG, "Bluetooth inicializado com sucesso");
    return ESP_OK;
}

esp_err_t bluetooth_search_and_connect(void)
{
    if (!sys_status.bt_initialized) {
        ESP_LOGE(TAG, "Bluetooth não inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (sys_status.bt_connected) {
        ESP_LOGI(TAG, "Já conectado - cancelando busca");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Procurando por %s... (tentativa %d/%d)", 
             TARGET_DEVICE_NAME, sys_status.connection_retries + 1, CONNECTION_RETRY_MAX);
    
    device_found = false;
    sys_status.connection_retries++;
    
    esp_err_t ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao configurar scan mode: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao iniciar descoberta: %s", esp_err_to_name(ret));
        return ret;
    }
    
    xTimerStart(discovery_timer, 0);
    
    return ESP_OK;
}

// ============================================================================
// SD CARD INIT
// ============================================================================

esp_err_t sdcard_init(void)
{
    ESP_LOGI(TAG, "Inicializando SD Card...");
    
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 18000;
    
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .intr_flags = 0
    };
    
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Erro SPI: %s", esp_err_to_name(ret));
        return ret;
    }
    
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false
    };
    
    sdmmc_card_t *card;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD Card OK!");
        ESP_LOGI(TAG, "  Nome: %s", card->cid.name);
        ESP_LOGI(TAG, "  Freq: %d kHz", host.max_freq_khz);
        ESP_LOGI(TAG, "  Tipo: %s", (card->ocr & (1 << 30)) ? "SDHC/SDXC" : "SDSC");
        ESP_LOGI(TAG, "  Tam: %llu MB", 
                    ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
        
        sys_status.sd_mounted = true;
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Todas tentativas falharam!");
    spi_bus_free(host.slot);
    return ret;
}

// ============================================================================
// STATUS E TIMERS
// ============================================================================

static void log_system_status(void)
{
    ESP_LOGI(TAG, "=== STATUS ===");
    ESP_LOGI(TAG, "SD:%s BT:%s Stream:%s WAVs:%d", 
            sys_status.sd_mounted ? "OK" : "ERR",
            sys_status.bt_connected ? "OK" : "ERR", 
            sys_status.streaming_active ? "ON" : "OFF",
            wav_count);
    
    if (stream_buffer) {
        ESP_LOGI(TAG, "Buf: %zuKB / %dKB (%.0f%%)",
                rb_available(stream_buffer)/1024, 
                STREAM_BUFFER_SIZE/1024,
                (rb_available(stream_buffer) * 100.0) / STREAM_BUFFER_SIZE);
    }
    
    ESP_LOGI(TAG, "Volume: %d%% (%.2f)", current_volume, volume_scale);
    ESP_LOGI(TAG, "Perf: %luKB E:%lu Heap:%lu", 
            total_bytes_streamed/1024, underrun_count, 
            (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "==============");
}

static void connection_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGW(TAG, "Timeout conexão BT");
    player_cmd_t cmd = CMD_RETRY_CONNECTION;
    xQueueSend(control_queue, &cmd, 0);
}

static void discovery_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGW(TAG, "Timeout descoberta");
    esp_bt_gap_cancel_discovery();
    
    if (!device_found && sys_status.connection_retries < CONNECTION_RETRY_MAX) {
        player_cmd_t cmd = CMD_RESTART_DISCOVERY;
        xQueueSend(control_queue, &cmd, 0);
    }
}

static void buffer_monitor_callback(TimerHandle_t xTimer)
{
    if (!sys_status.streaming_active) return;
    
    if (stream_buffer) {
        size_t stream_available = rb_available(stream_buffer);
        
        if (stream_available < STREAM_REFILL_THRESHOLD) {
            ESP_LOGW(TAG, "Buffer baixo: %zu KB", stream_available / 1024);
        }
    }
}

// ============================================================================
// TASK CONTROLE
// ============================================================================

static const char* get_filename_from_path(const char *path) {
    if (!path) return "unknown";
    const char *filename = strrchr(path, '/');
    return filename ? filename + 1 : path;
}

void player_control_task(void *pvParameter)
{
    player_cmd_t cmd;
    TickType_t last_status_log = 0;
    
    ESP_LOGI(TAG, "Task controle iniciada");
    
    while (1) {
        if (xTaskGetTickCount() - last_status_log > pdMS_TO_TICKS(30000)) {
            log_system_status();
            last_status_log = xTaskGetTickCount();
        }
        
        if (xQueueReceive(control_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (cmd) {
                case CMD_PLAY_NEXT:
                    ESP_LOGI(TAG, "CMD: PLAY_NEXT");
                    ESP_LOGI(TAG, "Heap ANTES de limpar: %lu (largest: %lu)", 
                            (unsigned long)esp_get_free_heap_size(),
                            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                    
                    // ========== MATAR TASK ANTERIOR ==========
                    sys_status.streaming_active = false;
                    
                    if (file_reader_task_handle != NULL) {
                        ESP_LOGI(TAG, "Finalizando task de leitura anterior...");
                        vTaskDelay(pdMS_TO_TICKS(100));  // Aguarda finalização
                        
                        // Verifica se ainda existe
                        if (eTaskGetState(file_reader_task_handle) != eDeleted) {
                            ESP_LOGW(TAG, "Task não finalizou sozinha, deletando forçado...");
                            vTaskDelete(file_reader_task_handle);
                        }
                        file_reader_task_handle = NULL;
                        ESP_LOGI(TAG, "Task anterior finalizada");
                    }
                    // =========================================
                    
                    vTaskDelay(pdMS_TO_TICKS(300));  // Aumentado para 300ms
                    
                    underrun_count = 0;
                    callback_count = 0;
                    stream_stabilized = false;
                    
                    if (current_file) {
                        fclose(current_file);
                        current_file = NULL;
                    }
                    
                    if (stream_buffer) rb_clear(stream_buffer);
                    
                    total_bytes_streamed = 0;
                    callback_count = 0;
                    underrun_count = 0;
                    
                    ESP_LOGI(TAG, "Heap DEPOIS de limpar: %lu (largest: %lu)", 
                            (unsigned long)esp_get_free_heap_size(),
                            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                    
                    if (!sys_status.system_ready || wav_count == 0) {
                        ESP_LOGW(TAG, "Sistema não pronto");
                        break;
                    }
                    
                    random_track();
                    char *filepath = get_current_track_path();
                    
                    if (!filepath) {
                        ESP_LOGE(TAG, "Path inválido");
                        sys_status.file_errors++;
                        break;
                    }
                    
                    current_file = fopen(filepath, "rb");
                    if (!current_file) {
                        ESP_LOGE(TAG, "Falha abrir: %s", strerror(errno));
                        sys_status.file_errors++;
                        esp_restart();
                        break;
                    }
                    
                    ESP_LOGI(TAG, "Reproduzindo: %s", get_filename_from_path(filepath));
                    
                    esp_err_t analyze_ret = analyze_wav_file(filepath, &current_wav_info);
                    
                    if (analyze_ret == ESP_OK && current_wav_info.is_valid_wav) {
                        ESP_LOGI(TAG, "WAV: %luHz %dch %dbit %lus",
                                current_wav_info.sample_rate, current_wav_info.channels,
                                current_wav_info.bits_per_sample, current_wav_info.duration_seconds);
                        fseek(current_file, current_wav_info.data_start_offset, SEEK_SET);
                    } else {
                        ESP_LOGW(TAG, "Análise WAV falhou - raw");
                        fseek(current_file, 0, SEEK_SET);
                        memset(&current_wav_info, 0, sizeof(current_wav_info));
                    }
                    
                    sys_status.streaming_active = true;
                    sys_status.file_errors = 0;
                    
                    // ========== CRIAR NOVA TASK COM HANDLE ==========
                    if (xTaskCreatePinnedToCore(file_reader_task, "reader", 
                                            4096, NULL, 10, &file_reader_task_handle, 1) != pdPASS) {
                        ESP_LOGE(TAG, "Falha criar task reader");
                        fclose(current_file);
                        current_file = NULL;
                        sys_status.streaming_active = false;
                        file_reader_task_handle = NULL;
                        esp_restart();
                        break;
                    }
                    // ================================================
                    
                    ESP_LOGI(TAG, "Pré-enchendo buffers...");
                    
                    size_t target_prebuffer = (STREAM_BUFFER_SIZE * 7) / 10;
                    int wait_attempts = 0;
                    
                    while (wait_attempts < 100) {
                        size_t stream_avail = rb_available(stream_buffer);
                        
                        if (stream_avail >= target_prebuffer) {
                            ESP_LOGI(TAG, "Buffer pronto: %zu KB (%.0f%%)", 
                                    stream_avail / 1024,
                                    (stream_avail * 100.0) / STREAM_BUFFER_SIZE);
                            break;
                        }
                        
                        vTaskDelay(pdMS_TO_TICKS(100));
                        wait_attempts++;
                    }
                    
                    if (wait_attempts >= 100) {
                        ESP_LOGW(TAG, "Timeout pré-buffer, iniciando com %zu KB",
                                rb_available(stream_buffer) / 1024);
                    }
                    
                    ESP_LOGI(TAG, "Garantindo que áudio está ativo...");
                    esp_err_t resume_ret = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                    if (resume_ret != ESP_OK) {
                        ESP_LOGE(TAG, "Falha ao iniciar áudio: %s", esp_err_to_name(resume_ret));
                    } else {
                        ESP_LOGI(TAG, "Áudio iniciado com sucesso");
                    }
                    
                    vTaskDelay(pdMS_TO_TICKS(200));
                    
                    ESP_LOGI(TAG, "Streaming iniciado");
                    break;
                    
                case CMD_RETRY_CONNECTION:
                    ESP_LOGI(TAG, "CMD: RETRY_CONNECTION");
                    
                    if (sys_status.connection_retries >= CONNECTION_RETRY_MAX) {
                        ESP_LOGE(TAG, "Max tentativas excedido");
                        xEventGroupSetBits(player_event_group, ERROR_BIT);
                        break;
                    }
                    
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    bluetooth_search_and_connect();
                    break;
                    
                case CMD_RESTART_DISCOVERY:
                    ESP_LOGI(TAG, "CMD: RESTART_DISCOVERY");
                    
                    if (sys_status.bt_connected) {
                        ESP_LOGI(TAG, "Já conectado - cancelando restart discovery");
                        break;
                    }
                    
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    bluetooth_search_and_connect();
                    break;
                    
                case CMD_STOP:
                    ESP_LOGI(TAG, "CMD: STOP");
                    sys_status.streaming_active = false;
                    if (current_file) {
                        fclose(current_file);
                        current_file = NULL;
                    }
                    if (stream_buffer) rb_clear(stream_buffer);
                    break;
                
                case CMD_TOGGLE_PAUSE:
                    ESP_LOGI(TAG, "CMD: TOGGLE_PAUSE");
                    
                    if (sys_status.audio_playing) {
                        ESP_LOGI(TAG, "Pausando stream...");
                        sys_status.streaming_active = false;
                        
                        esp_err_t pause_ret = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
                        if (pause_ret != ESP_OK) {
                            ESP_LOGE(TAG, "Falha pausar: %s", esp_err_to_name(pause_ret));
                        }
                    } else {
                        ESP_LOGI(TAG, "Retomando stream...");
                        sys_status.streaming_active = true;
                        
                        esp_err_t resume_ret = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                        if (resume_ret != ESP_OK) {
                            ESP_LOGE(TAG, "Falha retomar: %s", esp_err_to_name(resume_ret));
                        }
                    }
                    break;

                default:
                    ESP_LOGW(TAG, "Comando desconhecido: %d", cmd);
                    break;
            }
        }
        
        if (sys_status.file_errors > 10) {
            ESP_LOGE(TAG, "Muitos erros, reiniciando...");
            esp_restart();
        }
    }
}

// ============================================================================
// TASK PRINCIPAL
// ============================================================================

void main_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Task principal iniciada");
    
    ESP_LOGI(TAG, "Aguardando conexão Bluetooth...");
    
    EventBits_t bits = xEventGroupWaitBits(player_event_group, 
                                          BT_CONNECTED_BIT | ERROR_BIT, 
                                          false, false, 
                                          pdMS_TO_TICKS(60000));
    
    if (bits & ERROR_BIT) {
        ESP_LOGE(TAG, "Erro crítico detectado, reiniciando...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    
    if (!(bits & BT_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Timeout de conexão Bluetooth");
        log_system_status();
        
        if (sys_status.connection_retries < CONNECTION_RETRY_MAX) {
            player_cmd_t cmd = CMD_RETRY_CONNECTION;
            xQueueSend(control_queue, &cmd, 0);
            
            bits = xEventGroupWaitBits(player_event_group, BT_CONNECTED_BIT, 
                                     false, true, pdMS_TO_TICKS(30000));
            
            if (!(bits & BT_CONNECTED_BIT)) {
                ESP_LOGE(TAG, "Falha final de conexão, reiniciando...");
                esp_restart();
                return;
            }
        } else {
            ESP_LOGE(TAG, "Falha definitiva de conexão");
            return;
        }
    }
    
    ESP_LOGI(TAG, "Bluetooth conectado! Sistema pronto.");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_avrc_ct_send_set_player_value_cmd(0, ESP_AVRC_PS_REPEAT_MODE, 0);

    log_system_status();
    
    xEventGroupWaitBits(player_event_group, STREAM_READY_BIT, 
                       false, false, pdMS_TO_TICKS(5000));
    
    TickType_t last_alive_log = xTaskGetTickCount();
    
    while (1) {
        if (xTaskGetTickCount() - last_alive_log > pdMS_TO_TICKS(120000)) {
            ESP_LOGI(TAG, "Sistema ativo - Track: [%d/%d]", 
                    current_track + 1, wav_count);
            ESP_LOGI(TAG, "Stream: %s, Heap: %lu", 
                    sys_status.streaming_active ? "ON" : "OFF",
                    (unsigned long)esp_get_free_heap_size());
            last_alive_log = xTaskGetTickCount();
        }
        
        if (sys_status.bt_connected && !sys_status.audio_playing && current_file) {
            ESP_LOGW(TAG, "Inconsistência: arquivo aberto mas áudio parado");
        }
        
        if (!sys_status.bt_connected && sys_status.connection_retries < CONNECTION_RETRY_MAX) {
            ESP_LOGW(TAG, "Conexão perdida, tentando reconectar...");
            player_cmd_t cmd = CMD_RETRY_CONNECTION;
            xQueueSend(control_queue, &cmd, 0);
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============================================================================
// APP MAIN
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "===== ESP32 WAV Player v4.0 com Controle de Volume =====");
    ESP_LOGI(TAG, "Heap inicial: %lu bytes", (unsigned long)esp_get_free_heap_size());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("BT_AVRC", ESP_LOG_VERBOSE);
    esp_log_level_set("BT_APPL", ESP_LOG_VERBOSE);
    esp_log_level_set("BT_AV", ESP_LOG_VERBOSE);
    esp_log_level_set("BT_HCI", ESP_LOG_VERBOSE);
    
    memset(&sys_status, 0, sizeof(sys_status));
    
    player_event_group = xEventGroupCreate();
    control_queue = xQueueCreate(15, sizeof(player_cmd_t));
    connection_timer = xTimerCreate("conn", pdMS_TO_TICKS(15000), 
                                   pdFALSE, NULL, connection_timeout_callback);
    discovery_timer = xTimerCreate("disc", pdMS_TO_TICKS(DISCOVERY_TIMEOUT_SEC * 1000), 
                                  pdFALSE, NULL, discovery_timeout_callback);
    buffer_monitor_timer = xTimerCreate("buf_mon", pdMS_TO_TICKS(5000),
                                       pdTRUE, NULL, buffer_monitor_callback);
    
    if (!player_event_group || !control_queue || !connection_timer || 
        !discovery_timer || !buffer_monitor_timer) {
        ESP_LOGE(TAG, "Falha criar estruturas");
        esp_restart();
        return;
    }
    
    ESP_LOGI(TAG, "Estruturas criadas");
    
    esp_err_t ret = sdcard_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha SD Card: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    
    ret = count_wav_files();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha escanear WAVs: %s", esp_err_to_name(ret));
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Nenhum WAV encontrado!");
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                ESP_LOGE(TAG, "Aguardando arquivos WAV...");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    
    ret = init_audio_buffers();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha buffers: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    
    ESP_LOGI(TAG, "Heap após buffers: %lu", (unsigned long)esp_get_free_heap_size());
    
    ret = bluetooth_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha Bluetooth: %s", esp_err_to_name(ret));
        cleanup_audio_buffers();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    
    ESP_LOGI(TAG, "Heap após BT: %lu", (unsigned long)esp_get_free_heap_size());
    
    if (xTaskCreatePinnedToCore(player_control_task, "ctrl", 
                            4096, NULL, 7, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Falha task controle");
        cleanup_audio_buffers();
        esp_restart();
        return;
    }

    if (xTaskCreatePinnedToCore(main_task, "main", 
                            3072, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Falha task main");
        cleanup_audio_buffers();
        esp_restart();
        return;
    }

    ESP_LOGI(TAG, "Tasks criadas");
    
    init_volume_buttons();
    
    if (xTaskCreatePinnedToCore(volume_control_task, "volume", 
                            2048, NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Falha task volume");
        cleanup_audio_buffers();
        esp_restart();
        return;
    }
    
    log_system_status();
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    xTimerStart(buffer_monitor_timer, 0);
    
    ESP_LOGI(TAG, "Iniciando busca BT: %s", TARGET_DEVICE_NAME);
    ret = bluetooth_search_and_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha busca: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Sistema iniciado!");
    
    TickType_t last_check = xTaskGetTickCount();
    uint32_t restart_counter = 0;
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        
        if (xTaskGetTickCount() - last_check > pdMS_TO_TICKS(120000)) {
            size_t free_heap = esp_get_free_heap_size();
            
            ESP_LOGI(TAG, "Global: Heap=%zu, Stream=%s", 
                    free_heap, sys_status.streaming_active ? "ON" : "OFF");
            
            if (free_heap < 30000) {
                restart_counter++;
                ESP_LOGW(TAG, "Memória baixa (%zu) - cnt: %lu", 
                        free_heap, restart_counter);
                
                if (restart_counter >= 3) {
                    ESP_LOGE(TAG, "Memória crítica, restart");
                    cleanup_audio_buffers();
                    esp_restart();
                }
            } else {
                restart_counter = 0;
            }
            
            last_check = xTaskGetTickCount();
        }
    }
}
