#ifndef CONN_H
#define CONN_H

#include <pthread.h>
#include <stdbool.h>
#include "packet.h"

#define MAX_SEND_PACKET_SIZE (0x1000)
#define MAX_DATA_PAYLOAD (MAX_SEND_PACKET_SIZE - (2 + CSUM_SIZE + 2))

/* Timeout for socket write operations (milliseconds).
 * Prevents indefinite blocking if connection is congested or broken. */
#define CONN_SEND_TIMEOUT_MS 5000
#define CONN_SEND_POLL_MS 100

typedef struct {
    int listen_fd;
    int socket_fd;
    pthread_mutex_t send_mutex; /* Serialize socket writes */
} conn_t;

bool conn_init(conn_t *conn, char *addr_str, int port);
void conn_send_str(conn_t *conn, char *str);
void conn_send_pktstr(conn_t *conn, char *pktstr);
void conn_close(conn_t *conn);

/* Non-blocking send for ACKs.
 * Returns true if sent successfully, false if would block or error.
 * Used by reader thread to avoid blocking on send_mutex. */
bool conn_try_send_str(conn_t *conn, char *str);

#endif
