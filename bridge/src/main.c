/*
 * moonlight-tizen4 bridge
 *
 * Talks the Moonlight/GameStream protocol to Sunshine (or GeForce Experience)
 * and relays the stream to the Tizen 4.0 TV app over a WebSocket:
 *   video: H.264 repackaged as fragmented MP4 (no re-encoding)
 *   audio: Opus decoded to 16-bit PCM
 *   input: JSON messages from the TV turned into Moonlight input events
 */
#include "gamestream.h"
#include "json.h"
#include "platform.h"
#include "stream.h"
#include "ws.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/rand.h>

#define BRIDGE_VERSION "0.1.0"

static mutex_t g_client_lock;
static ws_conn_t *g_client;

static gs_server_t g_server;
static const char *g_test_file;
static mutex_t g_job_lock; /* jobs run one at a time */

/* ---------- sending to the TV ---------- */

void client_send_text(const char *json) {
    mutex_lock(&g_client_lock);
    if (g_client) ws_send_text(g_client, json);
    mutex_unlock(&g_client_lock);
}

void client_send_bin(int type, const void *data, size_t len) {
    mutex_lock(&g_client_lock);
    if (g_client) ws_send(g_client, WS_BINARY, type, data, len);
    mutex_unlock(&g_client_lock);
}

static void send_error(const char *msg) {
    char out[1024], esc[900];
    json_escape(esc, sizeof(esc), msg);
    snprintf(out, sizeof(out), "{\"t\":\"error\",\"msg\":%s}", esc);
    client_send_text(out);
}

static void send_host_state(bool reachable) {
    char out[1024], name[300], ver[100];
    json_escape(name, sizeof(name), g_test_file ? "Test Pattern" : g_server.hostname);
    json_escape(ver, sizeof(ver), g_server.app_version);
    snprintf(out, sizeof(out),
             "{\"t\":\"host\",\"reachable\":%s,\"name\":%s,\"paired\":%s,\"currentGame\":%d,"
             "\"version\":%s,\"bridge\":\"" BRIDGE_VERSION "\",\"streaming\":%s,\"test\":%s}",
             reachable ? "true" : "false", name, (g_test_file || g_server.paired) ? "true" : "false",
             g_server.current_game, ver, stream_active() ? "true" : "false", g_test_file ? "true" : "false");
    client_send_text(out);
}

/* ---------- long-running jobs (run off the receive thread) ---------- */

typedef struct {
    char cmd[16];
    char pin[8];
    stream_params_t sp;
} job_t;

static void job_thread(void *arg) {
    job_t *j = arg;
    char err[512] = "";
    mutex_lock(&g_job_lock);

    if (strcmp(j->cmd, "hello") == 0) {
        bool ok = g_test_file || gs_server_info(&g_server, err, sizeof(err));
        send_host_state(ok);
        if (!ok) send_error(err);
    } else if (strcmp(j->cmd, "pair") == 0) {
        bool ok = g_test_file || gs_pair(&g_server, j->pin, err, sizeof(err));
        char out[700], esc[600];
        json_escape(esc, sizeof(esc), err);
        snprintf(out, sizeof(out), "{\"t\":\"paired\",\"ok\":%s,\"msg\":%s}", ok ? "true" : "false", esc);
        client_send_text(out);
        if (ok && !g_test_file) gs_server_info(&g_server, err, sizeof(err));
        send_host_state(true);
    } else if (strcmp(j->cmd, "apps") == 0) {
        gs_app_t *apps = NULL;
        int n = 0;
        if (g_test_file) {
            apps = calloc(1, sizeof(gs_app_t));
            apps[0].id = 1;
            snprintf(apps[0].title, sizeof(apps[0].title), "Test Pattern");
            n = 1;
        } else if (!gs_applist(&g_server, &apps, &n, err, sizeof(err))) {
            send_error(err);
        }
        if (apps) {
            size_t cap = 64 + (size_t)n * 600;
            char *out = malloc(cap);
            size_t o = (size_t)snprintf(out, cap, "{\"t\":\"apps\",\"current\":%d,\"apps\":[", g_server.current_game);
            for (int i = 0; i < n; i++) {
                char title[560];
                json_escape(title, sizeof(title), apps[i].title);
                o += (size_t)snprintf(out + o, cap - o, "%s{\"id\":%d,\"title\":%s}", i ? "," : "", apps[i].id, title);
            }
            snprintf(out + o, cap - o, "]}");
            client_send_text(out);
            free(out);
            free(apps);
        }
    } else if (strcmp(j->cmd, "start") == 0) {
        bool ok;
        if (g_test_file) {
            ok = stream_start_test(g_test_file, &j->sp, err, sizeof(err));
        } else {
            ok = gs_server_info(&g_server, err, sizeof(err)) && stream_start(&g_server, &j->sp, err, sizeof(err));
        }
        if (!ok) send_error(err);
    } else if (strcmp(j->cmd, "quit") == 0) {
        stream_stop();
        if (!g_test_file && !gs_quit(&g_server, err, sizeof(err))) send_error(err);
        send_host_state(true);
    }

    mutex_unlock(&g_job_lock);
    free(j);
}

static void run_job(job_t *j) {
    if (!thread_spawn(job_thread, j)) free(j);
}

