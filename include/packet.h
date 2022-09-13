#ifndef PACKET_H
#define PACKET_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#define STR_ACK "+"

/* NOTE: this will be deprecated */
#define MAX_PACKET_SIZE (0x400)
#define CSUM_SIZE (2)

typedef struct {
    int end_pos;
    uint8_t data[];
} packet_t;

/* A naive packet buffer: maintain a big array to fill the packet */
typedef struct {
    int size;    /* the size for all valid characters in data buffer */
    int cap;     /* the capacity (1 << cap) of the data buffer */
    int end_pos; /* the end position of the first packet in data buffer */
    uint8_t *data;
} pktbuf_t;

void pktbuf_init(pktbuf_t *pktbuf);
ssize_t pktbuf_fill_from_file(pktbuf_t *pktbuf, int fd);
bool pktbuf_is_complete(pktbuf_t *pktbuf);
packet_t *pktbuf_pop_packet(pktbuf_t *pktbuf);
void pktbuf_destroy(pktbuf_t *pktbuf);

#endif
