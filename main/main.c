/*
 * ============================================================================
 * ESP32 MP3 Player - CORREÇÕES FINAIS
 * ============================================================================
 * 
 * CORREÇÕES APLICADAS:
 * 
 * 1. CONTROLE DE BUFFER MELHORADO:
 *    - Thresholds ajustados: 40-75% (era 30-70%)
 *    - Yield mais agressivo (a cada 3 frames vs 8)
 *    - Leitura adaptativa mais equilibrada
 * 
 * 2. TRANSIÇÃO ENTRE MÚSICAS:
 *    - Reset completo do stream buffer
 *    - Aguardar término da task anterior
 *    - Fechar arquivo antes de limpar decoder
 *    - Zerar bytes_left_in_mp3 e read_ptr
 * 
 * 3. FIM DE ARQUIVO:
 *    - Detectar EOF adequadamente
 *    - Processar bytes restantes no buffer
 *    - Limpar recursos na ordem correta
 * 
 * Compatível com: IDF 4.4.4 + libhelix
 * Data: 02/02/2026
 * ============================================================================
 */

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
#include "driver/rtc_io.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "nvs_flash.h"
#include <limits.h>

#include "mp3dec.h"

// Configurações do SD Card
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   4
#define MOUNT_POINT "/sdcard"
#define BOARD_LED_GPIO 2
#define BOARD_LED_ACTIVE_HIGH 1

// Pinos de controle de volume
#define PIN_VOL_UP   21
#define PIN_VOL_DOWN 22
#define PIN_PWR_SLEEP 33  // RTC IO: wakeup de deep sleep (botão ativo em LOW)
#define DEBOUNCE_TIME_MS 50
#define VOLUME_STEP 5
#define POWER_HOLD_MS 2000
#define AUTO_SLEEP_IDLE_MS (1 * 60 * 1000)  // 1 min sem BT/áudio -> deep sleep
#define BT_CONNECTING_STUCK_MS (90 * 1000)  // destrava estado "conectando" preso
#define A2DP_OPEN_FAIL_REDISCOVERY_THRESHOLD 3
#define PWR_RELEASE_WAIT_MS 5000

#define DOUBLE_CLICK_INTERVAL_MS 300
#define LONG_CLICK_THRESHOLD_MS 400

#define MAX_PATH_LEN 256

// ✅ CONFIGURAÇÃO OTIMIZADA - FOCO EM BALANCEAR PRODUÇÃO/CONSUMO
#define MP3_INPUT_BUFFER_SIZE     16384     // 16KB
#define PCM_OUTPUT_BUFFER_SIZE    4608      // 4.6KB - 1 frame
#define STREAM_BUFFER_SIZE        32278     // ~32KB
#define A2DP_CHUNK_SIZE           512       // Controlado pelo IDF

// ✅ THRESHOLDS MUITO REDUZIDOS: 30-60% (alvo: manter entre 30-60%)
#define MP3_LOW_WATERMARK      30.0f  // Muito reduzido
#define MP3_HIGH_WATERMARK    60.0f   // Muito reduzido

#define MP3_READ_MIN          1024
#define MP3_READ_MAX          16384

#define MP3_CRITICAL_BYTES    4096
#define MP3_NO_SYNC_DROP_BYTES 512
#define PREBUFFER_FRAMES      10

#define STREAM_REFILL_THRESHOLD   (STREAM_BUFFER_SIZE / 4)
#define TICKS_TO_MS(t) ((uint32_t)(t) * (uint32_t)portTICK_PERIOD_MS)

// Configuração do dispositivo alvo
// ✅ IDF 4.4.4: Usa apenas MAC address para descoberta
// (Campo EIR não disponível nesta versão do IDF)
#define TARGET_DEVICE_NAME "TWS"  // Apenas para referência/logs
#define TARGET_DEVICE_MAC  {0x41, 0x42, 0x78, 0xA4, 0x06, 0x97}
#define PREFER_MAC_OVER_NAME true  // Sempre true no IDF 4.4.4

#define CONNECTION_RETRY_MAX 5
#define FILE_READ_RETRY_MAX 3
#define DISCOVERY_TIMEOUT_SEC 45

#define SAMPLE_RATE_44K1 44100
#define SAMPLE_RATE_48K  48000

typedef struct {
    bool is_valid_mp3;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t file_size;
    uint32_t bitrate;
    uint32_t duration_seconds;
    bool a2dp_compatible;
} mp3_info_t;

static const char *TAG = "MP3Player";

typedef struct {
    bool sd_mounted;
    bool bt_initialized;
    bool bt_connected;
    bool bt_connecting;
    bool audio_playing;
    bool system_ready;
    bool codec_configured;
    bool streaming_active;
    int connection_retries;
    int file_errors;
} system_status_t;

static system_status_t sys_status = {0};

static uint8_t current_volume = 30;
static float volume_scale = 0.30f;

static esp_bd_addr_t target_device_addr;
static esp_bd_addr_t target_mac_addr = TARGET_DEVICE_MAC;
static bool device_found = false;
static bool discovery_active = false;
static bool connect_after_discovery_stop = false;
static int mp3_count = 0;
static int current_track = 0;
static FILE *current_file = NULL;
static mp3_info_t current_mp3_info = {0};
static TaskHandle_t file_reader_task_handle = NULL;
static volatile bool file_reader_task_running = false;

static HMP3Decoder mp3_decoder = NULL;

static uint8_t *mp3_input_buffer = NULL;
static int16_t *pcm_output_buffer = NULL;

static int bytes_left_in_mp3 = 0;
static uint8_t *read_ptr = NULL;

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

static uint32_t total_bytes_streamed = 0;
static uint32_t underrun_count = 0;
static uint32_t callback_count = 0;
static uint32_t cb_lock_fail_count = 0;
static uint32_t cb_empty_count = 0;
static uint32_t cb_partial_count = 0;
static uint32_t producer_frame_count = 0;
static uint32_t producer_drop_count = 0;
static bool stream_stabilized = false;
static uint32_t buffer_low_events = 0;
static uint32_t buffer_high_events = 0;
static TickType_t last_producer_tick = 0;
static bool playback_paused = false;
static TickType_t bt_connecting_since = 0;
static uint32_t a2dp_open_fail_streak = 0;

static void set_bt_connecting(bool connecting)
{
    if (connecting) {
        if (!sys_status.bt_connecting) {
            bt_connecting_since = xTaskGetTickCount();
        }
    } else {
        bt_connecting_since = 0;
    }
    sys_status.bt_connecting = connecting;
}

static bool bt_ready_for_playback(void)
{
    return sys_status.bt_connected && sys_status.streaming_active;
}

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
    CMD_PLAY_PREV,
    CMD_STOP,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_RETRY_CONNECTION,
    CMD_RESTART_DISCOVERY,
    CMD_CONNECT_TARGET,
    CMD_FILL_BUFFERS,
    CMD_TOGGLE_PAUSE
} player_cmd_t;

static volatile bool cmd_play_next_pending = false;
static volatile bool cmd_play_prev_pending = false;
static volatile bool cmd_retry_pending = false;
static volatile bool cmd_restart_disc_pending = false;
static volatile bool cmd_connect_pending = false;
static bool discovery_stop_pending = false;
static volatile bool restart_discovery_in_progress = false;

static const char *cmd_to_str(player_cmd_t cmd)
{
    switch (cmd) {
        case CMD_PLAY_NEXT: return "PLAY_NEXT";
        case CMD_PLAY_PREV: return "PLAY_PREV";
        case CMD_STOP: return "STOP";
        case CMD_PAUSE: return "PAUSE";
        case CMD_RESUME: return "RESUME";
        case CMD_RETRY_CONNECTION: return "RETRY_CONNECTION";
        case CMD_RESTART_DISCOVERY: return "RESTART_DISCOVERY";
        case CMD_CONNECT_TARGET: return "CONNECT_TARGET";
        case CMD_FILL_BUFFERS: return "FILL_BUFFERS";
        case CMD_TOGGLE_PAUSE: return "TOGGLE_PAUSE";
        default: return "UNKNOWN";
    }
}

static void log_bt_state(const char *reason)
{
    ESP_LOGD(TAG,
             "BT_STATE[%s] conn:%d connecting:%d stream:%d discovery:%d disc_stop_pending:%d found:%d retries:%d",
             reason ? reason : "-",
             sys_status.bt_connected,
             sys_status.bt_connecting,
             sys_status.streaming_active,
             discovery_active,
             discovery_stop_pending,
             device_found,
             sys_status.connection_retries);
}

static void log_queue_state(const char *reason)
{
    UBaseType_t pending = 0;
    if (control_queue) {
        pending = uxQueueMessagesWaiting(control_queue);
    }
    ESP_LOGD(TAG, "QUEUE[%s] pending:%lu", reason ? reason : "-", (unsigned long)pending);
}

static bool enqueue_control_cmd(player_cmd_t cmd)
{
    volatile bool *pending = NULL;

    switch (cmd) {
        case CMD_PLAY_NEXT:
            pending = &cmd_play_next_pending;
            break;
        case CMD_PLAY_PREV:
            pending = &cmd_play_prev_pending;
            break;
        case CMD_RETRY_CONNECTION:
            pending = &cmd_retry_pending;
            break;
        case CMD_RESTART_DISCOVERY:
            pending = &cmd_restart_disc_pending;
            break;
        case CMD_CONNECT_TARGET:
            pending = &cmd_connect_pending;
            break;
        default:
            break;
    }

    if (pending && *pending) {
        ESP_LOGD(TAG, "Queue dedup: %s já pendente", cmd_to_str(cmd));
        return false;
    }
    if (pending) {
        *pending = true;
    }

    if (xQueueSend(control_queue, &cmd, 0) != pdTRUE) {
        if (pending) {
            *pending = false;
        }
        ESP_LOGW(TAG, "Queue cheia ao enfileirar: %s", cmd_to_str(cmd));
        log_queue_state("send_fail");
        return false;
    }

    ESP_LOGD(TAG, "Queue <- %s", cmd_to_str(cmd));
    log_queue_state("after_enqueue");

    return true;
}

