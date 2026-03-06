#ifndef SDCARD_MANAGER_H
#define SDCARD_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t sdcard_manager_mount_sdspi(const char *tag,
                                     int pin_mosi,
                                     int pin_miso,
                                     int pin_clk,
                                     int pin_cs,
                                     const char *mount_point,
                                     bool *sd_mounted);

#endif
