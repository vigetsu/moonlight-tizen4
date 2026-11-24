#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#else
#  include <fcntl.h>
#  include <poll.h>
#endif

bool net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

typedef struct { thread_fn fn; void *arg; } thread_start_t;

#ifdef _WIN32
static DWORD WINAPI thread_tramp(LPVOID p) {
#else
static void *thread_tramp(void *p) {
#endif
    thread_start_t ts = *(thread_start_t *)p;
    free(p);
    ts.fn(ts.arg);
    return 0;
}

bool thread_spawn(thread_fn fn, void *arg) {
    thread_start_t *ts = malloc(sizeof(*ts));
    if (!ts) return false;
    ts->fn = fn;
    ts->arg = arg;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, thread_tramp, ts, 0, NULL);
    if (!h) { free(ts); return false; }
    CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, thread_tramp, ts) != 0) { free(ts); return false; }
    pthread_detach(t);
#endif
    return true;
}

void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

static void set_nonblocking(sock_t s, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

sock_t tcp_connect(const char *host, uint16_t port, int timeout_ms) {
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%u", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return SOCK_INVALID;

    sock_t s = SOCK_INVALID;
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == SOCK_INVALID) continue;
        set_nonblocking(s, true);
        int rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        bool ok = rc == 0;
        if (!ok) {
#ifdef _WIN32
            bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            bool pending = errno == EINPROGRESS;
#endif
            if (pending) {
                fd_set wf;
                struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
                FD_ZERO(&wf);
                FD_SET(s, &wf);
                if (select((int)s + 1, NULL, &wf, NULL, &tv) == 1) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
                    ok = err == 0;
                }
            }
        }
        if (ok) {
            set_nonblocking(s, false);
            break;
        }
        sock_close(s);
        s = SOCK_INVALID;
    }
    freeaddrinfo(res);
    return s;
}

void sock_set_timeouts(sock_t s, int recv_ms, int send_ms) {
#ifdef _WIN32
    DWORD r = (DWORD)recv_ms, w = (DWORD)send_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&r, sizeof(r));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&w, sizeof(w));
#else
    struct timeval r = { recv_ms / 1000, (recv_ms % 1000) * 1000 };
    struct timeval w = { send_ms / 1000, (send_ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof(w));
#endif
}

void sock_set_nodelay(sock_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
}

bool sock_send_all(sock_t s, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
#ifdef _WIN32
        int n = send(s, p, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
#else
        ssize_t n = send(s, p, len, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
#ifndef _WIN32
            if (n < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

const char *config_dir(void) {
    static char dir[1024];
    if (dir[0]) return dir;
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    snprintf(dir, sizeof(dir), "%s\\moonlight-tizen4", base ? base : ".");
    _mkdir(dir);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char parent[900];
    if (xdg && *xdg) snprintf(parent, sizeof(parent), "%s", xdg);
    else snprintf(parent, sizeof(parent), "%s/.config", home ? home : ".");
    mkdir(parent, 0700);
    snprintf(dir, sizeof(dir), "%s/moonlight-tizen4", parent);
    mkdir(dir, 0700);
#endif
    return dir;
}