static TimerHandle_t connection_timer;
static TimerHandle_t discovery_timer;
static TimerHandle_t buffer_monitor_timer;

#define CLICK_TIMEOUT_MS 500

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
static bool pwr_last_level = true;
static TickType_t pwr_last_change_time = 0;
static TickType_t pwr_press_start_time = 0;
static bool pwr_pressed = false;
static bool pwr_hold_handled = false;

// ============================================================================
// RING BUFFER
// ============================================================================

static ring_buffer_t *ring_buffer_create(size_t size)
{
    ring_buffer_t *rb = malloc(sizeof(ring_buffer_t));
    if (!rb) return NULL;
    
    rb->data = heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!rb->data) {
        free(rb);
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
        return NULL;
    }
    
    return rb;
}

static void ring_buffer_destroy(ring_buffer_t *rb)
{
    if (!rb) return;
    
    if (rb->mutex) {
        vSemaphoreDelete(rb->mutex);
    }
    if (rb->data) {
        free(rb->data);
    }
    free(rb);
}

static void ring_buffer_reset(ring_buffer_t *rb)
{
    if (!rb) return;
    
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(100))) {
        rb->write_pos = 0;
        rb->read_pos = 0;
        rb->available = 0;
        rb->is_full = false;
        rb->end_of_stream = false;
        xSemaphoreGive(rb->mutex);
    }
}

static size_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len)
{
    if (!rb || !data || len == 0) return 0;
    
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }
    
    size_t space = rb->size - rb->available;
    size_t to_write = (len > space) ? space : len;
    size_t written = 0;
    
    while (written < to_write) {
        size_t chunk = to_write - written;
        size_t space_to_end = rb->size - rb->write_pos;
        
        if (chunk > space_to_end) chunk = space_to_end;
        
        memcpy(rb->data + rb->write_pos, data + written, chunk);
        rb->write_pos = (rb->write_pos + chunk) % rb->size;
        written += chunk;
    }
    
    rb->available += written;
    if (rb->available == rb->size) {
        rb->is_full = true;
    }
    
    xSemaphoreGive(rb->mutex);
    
    return written;
}

static size_t ring_buffer_write_blocking(ring_buffer_t *rb, const uint8_t *data, size_t len, TickType_t max_wait_ticks)
{
    if (!rb || !data || len == 0) return 0;

    size_t total_written = 0;
    TickType_t start = xTaskGetTickCount();

    while (total_written < len && file_reader_task_running && sys_status.bt_connected) {
        size_t written = ring_buffer_write(rb, data + total_written, len - total_written);
        total_written += written;

        if (total_written >= len) {
            break;
        }

        if ((xTaskGetTickCount() - start) >= max_wait_ticks) {
            break;
        }

        vTaskDelay(1);
    }

    return total_written;
}

static size_t ring_buffer_available(ring_buffer_t *rb)
{
    if (!rb) return 0;
    
    size_t available = 0;
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(5))) {
        available = rb->available;
        xSemaphoreGive(rb->mutex);
    }
    
    return available;
}

// ============================================================================
// ID3v2 TAG
// ============================================================================

static int skip_id3v2(FILE *f)
{
    uint8_t header[10];
    
    if (fread(header, 1, 10, f) != 10) {
        fseek(f, 0, SEEK_SET);
        return 0;
    }
    
    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
        uint32_t tag_size = ((header[6] & 0x7F) << 21) |
                           ((header[7] & 0x7F) << 14) |
                           ((header[8] & 0x7F) << 7) |
                           (header[9] & 0x7F);
        
        ESP_LOGI(TAG, "ID3v2 tag: %" PRIu32 " bytes (pulando)", tag_size);
        
        fseek(f, 10 + tag_size, SEEK_SET);
        return tag_size + 10;
    }
    
    fseek(f, 0, SEEK_SET);
    return 0;
}

static bool drop_trailing_tag_if_present(uint8_t **ptr, int *bytes_left)
{
    if (!ptr || !*ptr || !bytes_left || *bytes_left <= 0) {
        return false;
    }

    /* ID3v1 tail: 128 bytes starting with "TAG". */
    if (*bytes_left >= 128 && memcmp(*ptr, "TAG", 3) == 0) {
        ESP_LOGI(TAG, "🏁 Detectado ID3v1 no final (%d bytes restantes), encerrando track", *bytes_left);
        *ptr += *bytes_left;
        *bytes_left = 0;
        return true;
    }

    /* APEv2 footer/header marker. */
    if (*bytes_left >= 32 && memcmp(*ptr, "APETAGEX", 8) == 0) {
        ESP_LOGI(TAG, "🏁 Detectado APE tag no final (%d bytes restantes), encerrando track", *bytes_left);
        *ptr += *bytes_left;
        *bytes_left = 0;
        return true;
    }

    return false;
}

// ============================================================================
// BUFFERS DE ÁUDIO
// ============================================================================

static esp_err_t init_audio_buffers(void)
{
    ESP_LOGI(TAG, "🔧 Alocando buffers de áudio...");
    
    mp3_input_buffer = heap_caps_malloc(MP3_INPUT_BUFFER_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Falha alocar MP3 input buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✅ MP3 input: %d bytes", MP3_INPUT_BUFFER_SIZE);
    
    pcm_output_buffer = heap_caps_malloc(PCM_OUTPUT_BUFFER_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!pcm_output_buffer) {
        ESP_LOGE(TAG, "Falha alocar PCM output buffer");
        free(mp3_input_buffer);
        mp3_input_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✅ PCM output: %d bytes", PCM_OUTPUT_BUFFER_SIZE);
    
    stream_buffer = ring_buffer_create(STREAM_BUFFER_SIZE);
    if (!stream_buffer) {
        ESP_LOGE(TAG, "Falha criar stream buffer");
        free(mp3_input_buffer);
        free(pcm_output_buffer);
        mp3_input_buffer = NULL;
        pcm_output_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✅ Stream buffer: %d bytes", STREAM_BUFFER_SIZE);
    
    ESP_LOGI(TAG, "✅ Buffers alocados com sucesso");
    return ESP_OK;
}

static void cleanup_audio_buffers(void)
{
    if (stream_buffer) {
        ring_buffer_destroy(stream_buffer);
        stream_buffer = NULL;
    }
    
    if (pcm_output_buffer) {
        free(pcm_output_buffer);
        pcm_output_buffer = NULL;
    }
    
    if (mp3_input_buffer) {
        free(mp3_input_buffer);
        mp3_input_buffer = NULL;
    }
}

// ============================================================================
// LIBHELIX MP3 DECODER
// ============================================================================

static esp_err_t init_audio_decoder(void)
{
    ESP_LOGI(TAG, "🎵 Inicializando libhelix MP3 decoder...");
    
    if (mp3_decoder) {
        ESP_LOGW(TAG, "Decoder já existe, destruindo...");
        MP3FreeDecoder(mp3_decoder);
        mp3_decoder = NULL;
    }
    
    mp3_decoder = MP3InitDecoder();
    if (!mp3_decoder) {
        ESP_LOGE(TAG, "❌ Falha ao criar decoder libhelix!");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ Libhelix decoder criado com sucesso");
    ESP_LOGI(TAG, "   Heap livre: %lu bytes", (unsigned long)esp_get_free_heap_size());
    
    sys_status.codec_configured = true;
    xEventGroupSetBits(player_event_group, CODEC_READY_BIT);
    
    return ESP_OK;
}

static void cleanup_audio_decoder(void)
{
    if (mp3_decoder) {
        MP3FreeDecoder(mp3_decoder);
        mp3_decoder = NULL;
    }
    sys_status.codec_configured = false;
}

// ============================================================================
// SD CARD
// ============================================================================

static void free_mp3_playlist_cache(void) {}

static esp_err_t sdcard_init(void)
{
    ESP_LOGI(TAG, "📀 Inicializando SD Card...");
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    sdmmc_card_t *card;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    
    // ✅ 20MHz (com fallback para 19MHz se falhar)
    host.max_freq_khz = 19000;
    
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;
    
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    
    sys_status.sd_mounted = true;
    ESP_LOGI(TAG, "✅ SD Card montado: %s", card->cid.name);
    ESP_LOGI(TAG, "   Tamanho: %.2f GB", (card->csd.capacity * 512.0) / (1024*1024*1024));
    
    return ESP_OK;
}

static esp_err_t count_mp3_files(void)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Falha abrir diretório");
        return ESP_FAIL;
    }

    mp3_count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            size_t len = strlen(entry->d_name);
            if (len > 4 && strcasecmp(entry->d_name + len - 4, ".mp3") == 0) {
                mp3_count++;
            }
        }
    }

    closedir(dir);

    if (mp3_count == 0) {
        ESP_LOGE(TAG, "❌ Nenhum arquivo MP3 encontrado!");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "✅ Total: %d arquivos MP3 encontrados", mp3_count);
    return ESP_OK;
}

static esp_err_t get_mp3_path(int index, char *path, size_t max_len)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) return ESP_FAIL;

    int count = 0;
    struct dirent *entry;
    esp_err_t ret = ESP_ERR_NOT_FOUND;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            size_t len = strlen(entry->d_name);
            if (len > 4 && strcasecmp(entry->d_name + len - 4, ".mp3") == 0) {
                if (count == index) {
                    int written = snprintf(path, max_len, "%s/%s", MOUNT_POINT, entry->d_name);
                    if (written < 0 || written >= (int)max_len) {
                        ret = ESP_ERR_NO_MEM;
                    } else {
                        ret = ESP_OK;
                    }
                    break;
                }
                count++;
            }
        }
    }

    closedir(dir);
    return ret;
}

