#include "stream.h"
#include "fmp4.h"
#include "json.h"
#include "platform.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus/opus_multistream.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Audio is batched into 20 ms PCM chunks to keep the TV's Web Audio scheduling cheap. */
#define AUDIO_BATCH_MS 20

static mutex_t g_lock;
static bool g_lock_init;
static volatile bool g_active;      /* a stream (real or test) is running */
static volatile bool g_real;        /* a real Moonlight connection is running */
static volatile bool g_test_run;
static volatile bool g_resync;
static int g_width, g_height;

static fmp4_t g_mux;
static uint8_t *g_au;
static size_t g_au_cap;

static OpusMSDecoder *g_opus;
static int g_channels, g_frame_size;
static int16_t *g_pcm;
static int g_pcm_frames, g_pcm_batch;

static void lock_init(void) {
    if (!g_lock_init) {
        mutex_init(&g_lock);
        g_lock_init = true;
    }
}

bool stream_active(void) { return g_active; }

/* ---------- video ---------- */

static void send_video_info(void) {
    char msg[256];
    snprintf(msg, sizeof(msg), "{\"t\":\"video\",\"codec\":\"%s\",\"w\":%d,\"h\":%d,\"fps\":%d}",
             g_mux.codec, g_width, g_height, g_mux.fps);
    client_send_text(msg);
}

/* Muxes one Annex-B access unit and sends it. Returns false if an IDR is needed. */
static bool push_access_unit(const uint8_t *au, size_t len, bool keyframe) {
    if (g_resync) {
        if (!keyframe) return false;
        g_resync = false;
        /* Forget parameter sets so a fresh init segment is produced. */
        g_mux.sps_len = g_mux.pps_len = 0;
        g_mux.have_params = false;
    }
    bytebuf_t init = {0}, frag = {0};
    bool ok = fmp4_mux_annexb(&g_mux, au, len, keyframe, &init, &frag);
    if (init.len) {
        send_video_info();
        client_send_bin(MSG_VIDEO_INIT, init.data, init.len);
    }
    if (ok) client_send_bin(MSG_VIDEO_FRAG, frag.data, frag.len);
    bb_free(&init);
    bb_free(&frag);
    return ok || g_mux.have_params;
}

static int dr_setup(int fmt, int w, int h, int fps, void *ctx, int flags) {
    (void)ctx; (void)flags;
    if (!(fmt & VIDEO_FORMAT_MASK_H264)) {
        fprintf(stderr, "[video] unsupported format 0x%x\n", fmt);
        return -1;
    }
    g_width = w;
    g_height = h;
    fmp4_init(&g_mux, w, h, fps);
    g_resync = false;
    return 0;
}

static int dr_submit(PDECODE_UNIT du) {
    if ((size_t)du->fullLength > g_au_cap) {
        size_t cap = (size_t)du->fullLength * 2;
        uint8_t *n = realloc(g_au, cap);
        if (!n) return DR_NEED_IDR;
        g_au = n;
        g_au_cap = cap;
    }
    size_t o = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next) {
        memcpy(g_au + o, e->data, (size_t)e->length);
        o += (size_t)e->length;
    }
    return push_access_unit(g_au, o, du->frameType == FRAME_TYPE_IDR) ? DR_OK : DR_NEED_IDR;
}

/* ---------- audio ---------- */

static void pcm_reset(int channels, int sample_rate) {
    free(g_pcm);
    g_channels = channels;
    g_pcm_batch = sample_rate * AUDIO_BATCH_MS / 1000;
    g_pcm = calloc((size_t)(g_pcm_batch + 2048) * (size_t)channels, sizeof(int16_t));
    g_pcm_frames = 0;
}

static void pcm_push(int frames) {
    g_pcm_frames += frames;
    if (g_pcm_frames >= g_pcm_batch) {
        client_send_bin(MSG_AUDIO_PCM, g_pcm, (size_t)g_pcm_frames * (size_t)g_channels * sizeof(int16_t));
        g_pcm_frames = 0;
    }
}

static void send_audio_info(int rate, int channels) {
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"t\":\"audio\",\"rate\":%d,\"channels\":%d}", rate, channels);
    client_send_text(msg);
}

