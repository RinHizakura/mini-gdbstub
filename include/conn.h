#ifndef CONN_H
#define CONN_H

#include <stdbool.h>
#include "packet.h"

#define MAX_SEND_PACKET_SIZE (0x400)

typedef struct {
    int listen_fd;
    int socket_fd;
} conn_t;

bool conn_init(conn_t *conn, char *addr_str, int port);
void conn_recv_packet(conn_t *conn, pktbuf_t *pktbuf);
void conn_send_str(conn_t *conn, char *str);
void conn_send_pktstr(conn_t *conn, char *pktstr);
void conn_close(conn_t *conn);
#endif