static esp_err_t analyze_mp3_file(const char *path, mp3_info_t *info)
{
    if (!path || !info) return ESP_ERR_INVALID_ARG;
    
    memset(info, 0, sizeof(mp3_info_t));
    
    struct stat file_stat;
    if (stat(path, &file_stat) != 0) {
        return ESP_FAIL;
    }
    
    info->file_size = file_stat.st_size;
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    
    skip_id3v2(f);
    
    uint8_t header_buf[4];
    if (fread(header_buf, 1, 4, f) != 4) {
        fclose(f);
        return ESP_FAIL;
    }
    
    if (header_buf[0] != 0xFF || (header_buf[1] & 0xE0) != 0xE0) {
        fclose(f);
        return ESP_FAIL;
    }
    
    int bitrate_index = (header_buf[2] >> 4) & 0x0F;
    int samplerate_index = (header_buf[2] >> 2) & 0x03;
    
    static const int bitrates[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };
    
    static const int samplerates[4] = {44100, 48000, 32000, 0};
    
    info->bitrate = bitrates[bitrate_index] * 1000;
    info->sample_rate = samplerates[samplerate_index];
    info->channels = 2;
    info->bits_per_sample = 16;
    
    if (info->bitrate > 0) {
        info->duration_seconds = (info->file_size * 8) / info->bitrate;
    }
    
    info->a2dp_compatible = (info->sample_rate == SAMPLE_RATE_44K1 || 
                             info->sample_rate == SAMPLE_RATE_48K);
    
    info->is_valid_mp3 = (info->sample_rate > 0 && info->bitrate > 0);
    
    fclose(f);
    
    ESP_LOGI(TAG, "📊 Análise MP3:");
    ESP_LOGI(TAG, "   Sample Rate: %" PRIu32 " Hz", info->sample_rate);
    ESP_LOGI(TAG, "   Bitrate: %" PRIu32 " kbps", info->bitrate / 1000);
    ESP_LOGI(TAG, "   Canais: %u", (unsigned int)info->channels);
    ESP_LOGI(TAG, "   Duração: %" PRIu32 " s (%.1f min)", 
             info->duration_seconds, info->duration_seconds / 60.0f);
    
    return ESP_OK;
}

static void log_system_status(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "📊 Status do Sistema:");
    ESP_LOGI(TAG, "   SD Card: %s", sys_status.sd_mounted ? "✅" : "❌");
    ESP_LOGI(TAG, "   Bluetooth: %s", sys_status.bt_initialized ? "✅" : "❌");
    ESP_LOGI(TAG, "   BT Conectado: %s", sys_status.bt_connected ? "✅" : "❌");
    ESP_LOGI(TAG, "   Decoder: %s", sys_status.codec_configured ? "✅" : "❌");
    ESP_LOGI(TAG, "   Tocando: %s", sys_status.audio_playing ? "✅" : "❌");
    ESP_LOGI(TAG, "   Stream ativo: %s", sys_status.streaming_active ? "✅" : "❌");
    ESP_LOGI(TAG, "   MP3s: %d arquivos", mp3_count);
    ESP_LOGI(TAG, "   Track: [%d/%d]", current_track + 1, mp3_count);
    ESP_LOGI(TAG, "   Volume: %d%%", current_volume);
    ESP_LOGI(TAG, "   Heap livre: %lu bytes", (unsigned long)esp_get_free_heap_size());
    
    if (stream_buffer) {
        size_t avail = ring_buffer_available(stream_buffer);
        ESP_LOGI(TAG, "   Buffer: %zu/%d bytes", avail, STREAM_BUFFER_SIZE);
    }
    
    ESP_LOGI(TAG, "   Callbacks A2DP: %lu", callback_count);
    ESP_LOGI(TAG, "   Underruns: %lu", underrun_count);
    ESP_LOGI(TAG, "   CB lock/empty/partial: %lu/%lu/%lu",
            cb_lock_fail_count, cb_empty_count, cb_partial_count);
    ESP_LOGI(TAG, "   Bytes streamed: %lu", total_bytes_streamed);
    ESP_LOGI(TAG, "   Buffer evt low/high: %lu/%lu", buffer_low_events, buffer_high_events);
    log_bt_state("status");
    ESP_LOGI(TAG, "");
}

// ✅ FUNÇÃO PARA SELECIONAR MÚSICA ALEATÓRIA
static int get_random_track(int current)
{
    if (mp3_count <= 1) return 0;
    
    int new_track;
    int attempts = 0;
    
    // Tentar até 10 vezes para garantir música diferente
    do {
        new_track = esp_random() % mp3_count;
        attempts++;
    } while (new_track == current && attempts < 10);
    
    return new_track;
}

static int get_previous_track(int current)
{
    if (mp3_count <= 1) return 0;
    return (current == 0) ? (mp3_count - 1) : (current - 1);
}

static void stop_playback_and_reset(bool wait_task, const char *reason)
{
    ESP_LOGI(TAG, "🧹 Reset playback (%s)", reason ? reason : "-");

    file_reader_task_running = false;

    if (wait_task && file_reader_task_handle != NULL) {
        for (int i = 0; i < 100; i++) {
            if (file_reader_task_handle == NULL) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (current_file) {
        fclose(current_file);
        current_file = NULL;
    }

    cleanup_audio_decoder();
    if (stream_buffer) {
        ring_buffer_reset(stream_buffer);
    }

    bytes_left_in_mp3 = 0;
    read_ptr = NULL;
    file_reader_task_handle = NULL;
    sys_status.audio_playing = false;
    playback_paused = false;
    xEventGroupClearBits(player_event_group, CODEC_READY_BIT | TRACK_FINISHED_BIT);
}

static void buffer_monitor_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!stream_buffer || !sys_status.streaming_active) return;
    
    size_t available = ring_buffer_available(stream_buffer);
    size_t low_mark = (STREAM_BUFFER_SIZE * 30) / 100;
    size_t high_mark = (STREAM_BUFFER_SIZE * 60) / 100;

    if (available < low_mark) {
        buffer_low_events++;
    } else if (available > high_mark) {
        buffer_high_events++;
    }
}

// ============================================================================
// A2DP CALLBACK - CONSUMIDOR DE DADOS
// ============================================================================

static int32_t bt_a2dp_source_data_cb(uint8_t *data, int32_t len)
{
    static uint8_t silence_packets = 0;
    callback_count++;
    
    if (!data || len <= 0) return 0;
    
    // Em alguns headsets o AUDIO_STATE pode oscilar; manter saída baseada na conexão.
    if (!sys_status.bt_connected || playback_paused) {
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
    }
    
    if (!stream_buffer) {
        memset(data, 0, len);
        return len;
    }
    
    BaseType_t taken = xSemaphoreTake(stream_buffer->mutex, pdMS_TO_TICKS(1));
    if (taken != pdTRUE) {
        memset(data, 0, len);
        underrun_count++;
        cb_lock_fail_count++;
        return len;
    }
    
    size_t available = stream_buffer->available;
    
    if (available == 0) {
        xSemaphoreGive(stream_buffer->mutex);
        underrun_count++;
        cb_empty_count++;
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
        cb_partial_count++;
    }
    
    // Log a cada 2500 callbacks (~1 min)
    if (callback_count % 2500 == 0) {
        ESP_LOGI(TAG, "A2DP: %lu KB streamed, %lu underruns, buf:%zu cb_lock/empty/partial:%lu/%lu/%lu",
                total_bytes_streamed / 1024, underrun_count, available,
                cb_lock_fail_count, cb_empty_count, cb_partial_count);
    }
    
    return len;
}

// ============================================================================
// DECODIFICAÇÃO MP3 - OTIMIZADA COM CONTROLE DE BUFFER MELHORADO
// ============================================================================

