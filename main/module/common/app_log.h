#ifndef APP_LOG_H
#define APP_LOG_H

#include "esp_log.h"

// Perfis de log (troque só APP_LOG_PROFILE para produção/dev)
#define LOG_PROFILE_SILENT     0  // só WARN/ERROR
#define LOG_PROFILE_ESSENTIAL  1  // infos essenciais + WARN/ERROR
#define LOG_PROFILE_VERBOSE    2  // tudo (inclui logs periódicos de desempenho)

#ifndef APP_LOG_PROFILE
#define APP_LOG_PROFILE LOG_PROFILE_ESSENTIAL
#endif

#if APP_LOG_PROFILE >= LOG_PROFILE_VERBOSE
#define PERF_LOGI ESP_LOGI
#else
#define PERF_LOGI(tag, format, ...) ((void)0)
#endif

static inline void app_log_apply_levels(const char *tag)
{
    esp_log_level_set("*", ESP_LOG_WARN);
#if APP_LOG_PROFILE == LOG_PROFILE_VERBOSE
    esp_log_level_set(tag, ESP_LOG_DEBUG);
#elif APP_LOG_PROFILE == LOG_PROFILE_ESSENTIAL
    esp_log_level_set(tag, ESP_LOG_INFO);
#else
    esp_log_level_set(tag, ESP_LOG_WARN);
#endif
    esp_log_level_set("BT_AVRC", ESP_LOG_WARN);
    esp_log_level_set("BT_APPL", ESP_LOG_WARN);
    esp_log_level_set("BT_AV", ESP_LOG_WARN);
}

#endif
