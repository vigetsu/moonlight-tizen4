/* GameStream (Sunshine / GeForce Experience) HTTP API client. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <Limelight.h>

typedef struct {
    char address[256];
    uint16_t http_port;
    uint16_t https_port;
    char hostname[128];
    char app_version[64];
    char gfe_version[64];
    char rtsp_url[512];
    int major_version;
    int codec_support;
    int current_game;
    bool paired;
} gs_server_t;

typedef struct {
    int id;
    char title[256];
    bool hdr;
} gs_app_t;

/* Loads or creates the client identity (certificate + key). */
bool gs_init(void);
void gs_server_setup(gs_server_t *s, const char *address);
bool gs_server_info(gs_server_t *s, char *err, size_t errlen);
/* Blocks until the PIN is entered on the host (or it times out). */
bool gs_pair(gs_server_t *s, const char *pin, char *err, size_t errlen);
bool gs_unpair(gs_server_t *s);
/* Returns a malloc'd app array. */
bool gs_applist(gs_server_t *s, gs_app_t **apps, int *count, char *err, size_t errlen);
/* Launches or resumes appid. Fills the input key/iv in cfg and the RTSP URL in s. */
bool gs_launch(gs_server_t *s, STREAM_CONFIGURATION *cfg, int appid, bool sops, bool host_audio,
               char *err, size_t errlen);
bool gs_quit(gs_server_t *s, char *err, size_t errlen);