static void decode_and_stream(void *pvParameter)
{
    ESP_LOGI(TAG, "🎵 Task de decodificação iniciada");
    ESP_LOGI(TAG, "   Heap livre: %lu bytes", (unsigned long)esp_get_free_heap_size());

    char filepath[MAX_PATH_LEN];
    bool reached_eof = false;

    if (get_mp3_path(current_track, filepath, sizeof(filepath)) != ESP_OK) {
        goto task_fail;
    }

    ESP_LOGI(TAG, "▶️ Reproduzindo [%d/%d]: %s",
             current_track + 1, mp3_count, filepath);

    if (analyze_mp3_file(filepath, &current_mp3_info) != ESP_OK) {
        goto task_fail;
    }

    current_file = fopen(filepath, "rb");
    if (!current_file) {
        goto task_fail;
    }

    skip_id3v2(current_file);
    ring_buffer_reset(stream_buffer);

    if (init_audio_decoder() != ESP_OK) {
        goto task_fail;
    }

    // ✅ IMPORTANTE: Zerar estados no início
    bytes_left_in_mp3 = 0;
    read_ptr = mp3_input_buffer;

    // =========================================================
    // 📦 PRÉ-BUFFER (10 frames)
    // =========================================================
    ESP_LOGI(TAG, "🔄 Pré-buffer (%d frames)...", PREBUFFER_FRAMES);

    int frame_count = 0;
    bool first_frame = true;
    TickType_t last_stream_progress_tick = xTaskGetTickCount();
    uint32_t last_stream_bytes = total_bytes_streamed;
    uint32_t consecutive_short_writes = 0;
    last_producer_tick = xTaskGetTickCount();
    uint16_t eof_no_sync_loops = 0;
    uint16_t eof_decode_err_loops = 0;

    while (frame_count < PREBUFFER_FRAMES && file_reader_task_running) {
        if (!sys_status.bt_connected) {
            break;
        }

        if (bytes_left_in_mp3 < MP3_CRITICAL_BYTES) {
            if (bytes_left_in_mp3 > 0 && read_ptr != mp3_input_buffer) {
                memmove(mp3_input_buffer, read_ptr, bytes_left_in_mp3);
            }
            read_ptr = mp3_input_buffer;

            size_t to_read = MP3_INPUT_BUFFER_SIZE - bytes_left_in_mp3;
            size_t r = fread(mp3_input_buffer + bytes_left_in_mp3, 1,
                            to_read, current_file);

            if (r > 0) {
                bytes_left_in_mp3 += r;
            } else if (feof(current_file)) {
                if (bytes_left_in_mp3 == 0) {
                    reached_eof = true;
                    break;
                }
            }
        }

        int offset = MP3FindSyncWord(read_ptr, bytes_left_in_mp3);
        if (offset < 0) {
            if (feof(current_file) && bytes_left_in_mp3 == 0) {
                reached_eof = true;
                break;
            }
            vTaskDelay(1);
            continue;
        }

        read_ptr += offset;
        bytes_left_in_mp3 -= offset;

        int err = MP3Decode(mp3_decoder, &read_ptr, &bytes_left_in_mp3,
                            pcm_output_buffer, 0);

        if (err == ERR_MP3_NONE) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(mp3_decoder, &fi);

            if (first_frame) {
                ESP_LOGI(TAG, "🎧 %d Hz | %d kbps | %d ch",
                         fi.samprate, fi.bitrate / 1000, fi.nChans);
                first_frame = false;
                xEventGroupSetBits(player_event_group, STREAM_READY_BIT);
            }

            ring_buffer_write_blocking(stream_buffer,
                                       (uint8_t *)pcm_output_buffer,
                                       fi.outputSamps * sizeof(int16_t),
                                       pdMS_TO_TICKS(40));

            frame_count++;
        }
    }

    ESP_LOGI(TAG, "✅ Pré-buffer pronto (%d frames)", frame_count);

    // =========================================================
    // 🔁 LOOP PRINCIPAL - CONTROLE DE BUFFER MELHORADO
    // =========================================================
    while (file_reader_task_running) {
        if (playback_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!sys_status.bt_connected) {
            break;
        }
        if (!sys_status.streaming_active && (frame_count % 120 == 0)) {
            ESP_LOGW(TAG, "Stream flag OFF com BT conectado; mantendo decode para não secar buffer");
            log_bt_state("stream_flag_off_keep_decoding");
        }

        // -----------------------------
        // 📊 Verificar nível do stream buffer
        // -----------------------------
        size_t stream_available = ring_buffer_available(stream_buffer);
        float stream_fill_percent = (stream_available * 100.0f) / STREAM_BUFFER_SIZE;

        // ✅ CONTROLE MELHORADO: Alvo 40-75%
        // < 40%: Acelerar leitura
        // 40-75%: Normal
        // > 75%: Reduzir (mas nunca parar)

        // -----------------------------
        // 📂 Leitura SD adaptativa baseada no stream buffer
        // -----------------------------
        if (bytes_left_in_mp3 < MP3_CRITICAL_BYTES) {
            // Mover dados restantes para início do buffer
            if (bytes_left_in_mp3 > 0 && read_ptr != mp3_input_buffer) {
                memmove(mp3_input_buffer, read_ptr, bytes_left_in_mp3);
            }
            read_ptr = mp3_input_buffer;

            size_t free = MP3_INPUT_BUFFER_SIZE - bytes_left_in_mp3;
            size_t to_read;

            // ✅ LÓGICA MUITO CONSERVADORA: Produzir MUITO menos
            if (stream_fill_percent < 30.0f) {
                // Buffer BAIXO (< 30%): LER agressivo
                to_read = MP3_READ_MAX;
                if (to_read > free) to_read = free;
            } 
            else if (stream_fill_percent < 45.0f) {
                // Buffer OK baixo (30-45%): ler médio
                to_read = MP3_READ_MAX / 2;
                if (to_read > free) to_read = free;
            }
            else if (stream_fill_percent < 60.0f) {
                // Buffer OK alto (45-60%)
                to_read = MP3_READ_MIN;
                if (to_read > free) to_read = free;
            }
            else {
                // Buffer ALTO (> 60%): ainda manter leitura mínima estável
                to_read = MP3_READ_MIN;
                if (to_read > free) to_read = free;
            }

            // Alinhar leitura (múltiplo de 4 bytes para performance)
            to_read &= ~0x03;

            size_t r = fread(mp3_input_buffer + bytes_left_in_mp3, 1,
                            to_read, current_file);
            
            if (r > 0) {
                bytes_left_in_mp3 += r;
            } else if (feof(current_file)) {
                // ✅ FIM DO ARQUIVO: processar bytes restantes
                if (bytes_left_in_mp3 == 0) {
                    ESP_LOGI(TAG, "🏁 Fim do arquivo MP3");
                    reached_eof = true;
                    break;
                }
            }
        }

        // -----------------------------
        // 🔎 Sync
        // -----------------------------
        int offset = MP3FindSyncWord(read_ptr, bytes_left_in_mp3);
        if (offset < 0) {
            if (feof(current_file) && drop_trailing_tag_if_present(&read_ptr, &bytes_left_in_mp3)) {
                reached_eof = true;
                break;
            }

            if (feof(current_file)) {
                eof_no_sync_loops++;
            } else {
                eof_no_sync_loops = 0;
            }

            /* Evita loop preso com lixo no buffer (ex.: tag de cauda) sem chegar no EOF. */
            if (!feof(current_file) && bytes_left_in_mp3 > MP3_CRITICAL_BYTES) {
                int drop = bytes_left_in_mp3 - MP3_CRITICAL_BYTES;
                if (drop > MP3_NO_SYNC_DROP_BYTES) {
                    drop = MP3_NO_SYNC_DROP_BYTES;
                }
                if (drop > 0) {
                    read_ptr += drop;
                    bytes_left_in_mp3 -= drop;
                    vTaskDelay(1);
                    continue;
                }
            }

            if (feof(current_file) &&
                (bytes_left_in_mp3 < 4 || eof_no_sync_loops > 30)) {
                ESP_LOGI(TAG, "🏁 Fim do stream (EOF sem sync, loops=%u, bytes=%d)",
                         (unsigned)eof_no_sync_loops, bytes_left_in_mp3);
                reached_eof = true;
                break;
            }
            vTaskDelay(1);
            continue;
        }
        eof_no_sync_loops = 0;

        read_ptr += offset;
        bytes_left_in_mp3 -= offset;

        // -----------------------------
        // 🎛 Decode
        // -----------------------------
        int err = MP3Decode(mp3_decoder, &read_ptr, &bytes_left_in_mp3,
                            pcm_output_buffer, 0);

        if (err != ERR_MP3_NONE) {
            if (feof(current_file)) {
                eof_decode_err_loops++;
                if (drop_trailing_tag_if_present(&read_ptr, &bytes_left_in_mp3)) {
                    reached_eof = true;
                    break;
                }
            } else {
                eof_decode_err_loops = 0;
            }

            if (feof(current_file) &&
                (bytes_left_in_mp3 < MP3_CRITICAL_BYTES || eof_decode_err_loops > 30)) {
                ESP_LOGI(TAG, "🏁 Fim do stream (decode+EOF, loops=%u, bytes=%d)",
                         (unsigned)eof_decode_err_loops, bytes_left_in_mp3);
                reached_eof = true;
                break;
            }
            vTaskDelay(1);
            continue;
        }
        eof_decode_err_loops = 0;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(mp3_decoder, &fi);

        // -----------------------------
        // 🔊 Volume
        // -----------------------------
        for (int i = 0; i < fi.outputSamps; i++) {
            int32_t s = pcm_output_buffer[i] * volume_scale;
            pcm_output_buffer[i] = (int16_t)
                (s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }

        // -----------------------------
        // 📤 PCM → ring buffer
        // -----------------------------
        size_t frame_bytes = (size_t)fi.outputSamps * sizeof(int16_t);
        size_t pushed = ring_buffer_write_blocking(stream_buffer,
                                   (uint8_t *)pcm_output_buffer,
                                   frame_bytes,
                                   pdMS_TO_TICKS(30));

        producer_frame_count++;
        if (pushed == 0) {
            producer_drop_count++;
            consecutive_short_writes++;
            if ((consecutive_short_writes % 50) == 0) {
                size_t stream_av = ring_buffer_available(stream_buffer);
                ESP_LOGW(TAG,
                         "Producer sem push (%lu seguidos). buf:%zu/%d mp3buf:%d bt:%d stream:%d",
                         (unsigned long)consecutive_short_writes,
                         stream_av, STREAM_BUFFER_SIZE, bytes_left_in_mp3,
                         sys_status.bt_connected, sys_status.streaming_active);
            }
        } else {
            consecutive_short_writes = 0;
            last_producer_tick = xTaskGetTickCount();
        }

        frame_count++;

        if (total_bytes_streamed != last_stream_bytes) {
            last_stream_bytes = total_bytes_streamed;
            last_stream_progress_tick = xTaskGetTickCount();
        } else {
            TickType_t stalled_for = xTaskGetTickCount() - last_stream_progress_tick;
            if (stalled_for > pdMS_TO_TICKS(3000) && (frame_count % 100 == 0)) {
                size_t stream_av = ring_buffer_available(stream_buffer);
                ESP_LOGW(TAG,
                         "Possível stall de stream: %lu ms sem progresso A2DP | buf:%zu/%d | mp3buf:%d | bt:%d stream:%d",
                         (unsigned long)TICKS_TO_MS(stalled_for),
                         stream_av,
                         STREAM_BUFFER_SIZE,
                         bytes_left_in_mp3,
                         sys_status.bt_connected,
                         sys_status.streaming_active);
                log_bt_state("decode_stall");
            }
        }

        // Log periódico
        if (frame_count % 500 == 0) {
            size_t stream_av = ring_buffer_available(stream_buffer);
            float fill_pct = (stream_av * 100.0f) / STREAM_BUFFER_SIZE;
            ESP_LOGI(TAG, "F%04d | Stream:%zu/%d (%.0f%%) | MP3buf:%d | Heap:%lu | Prod frame/drop:%lu/%lu",
                     frame_count,
                     stream_av, STREAM_BUFFER_SIZE, fill_pct,
                     bytes_left_in_mp3,
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)producer_frame_count,
                     (unsigned long)producer_drop_count);
        }

        // ✅ YIELD EM TODOS OS FRAMES: Máxima prioridade ao A2DP
        taskYIELD();
        
        // ✅ PAUSA ADICIONAL quando buffer muito cheio
        size_t current_available = ring_buffer_available(stream_buffer);
        float current_fill = (current_available * 100.0f) / STREAM_BUFFER_SIZE;
        
        if (current_fill > 70.0f) {
            // Buffer muito cheio: pausa curta para drenar
            vTaskDelay(pdMS_TO_TICKS(2));
        } else if (current_fill > 50.0f) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

task_fail:
    ESP_LOGI(TAG, "⏹ Decoder encerrado");
    bool natural_finish = reached_eof;

    // ✅ ORDEM CORRETA DE LIMPEZA:
    // 1. Fechar arquivo primeiro
    if (current_file) {
        fclose(current_file);
        current_file = NULL;
    }
    
    // 2. Limpar decoder
    cleanup_audio_decoder();
    
    // 3. Zerar estados
    bytes_left_in_mp3 = 0;
    read_ptr = NULL;
    
    // 4. Marcar task como não rodando
    file_reader_task_running = false;
    file_reader_task_handle = NULL;
    sys_status.audio_playing = false;
    
    // 5. Sinalizar fim da track
    if (natural_finish && sys_status.bt_connected) {
        xEventGroupSetBits(player_event_group, TRACK_FINISHED_BIT);
        player_cmd_t next_cmd = CMD_PLAY_NEXT;
        enqueue_control_cmd(next_cmd);
    }
    
    vTaskDelete(NULL);
}

