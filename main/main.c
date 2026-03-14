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
#include "app_config.h"
#include "app_log.h"
#include "ring_buffer.h"
#include "media_library.h"
#include "power_manager.h"
#include "control_queue.h"
#include "input_manager.h"
#include "bt_manager.h"
#include "player_utils.h"
#include "sdcard_manager.h"
#include "sleep_manager.h"
#include "audio_pipeline.h"
#include "bt_callbacks.h"
#include "playback_engine.h"
#include "a2dp_stream.h"
#include "player_tasks.h"
#include "app_facade.h"
#include "app_bootstrap.h"

#define TICKS_TO_MS(t) ((uint32_t)(t) * (uint32_t)portTICK_PERIOD_MS)

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

static uint8_t current_volume = APP_CONFIG_DEFAULT_VOLUME;
static float volume_scale = APP_CONFIG_DEFAULT_VOLUME / 100.0f;

static esp_bd_addr_t target_device_addr;
static esp_bd_addr_t target_mac_addr = APP_CONFIG_TARGET_DEVICE_MAC;
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
static bt_manager_t s_bt_manager = {0};
static sleep_manager_ctx_t s_sleep_manager = {0};
static app_facade_ctx_t s_app_facade = {0};
static audio_pipeline_ctx_t s_audio_pipeline = {0};
static bt_callbacks_ctx_t s_bt_callbacks = {0};
static playback_engine_ctx_t s_playback_engine = {0};
static a2dp_stream_ctx_t s_a2dp_stream = {0};
static player_tasks_ctx_t s_player_tasks = {0};

static EventGroupHandle_t player_event_group;
#define BT_CONNECTED_BIT    BIT0
#define TRACK_FINISHED_BIT  BIT1
#define PLAY_NEXT_BIT       BIT2
#define ERROR_BIT           BIT3
#define CODEC_READY_BIT     BIT4
#define STREAM_READY_BIT    BIT5

static bool discovery_stop_pending = false;
static volatile bool restart_discovery_in_progress = false;


static TimerHandle_t connection_timer;
static TimerHandle_t discovery_timer;
static TimerHandle_t buffer_monitor_timer;

