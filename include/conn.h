#ifndef CONN_H
#define CONN_H

#include <stdbool.h>
#include "packet.h"

typedef struct {
    int listen_fd;
    int socket_fd;

    pktbuf_t in, out;
} conn_t;

bool conn_init(conn_t *conn, char *addr_str, int port);
packet_t conn_recv(conn_t *conn);
void conn_close(conn_t *conn);
#endif