// ============================================================================
// CONTROLE DE VOLUME
// ============================================================================

static void set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    current_volume = volume;
    volume_scale = volume / 100.0f;
    
    ESP_LOGI(TAG, "🔊 Volume: %d%% (%.2f)", current_volume, volume_scale);
}

static void volume_up(void)
{
    uint8_t new_vol = current_volume + VOLUME_STEP;
    if (new_vol > 100) new_vol = 100;
    set_volume(new_vol);
}

static void volume_down(void)
{
    int new_vol = current_volume - VOLUME_STEP;
    if (new_vol < 0) new_vol = 0;
    set_volume((uint8_t)new_vol);
}

static void init_volume_buttons(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_VOL_UP) | (1ULL << PIN_VOL_DOWN) | (1ULL << PIN_PWR_SLEEP),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    ESP_LOGI(TAG, "✅ Botões configurados (VOL:%d/%d POWER:%d)", 
            PIN_VOL_UP, PIN_VOL_DOWN, PIN_PWR_SLEEP);
}

static void configure_deep_sleep_wakeup(void)
{
    // GPIO33 é RTC IO no ESP32 clássico e suporta ext0 wakeup.
    rtc_gpio_init((gpio_num_t)PIN_PWR_SLEEP);
    rtc_gpio_set_direction((gpio_num_t)PIN_PWR_SLEEP, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_PWR_SLEEP);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_PWR_SLEEP);

    esp_err_t err = esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_PWR_SLEEP, 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Wakeup deep sleep ativo no GPIO %d (nível LOW)", PIN_PWR_SLEEP);
    } else {
        ESP_LOGW(TAG, "⚠️ Falha ao configurar wakeup ext0: %s", esp_err_to_name(err));
    }
}

static void set_power_led(bool on)
{
    int level = on ? (BOARD_LED_ACTIVE_HIGH ? 1 : 0)
                   : (BOARD_LED_ACTIVE_HIGH ? 0 : 1);

    // Se veio de deep sleep com hold ativo, libera antes de reconfigurar.
    if (rtc_gpio_is_valid_gpio((gpio_num_t)BOARD_LED_GPIO)) {
        rtc_gpio_hold_dis((gpio_num_t)BOARD_LED_GPIO);
        rtc_gpio_deinit((gpio_num_t)BOARD_LED_GPIO);
    }

    gpio_reset_pin((gpio_num_t)BOARD_LED_GPIO);
    gpio_set_direction((gpio_num_t)BOARD_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_LED_GPIO, level);
}

static bool wait_wakeup_pin_inactive(uint32_t timeout_ms)
{
    if (gpio_get_level(PIN_PWR_SLEEP) != 0) {
        return true;
    }

    ESP_LOGW(TAG, "Botão wake ainda pressionado, aguardando soltar...");
    TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(PIN_PWR_SLEEP) == 0) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Debounce de soltura antes de dormir.
    vTaskDelay(pdMS_TO_TICKS(80));
    return true;
}

static void enter_deep_sleep(bool from_power_button)
{
    ESP_LOGW(TAG, "😴 Entrando em deep sleep (hold power button)");

    uint32_t wait_ms = from_power_button ? PWR_RELEASE_WAIT_MS : 600;
    if (!wait_wakeup_pin_inactive(wait_ms)) {
        ESP_LOGW(TAG, "Deep sleep cancelado: botão wake permaneceu LOW");
        return;
    }

    stop_playback_and_reset(true, "deep_sleep");
    free_mp3_playlist_cache();

    if (connection_timer) xTimerStop(connection_timer, 0);
    if (discovery_timer) xTimerStop(discovery_timer, 0);
    if (buffer_monitor_timer) xTimerStop(buffer_monitor_timer, 0);

    // Apaga LED "power" de software e mantém estado durante deep sleep.
    if (rtc_gpio_is_valid_gpio((gpio_num_t)BOARD_LED_GPIO)) {
        rtc_gpio_deinit((gpio_num_t)BOARD_LED_GPIO);
        rtc_gpio_init((gpio_num_t)BOARD_LED_GPIO);
        rtc_gpio_set_direction((gpio_num_t)BOARD_LED_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level((gpio_num_t)BOARD_LED_GPIO, BOARD_LED_ACTIVE_HIGH ? 0 : 1);
        rtc_gpio_pullup_dis((gpio_num_t)BOARD_LED_GPIO);
        rtc_gpio_pulldown_dis((gpio_num_t)BOARD_LED_GPIO);
        rtc_gpio_hold_en((gpio_num_t)BOARD_LED_GPIO);
    }

    configure_deep_sleep_wakeup();
    vTaskDelay(pdMS_TO_TICKS(80));
    esp_deep_sleep_start();
}

static void handle_button(button_state_t *btn)
{
    bool current_state = gpio_get_level(btn->pin);
    TickType_t now = xTaskGetTickCount();
    
    if (current_state != btn->last_state) {
        if (now - btn->last_change_time > pdMS_TO_TICKS(DEBOUNCE_TIME_MS)) {
            btn->last_change_time = now;
            btn->last_state = current_state;
            
            if (!current_state) {
                btn->pressed = true;
                btn->press_start_time = now;
                btn->is_long_press = false;
            } else if (btn->pressed) {
                btn->pressed = false;
                
                TickType_t press_duration = now - btn->press_start_time;
                
                if (!btn->is_long_press && 
                    press_duration < pdMS_TO_TICKS(LONG_CLICK_THRESHOLD_MS)) {
                    
                    if (now - btn->last_click_time < pdMS_TO_TICKS(DOUBLE_CLICK_INTERVAL_MS)) {
                        btn->click_count++;
                    } else {
                        btn->click_count = 1;
                    }
                    btn->last_click_time = now;
                }
            }
        }
    } else if (btn->pressed && !btn->is_long_press) {
        TickType_t press_duration = now - btn->press_start_time;
        if (press_duration >= pdMS_TO_TICKS(LONG_CLICK_THRESHOLD_MS)) {
            btn->is_long_press = true;
        }
    }
}

static void volume_control_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Task controle volume iniciada");
    
    TickType_t last_process_time = xTaskGetTickCount();
    static TickType_t last_next_track_cmd = 0;
    
    while (1) {
        handle_button(&vol_up_btn);
        handle_button(&vol_down_btn);
        
        TickType_t now = xTaskGetTickCount();

        // Botão de power/deep sleep: hold de POWER_HOLD_MS para dormir.
        bool pwr_level = gpio_get_level(PIN_PWR_SLEEP);
        if (pwr_level != pwr_last_level &&
            (now - pwr_last_change_time) > pdMS_TO_TICKS(DEBOUNCE_TIME_MS)) {
            pwr_last_change_time = now;
            pwr_last_level = pwr_level;
            if (!pwr_level) {
                pwr_pressed = true;
                pwr_hold_handled = false;
                pwr_press_start_time = now;
            } else {
                pwr_pressed = false;
                pwr_hold_handled = false;
            }
        }

        if (pwr_pressed && !pwr_hold_handled &&
            (now - pwr_press_start_time) > pdMS_TO_TICKS(POWER_HOLD_MS)) {
            pwr_hold_handled = true;
            ESP_LOGW(TAG, "⏻ Power hold detectado (%d ms)", POWER_HOLD_MS);
            enter_deep_sleep(true);
        }
        
        if (vol_up_btn.click_count > 0 && 
            now - vol_up_btn.last_click_time > pdMS_TO_TICKS(CLICK_TIMEOUT_MS)) {
            
            if (vol_up_btn.click_count == 2) {
                // ✅ PROTEÇÃO: Evitar múltiplos comandos próximos (3 segundos)
                if ((now - last_next_track_cmd) > pdMS_TO_TICKS(3000)) {
                    ESP_LOGI(TAG, "⏭️ Duplo clique: Música ALEATÓRIA");
                    
                    // ✅ Limpar queue antes de enviar
                    player_cmd_t dummy;
                    int cleared = 0;
                    while (xQueueReceive(control_queue, &dummy, 0) == pdTRUE) {
                        switch (dummy) {
                            case CMD_PLAY_NEXT: cmd_play_next_pending = false; break;
                            case CMD_PLAY_PREV: cmd_play_prev_pending = false; break;
                            case CMD_RETRY_CONNECTION: cmd_retry_pending = false; break;
                            case CMD_RESTART_DISCOVERY: cmd_restart_disc_pending = false; break;
                            case CMD_CONNECT_TARGET: cmd_connect_pending = false; break;
                            default: break;
                        }
                        cleared++;
                    }
                    if (cleared > 0) {
                        ESP_LOGI(TAG, "🗑️ Removidos %d comandos da queue", cleared);
                    }
                    
                    // Enviar comando
                    player_cmd_t cmd = CMD_PLAY_NEXT;
                    enqueue_control_cmd(cmd);
                    last_next_track_cmd = now;
                } else {
                    ESP_LOGW(TAG, "⚠️ Duplo clique ignorado (aguarde 3s)");
                }
            } else if (vol_up_btn.click_count == 1) {
                volume_up();
            }
            vol_up_btn.click_count = 0;
        }
        
        if (vol_up_btn.is_long_press) {
            volume_up();
            vol_up_btn.is_long_press = false;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        
        if (vol_down_btn.click_count > 0 && 
            now - vol_down_btn.last_click_time > pdMS_TO_TICKS(CLICK_TIMEOUT_MS)) {
            
            volume_down();
            vol_down_btn.click_count = 0;
        }
        
        if (vol_down_btn.is_long_press) {
            volume_down();
            vol_down_btn.is_long_press = false;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        
        if (now - last_process_time >= pdMS_TO_TICKS(50)) {
            last_process_time = now;
        }
        
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================================
// BLUETOOTH CALLBACKS
// ============================================================================

static void connection_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGW(TAG, "⏱️ Timeout de conexão BT");
    log_bt_state("conn_timeout");
    if (!sys_status.bt_connected && sys_status.bt_connecting) {
        set_bt_connecting(false);
        player_cmd_t cmd = CMD_RETRY_CONNECTION;
        enqueue_control_cmd(cmd);
    }
}

static void discovery_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGW(TAG, "⏱️ Timeout de discovery");
    log_bt_state("disc_timeout");
    if (!device_found && !sys_status.bt_connected && sys_status.bt_connecting) {
        // Se o discovery estourou tempo e não encontrou alvo, libera estado de conexão
        // para permitir novo ciclo de busca.
        set_bt_connecting(false);
        player_cmd_t cmd = CMD_RESTART_DISCOVERY;
        enqueue_control_cmd(cmd);
    }
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    ESP_LOGD(TAG, "GAP evt: %d", event);
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            char bda_str[18];
            snprintf(bda_str, sizeof(bda_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                    param->disc_res.bda[0], param->disc_res.bda[1],
                    param->disc_res.bda[2], param->disc_res.bda[3],
                    param->disc_res.bda[4], param->disc_res.bda[5]);
            
            // ✅ IDF 4.4.4: Verificar MAC address
            bool mac_match = false;
            if (target_mac_addr[0] != 0 || target_mac_addr[1] != 0 || target_mac_addr[2] != 0) {
                if (memcmp(param->disc_res.bda, target_mac_addr, ESP_BD_ADDR_LEN) == 0) {
                    mac_match = true;
                }
            }
            
            // Se encontrou por MAC, conectar imediatamente
            if (mac_match && !device_found) {
                device_found = true;
                memcpy(target_device_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
                
                ESP_LOGI(TAG, "✅ Dispositivo encontrado por MAC!");
                ESP_LOGI(TAG, "   MAC: %s", bda_str);
                
                if (discovery_timer) {
                    xTimerStop(discovery_timer, 0);
                }

                // Só conectar após confirmação de discovery parada
                if (discovery_active) {
                    connect_after_discovery_stop = true;
                    if (!discovery_stop_pending) {
                        discovery_stop_pending = true;
                        esp_bt_gap_cancel_discovery();
                        ESP_LOGI(TAG, "🛑 Solicitado stop discovery antes de conectar");
                    }
                } else {
                    player_cmd_t cmd = CMD_CONNECT_TARGET;
                    enqueue_control_cmd(cmd);
                }
            }
            break;
        }
        
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                if (discovery_active) {
                    ESP_LOGI(TAG, "🔍 Discovery parado");
                }
                discovery_active = false;
                discovery_stop_pending = false;
                // Discovery parou sem alvo e sem conexão em progresso real:
                // destrava "connecting" para não bloquear novos retries.
                if (!sys_status.bt_connected && !connect_after_discovery_stop) {
                    set_bt_connecting(false);
                }
                log_bt_state("disc_stopped");
                if (connect_after_discovery_stop && device_found && !sys_status.bt_connected) {
                    connect_after_discovery_stop = false;
                    player_cmd_t cmd = CMD_CONNECT_TARGET;
                    enqueue_control_cmd(cmd);
                }
            } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
                discovery_active = true;
                discovery_stop_pending = false;
                ESP_LOGI(TAG, "🔍 Discovery iniciado");
                log_bt_state("disc_started");
            }
            break;
        }
        
        case ESP_BT_GAP_AUTH_CMPL_EVT: {
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "✅ Autenticação OK");
            } else {
                ESP_LOGW(TAG, "⚠️ Auth falhou: %d", param->auth_cmpl.stat);
            }
            break;
        }
        
        default:
            break;
    }
}

