#ifndef PLAYER_UTILS_H
#define PLAYER_UTILS_H

#include <stdbool.h>

#include "freertos/FreeRTOS.h"

bool player_bt_ready_for_playback(bool bt_connected, bool streaming_active);
void player_set_bt_connecting(bool *bt_connecting,
                              TickType_t *bt_connecting_since,
                              bool connecting);

int player_get_random_track(int current_track, int total_tracks);
int player_get_previous_track(int current_track, int total_tracks);

#endif
