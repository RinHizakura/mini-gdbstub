#ifndef CONN_H
#define CONN_H

#include <pthread.h>
#include <stdbool.h>
#include "packet.h"

#define MAX_SEND_PACKET_SIZE (0x400)
#define MAX_DATA_PAYLOAD ( MAX_SEND_PACKET_SIZE - (2 + CSUM_SIZE + 2))

typedef struct {
    int listen_fd;
    int socket_fd;

    pktbuf_t pktbuf;
} conn_t;

bool conn_init(conn_t *conn, char *addr_str, int port);
void conn_recv_packet(conn_t *conn);
packet_t *conn_pop_packet(conn_t *conn);
bool conn_try_recv_intr(conn_t *conn);
void conn_send_str(conn_t *conn, char *str);
void conn_send_pktstr(conn_t *conn, char *pktstr);
void conn_close(conn_t *conn);
#endif
