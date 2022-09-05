#ifndef PACKET_H
#define PACKET_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#define STR_ACK "+"

#define MAX_PACKET_SIZE (0x400)
#define CSUM_SIZE (2)

typedef struct {
    uint8_t data[MAX_PACKET_SIZE + 1];
    int end_pos;
} packet_t;

/* A naive packet buffer: maintain a big array to fill the packet */
typedef struct {
    int size;    /* the size for all valid characters in data buffer */
    int end_pos; /* the end position of the first packet in data buffer */
    uint8_t data[MAX_PACKET_SIZE + 1];
} pktbuf_t;

void pktbuf_init(pktbuf_t *pktbuf);
ssize_t pktbuf_fill_from_file(pktbuf_t *pktbuf, int fd);
bool pktbuf_is_complete(pktbuf_t *pktbuf);
packet_t *pktbuf_pop_packet(pktbuf_t *pktbuf);

#endif