static int ar_init(int config, const POPUS_MULTISTREAM_CONFIGURATION oc, void *ctx, int flags) {
    (void)config; (void)ctx; (void)flags;
    int err;
    g_opus = opus_multistream_decoder_create(oc->sampleRate, oc->channelCount, oc->streams,
                                             oc->coupledStreams, oc->mapping, &err);
    if (!g_opus) return -1;
    g_frame_size = oc->samplesPerFrame;
    pcm_reset(oc->channelCount, oc->sampleRate);
    send_audio_info(oc->sampleRate, oc->channelCount);
    return 0;
}

static void ar_cleanup(void) {
    if (g_opus) opus_multistream_decoder_destroy(g_opus);
    g_opus = NULL;
}

static void ar_decode(char *data, int len) {
    if (!g_opus || !g_pcm) return;
    int n = opus_multistream_decode(g_opus, (const unsigned char *)data, data ? len : 0,
                                    g_pcm + (size_t)g_pcm_frames * (size_t)g_channels, g_frame_size, 0);
    if (n > 0) pcm_push(n);
}

/* ---------- connection listener ---------- */

static void cl_stage_starting(int stage) {
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"t\":\"stage\",\"name\":\"%s\"}", LiGetStageName(stage));
    client_send_text(msg);
}

static void cl_stage_failed(int stage, int code) {
    char msg[256], ports[128];
    unsigned flags = LiGetPortFlagsFromStage(stage);
    ports[0] = 0;
    if (flags) LiStringifyPortFlags(flags, ", ", ports, sizeof(ports));
    snprintf(msg, sizeof(msg), "{\"t\":\"error\",\"msg\":\"%s aşaması başarısız (hata %d)%s%s\"}",
             LiGetStageName(stage), code, ports[0] ? ". Kontrol edilecek portlar: " : "", ports);
    client_send_text(msg);
}

static void cl_started(void) {
    client_send_text("{\"t\":\"started\"}");
}

static void stop_thread(void *arg) {
    (void)arg;
    stream_stop();
}

static void cl_terminated(int code) {
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"t\":\"ended\",\"code\":%d}", code);
    client_send_text(msg);
    /* LiStopConnection must not run on a library callback thread. */
    thread_spawn(stop_thread, NULL);
}

static void cl_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[moonlight] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void cl_rumble(unsigned short n, unsigned short lo, unsigned short hi) {
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"t\":\"rumble\",\"i\":%u,\"lo\":%u,\"hi\":%u}", n, lo, hi);
    client_send_text(msg);
}

static void cl_status(int status) {
    client_send_text(status == CONN_STATUS_POOR ? "{\"t\":\"net\",\"poor\":true}"
                                                : "{\"t\":\"net\",\"poor\":false}");
}

/* ---------- start / stop ---------- */