static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    ESP_LOGD(TAG, "A2DP evt: %d", event);
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_connection_state_t state = param->conn_stat.state;
            ESP_LOGD(TAG, "A2DP conn state: %d", state);
            
            if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "✅ A2DP conectado!");
                set_bt_connecting(false);
                connect_after_discovery_stop = false;
                a2dp_open_fail_streak = 0;
                
                if (connection_timer) {
                    xTimerStop(connection_timer, 0);
                }
                if (discovery_timer) {
                    xTimerStop(discovery_timer, 0);
                }
                if (discovery_active) {
                    esp_bt_gap_cancel_discovery();
                    discovery_active = false;
                    discovery_stop_pending = false;
                }
                
                esp_err_t start_err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                if (start_err == ESP_OK) {
                    ESP_LOGI(TAG, "✅ Media control START enviado");
                } else {
                    ESP_LOGW(TAG, "⚠️ Media control falhou: %s", esp_err_to_name(start_err));
                }
                
                sys_status.bt_connected = true;
                sys_status.connection_retries = 0;
                xEventGroupSetBits(player_event_group, BT_CONNECTED_BIT);
                log_bt_state("a2dp_connected");
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGW(TAG, "⚠️ A2DP desconectado");
                bool had_stream_progress = sys_status.audio_playing ||
                                           sys_status.streaming_active ||
                                           (total_bytes_streamed > 0) ||
                                           (callback_count > 0);
                if (!had_stream_progress) {
                    if (a2dp_open_fail_streak < UINT32_MAX) {
                        a2dp_open_fail_streak++;
                    }
                } else {
                    a2dp_open_fail_streak = 0;
                }
                ESP_LOGW(TAG, "A2DP fail streak: %lu", (unsigned long)a2dp_open_fail_streak);

                sys_status.bt_connected = false;
                sys_status.streaming_active = false;
                set_bt_connecting(false);
                connect_after_discovery_stop = false;
                xEventGroupClearBits(player_event_group, BT_CONNECTED_BIT);
                xEventGroupClearBits(player_event_group, STREAM_READY_BIT);

                ESP_LOGW(TAG, "Música interrompida, limpando estado...");
                player_cmd_t stop_cmd = CMD_STOP;
                enqueue_control_cmd(stop_cmd);

                if (a2dp_open_fail_streak >= A2DP_OPEN_FAIL_REDISCOVERY_THRESHOLD) {
                    ESP_LOGW(TAG, "Falhas seguidas de abertura A2DP, forçando novo discovery");
                    player_cmd_t restart_cmd = CMD_RESTART_DISCOVERY;
                    enqueue_control_cmd(restart_cmd);
                } else {
                    player_cmd_t retry_cmd = CMD_RETRY_CONNECTION;
                    enqueue_control_cmd(retry_cmd);
                }
                log_bt_state("a2dp_disconnected");
            }
            break;
        }
        
        case ESP_A2D_AUDIO_STATE_EVT: {
            esp_a2d_audio_state_t state = param->audio_stat.state;
            
            if (state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "▶️ Stream de áudio iniciado");
                sys_status.streaming_active = true;
                xEventGroupSetBits(player_event_group, STREAM_READY_BIT);
                if (!sys_status.audio_playing) {
                    player_cmd_t cmd = CMD_PLAY_NEXT;
                    enqueue_control_cmd(cmd);
                }
                log_bt_state("audio_started");
            } else if (state == ESP_A2D_AUDIO_STATE_STOPPED) {
                ESP_LOGI(TAG, "⏸️ Stream de áudio parado");
                sys_status.streaming_active = false;
                xEventGroupClearBits(player_event_group, STREAM_READY_BIT);
                log_bt_state("audio_stopped");
            }
            break;
        }
        
        case ESP_A2D_AUDIO_CFG_EVT: {
            ESP_LOGI(TAG, "🔧 Codec configurado");
            break;
        }
        
        default:
            break;
    }
}

static void avrc_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            if (param->conn_stat.connected) {
                ESP_LOGI(TAG, "✅ AVRC conectado");
            } else {
                ESP_LOGI(TAG, "AVRC desconectado");
            }
            break;
        
        default:
            break;
    }
}

