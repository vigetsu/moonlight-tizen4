#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "gamestream.h"

/* Binary message types sent to the TV (first byte of each binary frame). */
#define MSG_VIDEO_INIT  0x01  /* fMP4 init segment */
#define MSG_VIDEO_FRAG  0x02  /* fMP4 media fragment (one frame) */
#define MSG_AUDIO_PCM   0x03  /* s16le interleaved PCM */

/* Implemented by main.c: deliver to the currently connected TV, if any. */
void client_send_text(const char *json);
void client_send_bin(int type, const void *data, size_t len);

typedef struct {
    int appid;
    int width, height, fps;
    int bitrate_kbps;
    bool sops;
    bool host_audio;
} stream_params_t;

/* Blocking: launches the app and starts the Moonlight connection. */
bool stream_start(gs_server_t *server, const stream_params_t *p, char *err, size_t errlen);
/* Test mode: streams an Annex-B H.264 file in a loop plus a test tone. */
bool stream_start_test(const char *h264_path, const stream_params_t *p, char *err, size_t errlen);
void stream_stop(void);
bool stream_active(void);
/* Forces a new init segment and requests an IDR frame. */
void stream_resync(void);
/* Handles an input JSON message from the TV. */
void stream_input(const char *json);
