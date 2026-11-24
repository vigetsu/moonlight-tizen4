/* Minimal RFC 6455 WebSocket server endpoint (one connection object per client). */
#pragma once

#include "platform.h"

#define WS_TEXT   0x1
#define WS_BINARY 0x2
#define WS_CLOSE  0x8
#define WS_PING   0x9
#define WS_PONG   0xA

typedef struct {
    sock_t sock;
    mutex_t send_lock;
    uint8_t *sendbuf;
    size_t sendcap;
    bool open;
    char peer[64];
} ws_conn_t;

sock_t ws_listen(uint16_t port);
/*
 * Accepts one TCP connection and performs the HTTP upgrade. Plain HTTP
 * requests get a short status page and false is returned.
 */
bool ws_accept(sock_t listener, ws_conn_t *c);
/*
 * Receives one complete message (handles ping/pong internally). Returns the
 * opcode (WS_TEXT/WS_BINARY) or -1 when the connection is closed. *data is
 * malloc'd and NUL terminated; free it after use.
 */
int ws_recv(ws_conn_t *c, uint8_t **data, size_t *len);
/* Thread-safe send. If prefix >= 0 that byte is put before the payload. */
bool ws_send(ws_conn_t *c, int opcode, int prefix, const void *data, size_t len);
bool ws_send_text(ws_conn_t *c, const char *text);
void ws_close(ws_conn_t *c);