/* ---------- message dispatch ---------- */

static void handle_text(const char *m) {
    char t[16];
    if (!json_get_str(m, "t", t, sizeof(t))) return;

    /* Input is by far the most frequent message; keep it first. */
    if (strcmp(t, "k") == 0 || strcmp(t, "mm") == 0 || strcmp(t, "mp") == 0 || strcmp(t, "mb") == 0 ||
        strcmp(t, "ms") == 0 || strcmp(t, "g") == 0 || strcmp(t, "ga") == 0 || strcmp(t, "txt") == 0) {
        stream_input(m);
        return;
    }
    if (strcmp(t, "idr") == 0) {
        stream_resync();
        return;
    }
    if (strcmp(t, "stop") == 0) {
        stream_stop();
        return;
    }
    if (strcmp(t, "ping") == 0) {
        char out[96];
        snprintf(out, sizeof(out), "{\"t\":\"pong\",\"ts\":%.0f}", json_get_num(m, "ts", 0));
        client_send_text(out);
        return;
    }

    job_t *j = calloc(1, sizeof(job_t));
    snprintf(j->cmd, sizeof(j->cmd), "%s", t);

    if (strcmp(t, "pair") == 0) {
        unsigned char r[4];
        RAND_bytes(r, sizeof(r));
        for (int i = 0; i < 4; i++) j->pin[i] = (char)('0' + r[i] % 10);
        char out[64];
        snprintf(out, sizeof(out), "{\"t\":\"pin\",\"pin\":\"%s\"}", j->pin);
        client_send_text(out);
    } else if (strcmp(t, "start") == 0) {
        j->sp.appid = (int)json_get_int(m, "app", 0);
        j->sp.width = (int)json_get_int(m, "w", 1920);
        j->sp.height = (int)json_get_int(m, "h", 1080);
        j->sp.fps = (int)json_get_int(m, "fps", 60);
        j->sp.bitrate_kbps = (int)json_get_int(m, "bitrate", 20000);
        j->sp.sops = json_get_int(m, "sops", 1) != 0;
        j->sp.host_audio = json_get_int(m, "hostAudio", 0) != 0;
    } else if (strcmp(t, "hello") != 0 && strcmp(t, "apps") != 0 && strcmp(t, "quit") != 0) {
        free(j);
        return;
    }
    run_job(j);
}

static void client_thread(void *arg) {
    ws_conn_t *c = arg;
    fprintf(stderr, "[bridge] TV connected from %s\n", c->peer);
    for (;;) {
        uint8_t *data;
        size_t len;
        int op = ws_recv(c, &data, &len);
        if (op < 0) break;
        if (op == WS_TEXT) handle_text((const char *)data);
        free(data);
    }
    fprintf(stderr, "[bridge] TV disconnected (%s)\n", c->peer);

    bool was_current = false;
    mutex_lock(&g_client_lock);
    if (g_client == c) {
        g_client = NULL;
        was_current = true;
    }
    mutex_unlock(&g_client_lock);
    /* Nobody is watching any more: end the stream but leave the game running. */
    if (was_current) stream_stop();
    ws_close(c);
    free(c->sendbuf);
    free(c);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "moonlight-tizen4 bridge " BRIDGE_VERSION "\n\n"
            "Usage: %s [options]\n"
            "  --host ADDR     Sunshine/GFE host address (default 127.0.0.1)\n"
            "  --port N        WebSocket port for the TV app (default 8090)\n"
            "  --test FILE     Stream an H.264 Annex-B file instead of a real host\n"
            "                  (for testing the TV app without Sunshine)\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 8090;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--test") == 0 && i + 1 < argc) g_test_file = argv[++i];
        else {
            usage(argv[0]);
            return strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 ? 0 : 1;
        }
    }

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    if (!net_init()) return 1;
    mutex_init(&g_client_lock);
    mutex_init(&g_job_lock);
    gs_server_setup(&g_server, host);
    if (!g_test_file && !gs_init()) return 1;

    sock_t ls = ws_listen((uint16_t)port);
    if (ls == SOCK_INVALID) {
        fprintf(stderr, "[bridge] cannot listen on port %d\n", port);
        return 1;
    }
    if (g_test_file) fprintf(stderr, "[bridge] TEST MODE: streaming %s\n", g_test_file);
    else fprintf(stderr, "[bridge] host: %s\n", host);
    fprintf(stderr, "[bridge] listening on port %d - enter this PC's IP address in the TV app\n", port);

    for (;;) {
        ws_conn_t *c = calloc(1, sizeof(ws_conn_t));
        if (!ws_accept(ls, c)) {
            free(c);
            continue;
        }
        /* A new TV connection replaces the old one (e.g. after the app restarts). */
        mutex_lock(&g_client_lock);
        ws_conn_t *old = g_client;
        g_client = c;
        if (old) {
            old->open = false;
            shutdown(old->sock, 2);
        }
        mutex_unlock(&g_client_lock);
        if (!thread_spawn(client_thread, c)) {
            mutex_lock(&g_client_lock);
            g_client = NULL;
            mutex_unlock(&g_client_lock);
            ws_close(c);
            free(c);
        }
    }
}
