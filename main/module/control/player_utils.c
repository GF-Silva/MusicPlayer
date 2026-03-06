#include "player_utils.h"

#include "esp_random.h"

bool player_bt_ready_for_playback(bool bt_connected, bool streaming_active)
{
    (void)streaming_active;
    return bt_connected;
}

void player_set_bt_connecting(bool *bt_connecting,
                              TickType_t *bt_connecting_since,
                              bool connecting)
{
    if (!bt_connecting || !bt_connecting_since) {
        return;
    }

    if (connecting) {
        if (!*bt_connecting) {
            *bt_connecting_since = xTaskGetTickCount();
        }
    } else {
        *bt_connecting_since = 0;
    }

    *bt_connecting = connecting;
}

int player_get_random_track(int current_track, int total_tracks)
{
    if (total_tracks <= 1) {
        return 0;
    }

    int new_track = current_track;
    int attempts = 0;

    do {
        new_track = (int)(esp_random() % (uint32_t)total_tracks);
        attempts++;
    } while (new_track == current_track && attempts < 10);

    return new_track;
}

int player_get_previous_track(int current_track, int total_tracks)
{
    if (total_tracks <= 1) {
        return 0;
    }
    return (current_track == 0) ? (total_tracks - 1) : (current_track - 1);
}
