#pragma once
/* A client connection of the web server: a plain TCP socket or a TLS session
 * on top of one (tls.c). The web handlers and the live WebSocket viewer only
 * talk through these calls, so HTTP and HTTPS share all of their code.
 * A connection must be used by one thread at a time. */
#include <stddef.h>
#include <sys/types.h>

typedef struct conn {
    int   fd;
    void *ssl;          /* mbedtls_ssl_context*, NULL for plain HTTP */
    int   nonblock;     /* internal: current read is non-blocking */
    void (*free_extra)(void *);   /* internal: per-connection TLS client state */
    void *extra;
} conn_t;

/* Wrap an accepted plain socket (heap-allocated; conn_close frees it). */
conn_t *conn_plain(int fd);
/* 1 if the connection is HTTPS. */
static inline int conn_is_tls(const conn_t *c) { return c->ssl != NULL; }
/* Send everything; 0 ok, -1 error/timeout (SO_SNDTIMEO on the socket). */
int     conn_send_all(conn_t *c, const void *p, size_t n);
/* Blocking read (SO_RCVTIMEO applies): >0 bytes, 0 peer closed, -1 error. */
ssize_t conn_recv(conn_t *c, void *p, size_t n);
/* Non-blocking read: >0 bytes, 0 peer closed, -1 error, -2 nothing yet. */
ssize_t conn_recv_nb(conn_t *c, void *p, size_t n);
/* 1 if a read would not block (data or EOF/error pending), 0 otherwise. */
int     conn_readable(conn_t *c, int timeout_ms);
/* Orderly close (TLS close_notify), closes the socket and frees c. */
void    conn_close(conn_t *c);
/* Half-close the sending side after the last response. */
void    conn_shutdown_wr(conn_t *c);