bool stream_start(gs_server_t *server, const stream_params_t *p, char *err, size_t errlen) {
    lock_init();
    mutex_lock(&g_lock);
    if (g_active) {
        mutex_unlock(&g_lock);
        snprintf(err, errlen, "Zaten bir yayın çalışıyor");
        return false;
    }
    g_active = true;
    mutex_unlock(&g_lock);

    STREAM_CONFIGURATION cfg;
    LiInitializeStreamConfiguration(&cfg);
    cfg.width = p->width;
    cfg.height = p->height;
    cfg.fps = p->fps;
    cfg.bitrate = p->bitrate_kbps;
    cfg.packetSize = 1392;
    cfg.streamingRemotely = STREAM_CFG_AUTO;
    cfg.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    cfg.supportedVideoFormats = VIDEO_FORMAT_H264;
    cfg.clientRefreshRateX100 = p->fps * 100;
    cfg.colorSpace = COLORSPACE_REC_709;
    cfg.colorRange = COLOR_RANGE_LIMITED;
    cfg.encryptionFlags = ENCFLG_AUDIO;

    if (!gs_launch(server, &cfg, p->appid, p->sops, p->host_audio, err, errlen)) {
        g_active = false;
        return false;
    }

    SERVER_INFORMATION si;
    LiInitializeServerInformation(&si);
    si.address = server->address;
    si.serverInfoAppVersion = server->app_version;
    si.serverInfoGfeVersion = server->gfe_version;
    si.rtspSessionUrl = server->rtsp_url[0] ? server->rtsp_url : NULL;
    si.serverCodecModeSupport = server->codec_support;

    DECODER_RENDERER_CALLBACKS dr;
    LiInitializeVideoCallbacks(&dr);
    dr.setup = dr_setup;
    dr.submitDecodeUnit = dr_submit;
    dr.capabilities = CAPABILITY_DIRECT_SUBMIT;

    AUDIO_RENDERER_CALLBACKS ar;
    LiInitializeAudioCallbacks(&ar);
    ar.init = ar_init;
    ar.cleanup = ar_cleanup;
    ar.decodeAndPlaySample = ar_decode;
    ar.capabilities = CAPABILITY_DIRECT_SUBMIT;

    CONNECTION_LISTENER_CALLBACKS cl;
    LiInitializeConnectionCallbacks(&cl);
    cl.stageStarting = cl_stage_starting;
    cl.stageFailed = cl_stage_failed;
    cl.connectionStarted = cl_started;
    cl.connectionTerminated = cl_terminated;
    cl.logMessage = cl_log;
    cl.rumble = cl_rumble;
    cl.connectionStatusUpdate = cl_status;

    if (LiStartConnection(&si, &cfg, &cl, &dr, &ar, NULL, 0, NULL, 0) != 0) {
        snprintf(err, errlen, "Moonlight bağlantısı kurulamadı");
        g_active = false;
        return false;
    }
    g_real = true;
    return true;
}

void stream_stop(void) {
    lock_init();
    mutex_lock(&g_lock);
    if (g_real) {
        g_real = false;
        LiStopConnection();
    }
    if (g_test_run) {
        g_test_run = false;
        mutex_unlock(&g_lock);
        sleep_ms(200); /* let the test thread notice */
        mutex_lock(&g_lock);
    }
    if (g_active) client_send_text("{\"t\":\"stopped\"}");
    g_active = false;
    mutex_unlock(&g_lock);
}

void stream_resync(void) {
    if (!g_active) return;
    g_resync = true;
    if (g_real) LiRequestIdrFrame();
}

/* ---------- test pattern mode ---------- */

typedef struct {
    uint8_t *data;
    size_t len;
    int fps;
} test_ctx_t;

/* Splits on access unit delimiters (NAL type 9). Returns end of AU starting at p. */
static const uint8_t *next_au(const uint8_t *p, const uint8_t *end) {
    const uint8_t *q = p + 4;
    for (; q + 4 < end; q++) {
        if (q[0] == 0 && q[1] == 0 && q[2] == 1 && (q[3] & 0x1f) == 9) {
            return (q > p && q[-1] == 0) ? q - 1 : q;
        }
    }
    return end;
}

static bool au_is_idr(const uint8_t *p, const uint8_t *end) {
    for (; p + 3 < end; p++)
        if (p[0] == 0 && p[1] == 0 && p[2] == 1 && (p[3] & 0x1f) == 5) return true;
    return false;
}

static void test_thread(void *arg) {
    test_ctx_t *t = arg;
    const uint8_t *end = t->data + t->len;
    const uint8_t *p = t->data;
    uint64_t frame_us = 1000000ULL / (uint64_t)t->fps;
    uint64_t next = LiGetMicroseconds();
    double phase = 0;
    int rate = 48000, per_frame = rate / t->fps;
    int16_t *tone = malloc((size_t)per_frame * 2 * sizeof(int16_t));

    send_audio_info(rate, 2);
    pcm_reset(2, rate);
    client_send_text("{\"t\":\"started\"}");

    while (g_test_run) {
        const uint8_t *e = next_au(p, end);
        bool idr = au_is_idr(p, e);
        if (!push_access_unit(p, (size_t)(e - p), idr)) {
            /* Resync requested: jump back to the start (an IDR). */
            p = t->data;
            continue;
        }
        p = e >= end ? t->data : e;

        /* 440 Hz beep, on for the first quarter of every second. */
        for (int i = 0; i < per_frame; i++) {
            bool on = fmod(phase / rate, 1.0) < 0.25;
            int16_t v = on ? (int16_t)(6000 * sin(2 * M_PI * 440 * phase / rate)) : 0;
            tone[i * 2] = tone[i * 2 + 1] = v;
            phase++;
        }
        memcpy(g_pcm + (size_t)g_pcm_frames * 2, tone, (size_t)per_frame * 2 * sizeof(int16_t));
        pcm_push(per_frame);

        next += frame_us;
        uint64_t now = LiGetMicroseconds();
        if (next > now) sleep_ms((int)((next - now) / 1000));
        else if (now - next > 500000) next = now;
    }
    free(tone);
    free(t->data);
    free(t);
}

