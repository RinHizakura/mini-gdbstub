#ifndef CONN_H
#define CONN_H

#include <stdbool.h>

typedef struct {
    int listen_fd;
    int socket_fd;
} conn_t;

bool conn_init(conn_t *conn, char *addr_str, int port);
void conn_recv(conn_t *conn);
void conn_close(conn_t *conn);
#endif
