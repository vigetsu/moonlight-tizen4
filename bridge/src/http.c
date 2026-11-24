#include "http.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <openssl/err.h>

typedef struct {
    char *data;
    size_t len, cap;
} buf_t;

static bool buf_append(buf_t *b, const char *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8192;
        while (cap < b->len + n + 1) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) return false;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
    return true;
}

static const char *ci_find(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0) return hay;
    return NULL;
}

/* Decodes a chunked body in place. */
static size_t dechunk(char *body, size_t len) {
    char *src = body, *end = body + len, *dst = body;
    while (src < end) {
        char *eol = strstr(src, "\r\n");
        if (!eol) break;
        size_t n = strtoul(src, NULL, 16);
        src = eol + 2;
        if (n == 0 || src + n > end) break;
        memmove(dst, src, n);
        dst += n;
        src += n + 2;
    }
    *dst = 0;
    return (size_t)(dst - body);
}

bool http_get(const char *host, uint16_t port, const char *path, SSL_CTX *tls_ctx,
              int timeout_ms, http_resp_t *out) {
    memset(out, 0, sizeof(*out));
    sock_t s = tcp_connect(host, port, 5000);
    if (s == SOCK_INVALID) return false;
    sock_set_timeouts(s, timeout_ms, 10000);

    SSL *ssl = NULL;
    bool ok = false;
    buf_t b = {0};

    if (tls_ctx) {
        ssl = SSL_new(tls_ctx);
        if (!ssl) goto done;
        SSL_set_fd(ssl, (int)s);
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) != 1) {
            ERR_print_errors_fp(stderr);
            goto done;
        }
    }

    char req[16384];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: moonlight-tizen4\r\n"
                     "Accept: */*\r\nConnection: close\r\n\r\n", path, host, port);
    if (n <= 0 || (size_t)n >= sizeof(req)) goto done;

    if (ssl) {
        if (SSL_write(ssl, req, n) != n) goto done;
    } else if (!sock_send_all(s, req, (size_t)n)) {
        goto done;
    }

    char tmp[8192];
    for (;;) {
        int r = ssl ? SSL_read(ssl, tmp, sizeof(tmp)) : (int)recv(s, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        if (!buf_append(&b, tmp, (size_t)r)) goto done;
    }
    if (!b.data) goto done;

    char *hdr_end = strstr(b.data, "\r\n\r\n");
    if (!hdr_end || sscanf(b.data, "HTTP/%*s %d", &out->status) != 1) goto done;
    *hdr_end = 0;
    bool chunked = ci_find(b.data, "transfer-encoding: chunked") != NULL;
    char *body = hdr_end + 4;
    size_t blen = b.len - (size_t)(body - b.data);
    if (chunked) blen = dechunk(body, blen);

    out->body = malloc(blen + 1);
    if (!out->body) goto done;
    memcpy(out->body, body, blen);
    out->body[blen] = 0;
    out->len = blen;
    ok = true;

done:
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    sock_close(s);
    free(b.data);
    return ok;
}

void http_resp_free(http_resp_t *r) {
    free(r->body);
    r->body = NULL;
}

char *xml_get(const char *xml, const char *tag) {
    if (!xml) return NULL;
    char open[128], close[128];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *a = strstr(xml, open);
    if (!a) {
        /* Empty element, e.g. <tag/> */
        snprintf(open, sizeof(open), "<%s/>", tag);
        if (strstr(xml, open)) return strdup("");
        return NULL;
    }
    a += strlen(open);
    const char *z = strstr(a, close);
    if (!z) return NULL;
    size_t n = (size_t)(z - a);
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, a, n);
    r[n] = 0;
    return r;
}

int xml_status_code(const char *xml, char *msg, size_t msglen) {
    if (msg && msglen) msg[0] = 0;
    if (!xml) return -1;
    const char *p = strstr(xml, "status_code=\"");
    if (!p) return -1;
    int code = atoi(p + 13);
    const char *m = strstr(xml, "status_message=\"");
    if (m && msg && msglen) {
        m += 16;
        const char *e = strchr(m, '"');
        size_t n = e ? (size_t)(e - m) : 0;
        if (n >= msglen) n = msglen - 1;
        memcpy(msg, m, n);
        msg[n] = 0;
    }
    return code;
}