// ============================================================================
// APP MAIN
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "===== ESP32 MP3 Player - CORREÇÕES FINAIS =====");
    ESP_LOGI(TAG, "Heap inicial: %lu bytes", (unsigned long)esp_get_free_heap_size());

    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    if (wake == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "🔋 Wakeup por botão (EXT0 GPIO %d)", APP_CONFIG_POWER_WAKE_PIN);
        if (!pm_require_hold_low((gpio_num_t)APP_CONFIG_POWER_WAKE_PIN, APP_CONFIG_POWER_HOLD_MS, 20, TAG)) {
            ESP_LOGI(TAG, "Wake curto: retornando para deep sleep");
            pm_hold_led_off_during_sleep((gpio_num_t)APP_CONFIG_BOARD_LED_GPIO, APP_CONFIG_BOARD_LED_ACTIVE_HIGH);
            pm_configure_ext0_wakeup((gpio_num_t)APP_CONFIG_POWER_WAKE_PIN, 0, TAG);
            vTaskDelay(pdMS_TO_TICKS(40));
            esp_deep_sleep_start();
        }
    } else if (wake != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "🔋 Wakeup cause: %d", wake);
    }

    // LED azul (GPIO2) como indicador principal de "ligado".
    pm_set_power_led((gpio_num_t)APP_CONFIG_BOARD_LED_GPIO, APP_CONFIG_BOARD_LED_ACTIVE_HIGH, true);

    app_log_apply_levels(TAG);

    esp_err_t ret = app_bootstrap_init(&(app_bootstrap_ctx_t){
        .tag = TAG,
        .player_event_group = &player_event_group,
        .connection_timer = &connection_timer,
        .discovery_timer = &discovery_timer,
        .buffer_monitor_timer = &buffer_monitor_timer,
        .discovery_timeout_sec = APP_CONFIG_DISCOVERY_TIMEOUT_SEC,
        .pwr_release_wait_ms = APP_CONFIG_PWR_RELEASE_WAIT_MS,
        .bt_connecting_stuck_ms = APP_CONFIG_BT_CONNECTING_STUCK_MS,
        .auto_sleep_idle_ms = APP_CONFIG_AUTO_SLEEP_IDLE_MS,
        .a2dp_open_fail_rediscovery_threshold = APP_CONFIG_A2DP_OPEN_FAIL_REDISCOVERY_THRESHOLD,
        .decode_stall_recovery_ms = APP_CONFIG_DECODE_STALL_RECOVERY_MS,
        .control_queue_len = 15,
        .sd_mounted = &sys_status.sd_mounted,
        .bt_initialized = &sys_status.bt_initialized,
        .bt_connected = &sys_status.bt_connected,
        .bt_connecting = &sys_status.bt_connecting,
        .audio_playing = &sys_status.audio_playing,
        .codec_configured = &sys_status.codec_configured,
        .streaming_active = &sys_status.streaming_active,
        .connection_retries = &sys_status.connection_retries,
        .file_errors = &sys_status.file_errors,
        .current_volume = &current_volume,
        .volume_scale = &volume_scale,
        .current_track = &current_track,
        .mp3_count = &mp3_count,
        .device_found = &device_found,
        .discovery_active = &discovery_active,
        .connect_after_discovery_stop = &connect_after_discovery_stop,
        .discovery_stop_pending = &discovery_stop_pending,
        .restart_discovery_in_progress = &restart_discovery_in_progress,
        .bt_connecting_since = &bt_connecting_since,
        .a2dp_open_fail_streak = &a2dp_open_fail_streak,
        .target_device_addr = &target_device_addr,
        .target_mac_addr = &target_mac_addr,
        .current_file = &current_file,
        .current_mp3_info = &current_mp3_info,
        .file_reader_task_handle = &file_reader_task_handle,
        .file_reader_task_running = &file_reader_task_running,
        .mp3_decoder = &mp3_decoder,
        .mp3_input_buffer = &mp3_input_buffer,
        .pcm_output_buffer = &pcm_output_buffer,
        .bytes_left_in_mp3 = &bytes_left_in_mp3,
        .read_ptr = &read_ptr,
        .stream_buffer = &stream_buffer,
        .total_bytes_streamed = &total_bytes_streamed,
        .underrun_count = &underrun_count,
        .callback_count = &callback_count,
        .cb_lock_fail_count = &cb_lock_fail_count,
        .cb_empty_count = &cb_empty_count,
        .cb_partial_count = &cb_partial_count,
        .producer_frame_count = &producer_frame_count,
        .producer_drop_count = &producer_drop_count,
        .stream_stabilized = &stream_stabilized,
        .last_producer_tick = &last_producer_tick,
        .buffer_low_events = &buffer_low_events,
        .buffer_high_events = &buffer_high_events,
        .playback_paused = &playback_paused,
        .stream_buffer_size = APP_CONFIG_STREAM_BUFFER_SIZE,
        .stream_low_bytes = APP_CONFIG_STREAM_LOW_BYTES,
        .stream_high_bytes = APP_CONFIG_STREAM_HIGH_BYTES,
        .mp3_input_buffer_size = APP_CONFIG_MP3_INPUT_BUFFER_SIZE,
        .pcm_output_buffer_size = APP_CONFIG_PCM_OUTPUT_BUFFER_SIZE,
        .prebuffer_frames = APP_CONFIG_PREBUFFER_FRAMES,
        .mp3_critical_bytes = APP_CONFIG_MP3_CRITICAL_BYTES,
        .mp3_read_min = APP_CONFIG_MP3_READ_MIN,
        .mp3_read_max = APP_CONFIG_MP3_READ_MAX,
        .mp3_no_sync_drop_bytes = APP_CONFIG_MP3_NO_SYNC_DROP_BYTES,
        .power_pin = APP_CONFIG_POWER_WAKE_PIN,
        .board_led_gpio = APP_CONFIG_BOARD_LED_GPIO,
        .board_led_active_high = APP_CONFIG_BOARD_LED_ACTIVE_HIGH,
        .mount_point = APP_CONFIG_MOUNT_POINT,
        .max_path_len = 256,
        .bt_connected_bit = BT_CONNECTED_BIT,
        .track_finished_bit = TRACK_FINISHED_BIT,
        .stream_ready_bit = STREAM_READY_BIT,
        .codec_ready_bit = CODEC_READY_BIT,
        .sleep_manager = &s_sleep_manager,
        .app_facade = &s_app_facade,
        .bt_manager = &s_bt_manager,
        .audio_pipeline = &s_audio_pipeline,
        .bt_callbacks = &s_bt_callbacks,
        .playback_engine = &s_playback_engine,
        .a2dp_stream = &s_a2dp_stream,
        .player_tasks = &s_player_tasks,
    });
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha bootstrap: %s", esp_err_to_name(ret));
        esp_restart();
        return;
    }
    
    ESP_LOGI(TAG, "Estruturas criadas");
    
    ret = sdcard_manager_mount_sdspi(TAG,
                                     APP_CONFIG_SD_PIN_MOSI,
                                     APP_CONFIG_SD_PIN_MISO,
                                     APP_CONFIG_SD_PIN_CLK,
                                     APP_CONFIG_SD_PIN_CS,
                                     APP_CONFIG_MOUNT_POINT,
                                     &sys_status.sd_mounted);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha SD Card: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    
    ret = media_count_mp3_files(APP_CONFIG_MOUNT_POINT, &mp3_count, TAG);
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
    
    ret = app_facade_init_audio_buffers(APP_CONFIG_MP3_INPUT_BUFFER_SIZE,
                                        APP_CONFIG_PCM_OUTPUT_BUFFER_SIZE,
                                        APP_CONFIG_STREAM_BUFFER_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha buffers: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    
    ESP_LOGI(TAG, "Heap após buffers: %lu (esperado ~40KB+)", (unsigned long)esp_get_free_heap_size());
    
    ret = app_facade_bluetooth_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha Bluetooth: %s", esp_err_to_name(ret));
        app_facade_cleanup_audio_buffers();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    
    ESP_LOGI(TAG, "Heap após BT: %lu", (unsigned long)esp_get_free_heap_size());
    
    if (xTaskCreatePinnedToCore(player_tasks_control_task, "ctrl", 
                            3584, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Falha task controle");
        app_facade_cleanup_audio_buffers();
        esp_restart();
        return;
    }

    if (xTaskCreatePinnedToCore(player_tasks_main_task, "main", 
                            2560, NULL, 4, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Falha task main");
        app_facade_cleanup_audio_buffers();
        esp_restart();
        return;
    }

    ESP_LOGI(TAG, "Tasks criadas");

    input_manager_cfg_t input_cfg = {
        .pin_power = APP_CONFIG_POWER_WAKE_PIN,
        .led_gpio = APP_CONFIG_BOARD_LED_GPIO,
        .led_active_high = APP_CONFIG_BOARD_LED_ACTIVE_HIGH,
        .debounce_ms = APP_CONFIG_DEBOUNCE_MS,
        .click_timeout_ms = 500,
        .double_click_interval_ms = APP_CONFIG_DOUBLE_CLICK_INTERVAL_MS,
        .long_click_threshold_ms = APP_CONFIG_LONG_CLICK_THRESHOLD_MS,
        .power_hold_ms = APP_CONFIG_POWER_HOLD_MS,
        .volume_step = APP_CONFIG_VOLUME_STEP,
        .volume_percent = &current_volume,
        .volume_scale = &volume_scale,
        .log_tag = TAG,
        .on_power_hold = app_facade_input_on_power_hold,
    };
    if (input_manager_init(&input_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Falha init input manager");
        app_facade_cleanup_audio_buffers();
        esp_restart();
        return;
    }

    pm_configure_ext0_wakeup((gpio_num_t)APP_CONFIG_POWER_WAKE_PIN, 0, TAG);
    
    if (xTaskCreatePinnedToCore(input_manager_task, "volume", 
                            3072, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Falha task volume");
        app_facade_cleanup_audio_buffers();
        esp_restart();
        return;
    }
    
    app_facade_log_system_status();
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    xTimerStart(buffer_monitor_timer, 0);
    
    ESP_LOGI(TAG, "Iniciando busca BT: %s", APP_CONFIG_TARGET_DEVICE_NAME);
    ret = app_facade_bluetooth_search_and_connect();
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
    ESP_LOGI(TAG, "   Referência: '%s'", APP_CONFIG_TARGET_DEVICE_NAME);
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
                    app_facade_cleanup_audio_buffers();
                    esp_restart();
                }
            } else {
                restart_counter = 0;
            }
            
            last_check = xTaskGetTickCount();
        }
    }
}