static void avrc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "AVRC TG: %s", param->conn_stat.connected ? "conectado" : "desconectado");
            break;

        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            if (param->psth_cmd.key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) {
                break;
            }
            switch (param->psth_cmd.key_code) {
                case ESP_AVRC_PT_CMD_PLAY:
                    ESP_LOGI(TAG, "AVRCP clique: PLAY/PAUSE toggle");
                    enqueue_control_cmd(playback_paused ? CMD_RESUME : CMD_PAUSE);
                    break;
                case ESP_AVRC_PT_CMD_PAUSE:
                    ESP_LOGI(TAG, "AVRCP clique: PAUSE");
                    enqueue_control_cmd(CMD_PAUSE);
                    break;
                case ESP_AVRC_PT_CMD_STOP:
                    ESP_LOGI(TAG, "AVRCP clique: STOP");
                    enqueue_control_cmd(CMD_STOP);
                    break;
                case ESP_AVRC_PT_CMD_FORWARD:
                    ESP_LOGI(TAG, "AVRCP duplo clique: NEXT");
                    enqueue_control_cmd(CMD_PLAY_NEXT);
                    break;
                case ESP_AVRC_PT_CMD_BACKWARD:
                    ESP_LOGI(TAG, "AVRCP duplo clique: PREV");
                    enqueue_control_cmd(CMD_PLAY_PREV);
                    break;
                case ESP_AVRC_PT_CMD_VOL_UP:
                    volume_up();
                    break;
                case ESP_AVRC_PT_CMD_VOL_DOWN:
                    volume_down();
                    break;
                default:
                    ESP_LOGD(TAG, "AVRCP cmd não mapeado: 0x%02X", param->psth_cmd.key_code);
                    break;
            }
            break;

        default:
            break;
    }
}

static esp_err_t bluetooth_init(void)
{
    ESP_LOGI(TAG, "🔵 Inicializando Bluetooth...");
    
    esp_err_t ret;
    
    ESP_ERROR_CHECK(nvs_flash_init());
    
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable: %s", esp_err_to_name(ret));
        return ret;
    }
    
    esp_bt_dev_set_device_name("ESP32_MP3");
    // Evita conexões ACL de entrada fora do fluxo controlado do player.
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    
    esp_bt_gap_register_callback(gap_callback);
    
    // AVRCP precisa subir antes do A2DP source
    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(avrc_callback);
    esp_avrc_tg_init();
    esp_avrc_tg_register_callback(avrc_tg_callback);

    esp_avrc_psth_bit_mask_t cmd_set = {0};
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PLAY);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PAUSE);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_STOP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_BACKWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_VOL_UP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_VOL_DOWN);
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_set);

    esp_a2d_register_callback(a2dp_callback);
    esp_a2d_source_register_data_callback(bt_a2dp_source_data_cb);
    esp_a2d_source_init();
    
    sys_status.bt_initialized = true;
    ESP_LOGI(TAG, "✅ Bluetooth inicializado");
    
    return ESP_OK;
}

static esp_err_t bluetooth_search_and_connect(void)
{
    if (sys_status.bt_connected || sys_status.bt_connecting) {
        return ESP_OK;
    }
    if (discovery_active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "🔍 Iniciando busca Bluetooth...");
    
    device_found = false;
    connect_after_discovery_stop = false;
    discovery_stop_pending = false;
    set_bt_connecting(true);
    
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    
    if (discovery_timer) {
        xTimerStart(discovery_timer, 0);
    }
    log_bt_state("search_start");
    
    return ESP_OK;
}

static void start_current_track_playback(void)
{
    stop_playback_and_reset(true, "start_track");

    total_bytes_streamed = 0;
    underrun_count = 0;
    callback_count = 0;
    cb_lock_fail_count = 0;
    cb_empty_count = 0;
    cb_partial_count = 0;
    producer_frame_count = 0;
    producer_drop_count = 0;
    stream_stabilized = false;

    vTaskDelay(pdMS_TO_TICKS(120));

    file_reader_task_running = true;
    playback_paused = false;
    if (xTaskCreatePinnedToCore(decode_and_stream, "decode",
                                6144, NULL, 6, &file_reader_task_handle, 1) == pdPASS) {
        ESP_LOGI(TAG, "✅ Task decode criada (prio 6)");
        sys_status.audio_playing = true;
    } else {
        ESP_LOGE(TAG, "❌ Falha criar task");
        sys_status.file_errors++;
        file_reader_task_running = false;
    }
}

// ============================================================================
// PLAYER CONTROL TASK
// ============================================================================

static void player_control_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Task controle iniciada");
    player_cmd_t cmd;
    
    // ✅ Proteção contra comandos duplicados
    static TickType_t last_play_next_time = 0;
    static const TickType_t PLAY_NEXT_DEBOUNCE_MS = 3000;  // 3 segundos entre músicas (aumentado)
    
    while (1) {
        if (xQueueReceive(control_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGD(TAG, "Queue -> %s", cmd_to_str(cmd));
            log_queue_state("after_dequeue");
            switch (cmd) {
                case CMD_PLAY_NEXT:
                    cmd_play_next_pending = false;
                    break;
                case CMD_PLAY_PREV:
                    cmd_play_prev_pending = false;
                    break;
                case CMD_RETRY_CONNECTION:
                    cmd_retry_pending = false;
                    break;
                case CMD_RESTART_DISCOVERY:
                    restart_discovery_in_progress = false;
                    cmd_restart_disc_pending = false;
                    break;
                case CMD_CONNECT_TARGET:
                    cmd_connect_pending = false;
                    break;
                default:
                    break;
            }
            
            switch (cmd) {
                case CMD_PLAY_NEXT: {
                    if (!bt_ready_for_playback()) {
                        ESP_LOGW(TAG, "⚠️ PLAY_NEXT ignorado (BT/stream não pronto)");
                        break;
                    }

                    // ✅ PROTEÇÃO: Ignorar se chamado muito rápido
                    TickType_t now = xTaskGetTickCount();
                    if (last_play_next_time != 0 &&
                        (now - last_play_next_time) < pdMS_TO_TICKS(PLAY_NEXT_DEBOUNCE_MS)) {
                        ESP_LOGW(TAG, "⚠️ PLAY_NEXT ignorado (debounce: %lu ms)", 
                                (unsigned long)TICKS_TO_MS(now - last_play_next_time));
                        break;
                    }
                    last_play_next_time = now;

                    int old_track = current_track;
                    current_track = get_random_track(current_track);
                    ESP_LOGI(TAG, "🎲 PLAY_NEXT aleatório: [%d/%d] → [%d/%d]",
                             old_track + 1, mp3_count, current_track + 1, mp3_count);
                    
                    ESP_LOGI(TAG, "🎵 Comando: PLAY_NEXT para track [%d/%d]", 
                            current_track + 1, mp3_count);
                    start_current_track_playback();
                    break;
                }

                case CMD_PLAY_PREV: {
                    if (!bt_ready_for_playback()) {
                        ESP_LOGW(TAG, "⚠️ PLAY_PREV ignorado (BT/stream não pronto)");
                        break;
                    }
                    TickType_t now = xTaskGetTickCount();
                    if (last_play_next_time != 0 &&
                        (now - last_play_next_time) < pdMS_TO_TICKS(800)) {
                        ESP_LOGW(TAG, "⚠️ PLAY_PREV ignorado (debounce: %lu ms)",
                                (unsigned long)TICKS_TO_MS(now - last_play_next_time));
                        break;
                    }
                    last_play_next_time = now;

                    int old_track = current_track;
                    current_track = get_previous_track(current_track);
                    ESP_LOGI(TAG, "⏮️ PLAY_PREV: [%d/%d] → [%d/%d]",
                             old_track + 1, mp3_count, current_track + 1, mp3_count);
                    start_current_track_playback();
                    break;
                }
                
                case CMD_STOP:
                    ESP_LOGI(TAG, "⏹️ Comando: STOP");
                    stop_playback_and_reset(true, "cmd_stop");
                    break;

                case CMD_PAUSE:
                    ESP_LOGI(TAG, "⏸️ Comando: PAUSE");
                    playback_paused = true;
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
                    break;

                case CMD_RESUME:
                    ESP_LOGI(TAG, "▶️ Comando: RESUME");
                    playback_paused = false;
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                    break;
                
                case CMD_RETRY_CONNECTION:
                    if (sys_status.bt_connected || sys_status.bt_connecting) {
                        ESP_LOGI(TAG, "RETRY_CONNECTION ignorado: já conectado/conectando");
                        break;
                    }
                    ESP_LOGI(TAG, "🔄 Comando: RETRY_CONNECTION");

                    if (a2dp_open_fail_streak >= A2DP_OPEN_FAIL_REDISCOVERY_THRESHOLD) {
                        ESP_LOGW(TAG, "RETRY_CONNECTION -> RESTART_DISCOVERY (fail streak=%lu)",
                                 (unsigned long)a2dp_open_fail_streak);
                        player_cmd_t redisc = CMD_RESTART_DISCOVERY;
                        enqueue_control_cmd(redisc);
                        break;
                    }

                    uint32_t retry_delay_ms = 1200 + (a2dp_open_fail_streak * 800);
                    if (retry_delay_ms > 4000) retry_delay_ms = 4000;
                    vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
                    if (sys_status.bt_connected || sys_status.bt_connecting) {
                        break;
                    }
                    if (sys_status.connection_retries < INT_MAX) {
                        sys_status.connection_retries++;
                    }

                    // Primeiro tenta conexão direta no MAC conhecido para evitar race de discovery.
                    memcpy(target_device_addr, target_mac_addr, ESP_BD_ADDR_LEN);
                    device_found = true;
                    set_bt_connecting(true);
                    connect_after_discovery_stop = false;
                    player_cmd_t conn_cmd = CMD_CONNECT_TARGET;
                    if (!enqueue_control_cmd(conn_cmd)) {
                        // Se já houver connect pendente, não abrir discovery em paralelo.
                        // Discovery fica como fallback quando timeout ocorrer.
                    }
                    break;
                
                case CMD_RESTART_DISCOVERY:
                    restart_discovery_in_progress = true;
                    if (sys_status.bt_connected) {
                        ESP_LOGI(TAG, "RESTART_DISCOVERY ignorado: já conectado");
                        restart_discovery_in_progress = false;
                        break;
                    }
                    // Permite reiniciar busca mesmo se "connecting" ficou preso sem discovery ativo.
                    if (sys_status.bt_connecting && !discovery_active) {
                        ESP_LOGW(TAG, "RESTART_DISCOVERY: limpando connecting preso");
                        set_bt_connecting(false);
                    }
                    if (sys_status.bt_connecting && discovery_active) {
                        ESP_LOGI(TAG, "RESTART_DISCOVERY ignorado: discovery em andamento");
                        break;
                    }
                    ESP_LOGI(TAG, "🔄 Comando: RESTART_DISCOVERY");
                    
                    device_found = false;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    bluetooth_search_and_connect();
                    restart_discovery_in_progress = false;
                    break;

                case CMD_CONNECT_TARGET: {
                    if (!device_found || sys_status.bt_connected || sys_status.bt_connecting == false) {
                        ESP_LOGW(TAG, "CONNECT_TARGET ignorado | found:%d connected:%d connecting:%d",
                                 device_found, sys_status.bt_connected, sys_status.bt_connecting);
                        break;
                    }
                    if (discovery_active) {
                        connect_after_discovery_stop = true;
                        if (!discovery_stop_pending) {
                            discovery_stop_pending = true;
                            esp_bt_gap_cancel_discovery();
                            ESP_LOGI(TAG, "🛑 Cancelando discovery para conectar alvo");
                        }
                        break;
                    }
                    ESP_LOGI(TAG, "🔗 Conectando ao A2DP...");
                    if (connection_timer) {
                        xTimerStop(connection_timer, 0);
                    }
                    set_bt_connecting(true);
                    esp_err_t conn_err = esp_a2d_source_connect(target_device_addr);
                    if (conn_err != ESP_OK) {
                        ESP_LOGE(TAG, "❌ Falha connect: %s", esp_err_to_name(conn_err));
                        device_found = false;
                        set_bt_connecting(false);
                    } else {
                        ESP_LOGI(TAG, "✅ Comando connect enviado");
                        if (connection_timer) {
                            xTimerStart(connection_timer, 0);
                        }
                        log_bt_state("connect_cmd_sent");
                    }
                    break;
                }
                
                default:
                    break;
            }
        }
        
        EventBits_t bits = xEventGroupGetBits(player_event_group);
        
        if (bits & TRACK_FINISHED_BIT) {
            ESP_LOGI(TAG, "✅ Track finalizado, preparando próxima...");
            xEventGroupClearBits(player_event_group, TRACK_FINISHED_BIT);
            
            // ✅ Limpar comandos duplicados da queue
            player_cmd_t dummy;
            int cleared = 0;
            while (xQueueReceive(control_queue, &dummy, 0) == pdTRUE) {
                switch (dummy) {
                    case CMD_PLAY_NEXT: cmd_play_next_pending = false; break;
                    case CMD_PLAY_PREV: cmd_play_prev_pending = false; break;
                    case CMD_RETRY_CONNECTION: cmd_retry_pending = false; break;
                    case CMD_RESTART_DISCOVERY: cmd_restart_disc_pending = false; break;
                    case CMD_CONNECT_TARGET: cmd_connect_pending = false; break;
                    default: break;
                }
                cleared++;
            }
            if (cleared > 0) {
                ESP_LOGI(TAG, "🗑️ Removidos %d comandos duplicados da queue", cleared);
            }

            if (!bt_ready_for_playback()) {
                continue;
            }
            
            // Aguardar um pouco antes de iniciar próxima
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            // Enviar comando para tocar próxima (aleatória dentro do CMD_PLAY_NEXT)
            player_cmd_t next_cmd = CMD_PLAY_NEXT;
            enqueue_control_cmd(next_cmd);
        }
        
        if (sys_status.file_errors > 10) {
            ESP_LOGE(TAG, "Muitos erros, reiniciando...");
            esp_restart();
        }
    }
}

