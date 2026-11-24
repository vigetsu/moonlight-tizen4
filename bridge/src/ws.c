#include "ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define MAX_MSG (4 * 1024 * 1024)

sock_t ws_listen(uint16_t port) {
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALID) return s;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
        sock_close(s);
        return SOCK_INVALID;
    }
    return s;
}

static bool recv_exact(sock_t s, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        int r = (int)recv(s, p, (int)n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static const char *header_value(const char *req, const char *name, char *out, size_t outlen) {
    size_t nl = strlen(name);
    for (const char *line = req; line && *line; ) {
        if (strncasecmp(line, name, nl) == 0 && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (*v == ' ') v++;
            size_t i = 0;
            while (v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < outlen) {
                out[i] = v[i];
                i++;
            }
            out[i] = 0;
            return out;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

static const char STATUS_PAGE[] =
    "<!doctype html><meta charset=utf-8><title>moonlight-tizen4</title>"
    "<body style='font-family:sans-serif;background:#111;color:#eee;padding:2em'>"
    "<h1>moonlight-tizen4 k&ouml;pr&uuml;s&uuml; &ccedil;al&inodot;&scedil;&inodot;yor</h1>"
    "<p>TV uygulamas&inodot;nda bu bilgisayar&inodot;n IP adresini girin.</p>";

bool ws_accept(sock_t listener, ws_conn_t *c) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    memset(c, 0, sizeof(*c));
    c->sock = accept(listener, (struct sockaddr *)&peer, &plen);
    if (c->sock == SOCK_INVALID) return false;
    inet_ntop(AF_INET, &peer.sin_addr, c->peer, sizeof(c->peer));
    sock_set_timeouts(c->sock, 10000, 10000);

    char req[8192];
    size_t got = 0;
    while (got + 1 < sizeof(req)) {
        int r = (int)recv(c->sock, req + got, (int)(sizeof(req) - 1 - got), 0);
        if (r <= 0) break;
        got += (size_t)r;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[got] = 0;

    char key[128], upgrade[64];
    if (!header_value(req, "Sec-WebSocket-Key", key, sizeof(key)) ||
        !header_value(req, "Upgrade", upgrade, sizeof(upgrade)) || strcasecmp(upgrade, "websocket") != 0) {
        char resp[1024];
        int n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                         sizeof(STATUS_PAGE) - 1, STATUS_PAGE);
        sock_send_all(c->sock, resp, (size_t)n);
        sock_close(c->sock);
        return false;
    }

    char cat[256];
    unsigned char sha[SHA_DIGEST_LENGTH];
    char accept_key[64];
    snprintf(cat, sizeof(cat), "%s" WS_GUID, key);
    SHA1((unsigned char *)cat, strlen(cat), sha);
    EVP_EncodeBlock((unsigned char *)accept_key, sha, SHA_DIGEST_LENGTH);

    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key);
    if (!sock_send_all(c->sock, resp, (size_t)n)) {
        sock_close(c->sock);
        return false;
    }

    /* No receive timeout once upgraded: the TV may be idle in menus for a long time. */
    sock_set_timeouts(c->sock, 0, 5000);
    sock_set_nodelay(c->sock);
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(c->sock, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
    mutex_init(&c->send_lock);
    c->open = true;
    return true;
}

int ws_recv(ws_conn_t *c, uint8_t **data, size_t *len) {
    uint8_t *msg = NULL;
    size_t mlen = 0;
    int msg_op = 0;
    *data = NULL;
    *len = 0;

    for (;;) {
        uint8_t h[2];
        if (!recv_exact(c->sock, h, 2)) goto closed;
        bool fin = h[0] & 0x80;
        int op = h[0] & 0x0f;
        bool masked = h[1] & 0x80;
        uint64_t plen = h[1] & 0x7f;
        if (plen == 126) {
            uint8_t e[2];
            if (!recv_exact(c->sock, e, 2)) goto closed;
            plen = ((uint64_t)e[0] << 8) | e[1];
        } else if (plen == 127) {
            uint8_t e[8];
            if (!recv_exact(c->sock, e, 8)) goto closed;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | e[i];
        }
        if (plen > MAX_MSG || mlen + plen > MAX_MSG) goto closed;
        uint8_t mask[4] = {0};
        if (masked && !recv_exact(c->sock, mask, 4)) goto closed;

        uint8_t *payload = malloc((size_t)plen + 1);
        if (!payload) goto closed;
        if (plen && !recv_exact(c->sock, payload, (size_t)plen)) { free(payload); goto closed; }
        for (uint64_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];
        payload[plen] = 0;

        if (op == WS_PING) {
            ws_send(c, WS_PONG, -1, payload, (size_t)plen);
            free(payload);
            continue;
        }
        if (op == WS_PONG) { free(payload); continue; }
        if (op == WS_CLOSE) { free(payload); goto closed; }

        if (op != 0) msg_op = op;
        uint8_t *nm = realloc(msg, mlen + (size_t)plen + 1);
        if (!nm) { free(payload); goto closed; }
        msg = nm;
        memcpy(msg + mlen, payload, (size_t)plen);
        mlen += (size_t)plen;
        msg[mlen] = 0;
        free(payload);
        if (fin) {
            *data = msg;
            *len = mlen;
            return msg_op;
        }
    }

closed:
    free(msg);
    return -1;
}

bool ws_send(ws_conn_t *c, int opcode, int prefix, const void *data, size_t len) {
    if (!c->open) return false;
    size_t plen = len + (prefix >= 0 ? 1 : 0);
    mutex_lock(&c->send_lock);
    size_t need = plen + 10;
    if (need > c->sendcap) {
        uint8_t *nb = realloc(c->sendbuf, need);
        if (!nb) { mutex_unlock(&c->send_lock); return false; }
        c->sendbuf = nb;
        c->sendcap = need;
    }
    uint8_t *b = c->sendbuf;
    size_t o = 0;
    b[o++] = 0x80 | (uint8_t)opcode;
    if (plen < 126) {
        b[o++] = (uint8_t)plen;
    } else if (plen < 65536) {
        b[o++] = 126;
        b[o++] = (uint8_t)(plen >> 8);
        b[o++] = (uint8_t)plen;
    } else {
        b[o++] = 127;
        for (int i = 7; i >= 0; i--) b[o++] = (uint8_t)((uint64_t)plen >> (i * 8));
    }
    if (prefix >= 0) b[o++] = (uint8_t)prefix;
    if (len) memcpy(b + o, data, len);
    o += len;
    bool ok = sock_send_all(c->sock, b, o);
    if (!ok) c->open = false;
    mutex_unlock(&c->send_lock);
    return ok;
}

bool ws_send_text(ws_conn_t *c, const char *text) {
    return ws_send(c, WS_TEXT, -1, text, strlen(text));
}

void ws_close(ws_conn_t *c) {
    if (c->sock != SOCK_INVALID) {
        if (c->open) ws_send(c, WS_CLOSE, -1, NULL, 0);
        c->open = false;
        sock_close(c->sock);
        c->sock = SOCK_INVALID;
    }
}
