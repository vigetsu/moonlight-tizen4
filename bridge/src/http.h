#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <openssl/ssl.h>

typedef struct {
    char *body;       /* NUL terminated, owned */
    size_t len;
    int status;       /* HTTP status code */
} http_resp_t;

/*
 * Performs a GET request. If tls_ctx is non-NULL the request is made over TLS
 * (the context carries the client certificate). timeout_ms applies to connect
 * and to each read; pairing needs long reads because Sunshine waits for the PIN.
 */
bool http_get(const char *host, uint16_t port, const char *path, SSL_CTX *tls_ctx,
              int timeout_ms, http_resp_t *out);
void http_resp_free(http_resp_t *r);

/* Returns a malloc'd copy of the text inside the first <tag>...</tag>, or NULL. */
char *xml_get(const char *xml, const char *tag);
/* Reads the status_code attribute of the root element; returns -1 if absent. */
int xml_status_code(const char *xml, char *msg, size_t msglen);