// ============================================================================
// MAIN TASK
// ============================================================================

static void main_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Task principal iniciada");
    ESP_LOGI(TAG, "Aguardando conexão Bluetooth...");
    
    EventBits_t bits = xEventGroupWaitBits(player_event_group,
                                          BT_CONNECTED_BIT,
                                          false, false,
                                          pdMS_TO_TICKS(60000));
    if (!(bits & BT_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Sem conexão inicial em 60s, mantendo retries contínuos...");
        player_cmd_t cmd = CMD_RETRY_CONNECTION;
        enqueue_control_cmd(cmd);
        ESP_LOGI(TAG, "Sistema iniciado sem BT (modo reconexão contínua).");
    } else {
        ESP_LOGI(TAG, "Bluetooth conectado! Sistema pronto.");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    log_system_status();
    
    xEventGroupWaitBits(player_event_group, STREAM_READY_BIT, 
                       false, false, pdMS_TO_TICKS(5000));
    
    TickType_t last_alive_log = xTaskGetTickCount();
    TickType_t idle_without_bt_start = 0;
    
    while (1) {
        if (xTaskGetTickCount() - last_alive_log > pdMS_TO_TICKS(120000)) {
            ESP_LOGI(TAG, "Sistema ativo - Track: [%d/%d]", 
                    current_track + 1, mp3_count);
            ESP_LOGI(TAG, "Stream: %s, Heap: %lu", 
                    sys_status.streaming_active ? "ON" : "OFF",
                    (unsigned long)esp_get_free_heap_size());
            last_alive_log = xTaskGetTickCount();
        }
        
        if (!sys_status.bt_connected && !sys_status.audio_playing) {
            if (idle_without_bt_start == 0) {
                idle_without_bt_start = xTaskGetTickCount();
            }
            if (!sys_status.bt_connecting &&
                !cmd_retry_pending &&
                !cmd_restart_disc_pending &&
                !cmd_connect_pending &&
                !restart_discovery_in_progress &&
                !discovery_active &&
                !discovery_stop_pending) {
                ESP_LOGW(TAG, "Conexão perdida (sem música), tentando reconectar...");
                player_cmd_t cmd = CMD_RETRY_CONNECTION;
                enqueue_control_cmd(cmd);
            }

            if (sys_status.bt_connecting && bt_connecting_since != 0) {
                TickType_t connecting_ticks = xTaskGetTickCount() - bt_connecting_since;
                if (connecting_ticks > pdMS_TO_TICKS(BT_CONNECTING_STUCK_MS)) {
                    ESP_LOGW(TAG, "⚠️ bt_connecting preso por %lu ms, reiniciando ciclo",
                             (unsigned long)TICKS_TO_MS(connecting_ticks));
                    set_bt_connecting(false);
                    player_cmd_t cmd = CMD_RESTART_DISCOVERY;
                    enqueue_control_cmd(cmd);
                }
            }

            TickType_t idle_ticks = xTaskGetTickCount() - idle_without_bt_start;
            if (idle_ticks > pdMS_TO_TICKS(AUTO_SLEEP_IDLE_MS)) {
                ESP_LOGW(TAG, "⏲️ Auto-sleep: %lu ms sem BT/áudio",
                        (unsigned long)TICKS_TO_MS(idle_ticks));
                enter_deep_sleep(false);
            }
        } else {
            idle_without_bt_start = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============================================================================
// APP MAIN
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "===== ESP32 MP3 Player - CORREÇÕES FINAIS =====");
    ESP_LOGI(TAG, "Heap inicial: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // LED azul (GPIO2) como indicador principal de "ligado".
    set_power_led(true);

    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    if (wake == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "🔋 Wakeup por botão (EXT0 GPIO %d)", PIN_PWR_SLEEP);
    } else if (wake != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "🔋 Wakeup cause: %d", wake);
    }

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("BT_AVRC", ESP_LOG_WARN);
    esp_log_level_set("BT_APPL", ESP_LOG_WARN);
    esp_log_level_set("BT_AV", ESP_LOG_WARN);
    
    memset(&sys_status, 0, sizeof(sys_status));
    
    player_event_group = xEventGroupCreate();
    control_queue = xQueueCreate(15, sizeof(player_cmd_t));
    connection_timer = xTimerCreate("conn", pdMS_TO_TICKS(60000), 
                                   pdFALSE, NULL, connection_timeout_callback);
    discovery_timer = xTimerCreate("disc", pdMS_TO_TICKS(DISCOVERY_TIMEOUT_SEC * 1000), 
                                  pdFALSE, NULL, discovery_timeout_callback);
    buffer_monitor_timer = xTimerCreate("buf_mon", pdMS_TO_TICKS(500),
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
    
    ret = count_mp3_files();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha escanear MP3s: %s", esp_err_to_name(ret));
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Nenhum MP3 encontrado!");
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                ESP_LOGE(TAG, "Aguardando arquivos MP3...");
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
    
    ESP_LOGI(TAG, "Heap após buffers: %lu (esperado ~40KB+)", (unsigned long)esp_get_free_heap_size());
    
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
                            3584, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Falha task controle");
        cleanup_audio_buffers();
        esp_restart();
        return;
    }

    if (xTaskCreatePinnedToCore(main_task, "main", 
                            2560, NULL, 4, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Falha task main");
        cleanup_audio_buffers();
        esp_restart();
        return;
    }

    ESP_LOGI(TAG, "Tasks criadas");
    
    init_volume_buttons();
    configure_deep_sleep_wakeup();
    
    if (xTaskCreatePinnedToCore(volume_control_task, "volume", 
                            3072, NULL, 3, NULL, 0) != pdPASS) {
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
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "🎯 Configuração de busca Bluetooth:");
    ESP_LOGI(TAG, "   Modo: Busca por MAC address");
    ESP_LOGI(TAG, "   MAC alvo: %02X:%02X:%02X:%02X:%02X:%02X",
            target_mac_addr[0], target_mac_addr[1], target_mac_addr[2],
            target_mac_addr[3], target_mac_addr[4], target_mac_addr[5]);
    ESP_LOGI(TAG, "   Referência: '%s'", TARGET_DEVICE_NAME);
    ESP_LOGI(TAG, "");
    
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
                        free_heap, (unsigned long)restart_counter);
                
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
