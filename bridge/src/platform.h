/* Minimal socket/thread portability layer (POSIX + Windows). */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
#  define sock_errno WSAGetLastError()
typedef HANDLE thread_t;
typedef CRITICAL_SECTION mutex_t;
#  define mutex_init(m)    InitializeCriticalSection(m)
#  define mutex_lock(m)    EnterCriticalSection(m)
#  define mutex_unlock(m)  LeaveCriticalSection(m)
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <errno.h>
#  include <pthread.h>
typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
#  define sock_errno errno
typedef pthread_t thread_t;
typedef pthread_mutex_t mutex_t;
#  define mutex_init(m)    pthread_mutex_init(m, NULL)
#  define mutex_lock(m)    pthread_mutex_lock(m)
#  define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

typedef void (*thread_fn)(void *arg);

bool net_init(void);
/* Starts a detached thread. */
bool thread_spawn(thread_fn fn, void *arg);
void sleep_ms(int ms);
/* Connects a TCP socket with a timeout. Returns SOCK_INVALID on failure. */
sock_t tcp_connect(const char *host, uint16_t port, int timeout_ms);
void sock_set_timeouts(sock_t s, int recv_ms, int send_ms);
void sock_set_nodelay(sock_t s);
/* Sends the whole buffer. Returns false on error. */
bool sock_send_all(sock_t s, const void *buf, size_t len);
/* Returns the per-user config directory (created if missing). */
const char *config_dir(void);