bool stream_start_test(const char *path, const stream_params_t *p, char *err, size_t errlen) {
    lock_init();
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "Test dosyası açılamadı: %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    test_ctx_t *t = calloc(1, sizeof(*t));
    t->data = malloc((size_t)n);
    t->len = fread(t->data, 1, (size_t)n, f);
    fclose(f);
    t->fps = p->fps > 0 ? p->fps : 60;

    mutex_lock(&g_lock);
    if (g_active) {
        mutex_unlock(&g_lock);
        free(t->data);
        free(t);
        snprintf(err, errlen, "Zaten bir yayın çalışıyor");
        return false;
    }
    g_active = true;
    g_test_run = true;
    mutex_unlock(&g_lock);

    g_width = p->width;
    g_height = p->height;
    fmp4_init(&g_mux, p->width, p->height, t->fps);
    g_resync = false;
    if (!thread_spawn(test_thread, t)) {
        g_active = g_test_run = false;
        snprintf(err, errlen, "İş parçacığı başlatılamadı");
        return false;
    }
    return true;
}

/* ---------- input ---------- */

void stream_input(const char *m) {
    if (!g_real) return;
    char t[8];
    if (!json_get_str(m, "t", t, sizeof(t))) return;

    if (strcmp(t, "k") == 0) {
        /* Windows virtual key code; the 0x80 prefix marks it as a VK for the host. */
        short vk = (short)json_get_int(m, "c", 0);
        LiSendKeyboardEvent((short)(0x8000 | vk), json_get_int(m, "d", 0) ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                            (char)json_get_int(m, "m", 0));
    } else if (strcmp(t, "mm") == 0) {
        LiSendMouseMoveEvent((short)json_get_int(m, "x", 0), (short)json_get_int(m, "y", 0));
    } else if (strcmp(t, "mp") == 0) {
        LiSendMousePositionEvent((short)json_get_int(m, "x", 0), (short)json_get_int(m, "y", 0),
                                 (short)json_get_int(m, "w", g_width), (short)json_get_int(m, "h", g_height));
    } else if (strcmp(t, "mb") == 0) {
        LiSendMouseButtonEvent(json_get_int(m, "d", 0) ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
                               (int)json_get_int(m, "b", BUTTON_LEFT));
    } else if (strcmp(t, "ms") == 0) {
        LiSendHighResScrollEvent((short)json_get_int(m, "v", 0));
    } else if (strcmp(t, "g") == 0) {
        LiSendMultiControllerEvent((uint8_t)json_get_int(m, "i", 0), (uint16_t)json_get_int(m, "mask", 1),
                                   (int)json_get_int(m, "b", 0), (unsigned char)json_get_int(m, "lt", 0),
                                   (unsigned char)json_get_int(m, "rt", 0), (short)json_get_int(m, "lx", 0),
                                   (short)json_get_int(m, "ly", 0), (short)json_get_int(m, "rx", 0),
                                   (short)json_get_int(m, "ry", 0));
    } else if (strcmp(t, "ga") == 0) {
        LiSendControllerArrivalEvent((uint8_t)json_get_int(m, "i", 0), (uint16_t)json_get_int(m, "mask", 1),
                                     LI_CTYPE_XBOX, 0xFFFFFFFF, LI_CCAP_ANALOG_TRIGGERS);
    } else if (strcmp(t, "txt") == 0) {
        char s[256];
        if (json_get_str(m, "s", s, sizeof(s))) LiSendUtf8TextEvent(s, (unsigned)strlen(s));
    }
}
