#ifndef MEDIA_LIBRARY_H
#define MEDIA_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

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

int media_skip_id3v2(FILE *f, const char *tag);
bool media_drop_trailing_tag_if_present(uint8_t **ptr, int *bytes_left, const char *tag);

esp_err_t media_count_mp3_files(const char *mount_point, int *out_count, const char *tag);
esp_err_t media_get_mp3_path(const char *mount_point, int index, char *path, size_t max_len);
esp_err_t media_analyze_mp3_file(const char *path, mp3_info_t *info, const char *tag);

#endif
