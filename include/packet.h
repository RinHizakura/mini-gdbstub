#ifndef PACKET_H
#define PACKET_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#define STR_ACK "+"
#define STR_NACK "-"
#define INTR_CHAR '\x03'

#define CSUM_SIZE (2)

typedef struct {
    int end_pos;
    uint8_t data[];
} packet_t;

/* A ring buffer for incoming packets: valid bytes live at logical offsets
 * [0, size), where logical offset i maps to data[(head + i) & mask]. */
typedef struct {
    int head;    /* physical index of logical offset 0 */
    int size;    /* the size for all valid characters in data buffer */
    int cap;     /* the capacity (1 << cap) of the data buffer */
    int end_pos; /* logical end position of the first packet, -1 if none */
    uint8_t *data;
} pktbuf_t;

bool pktbuf_init(pktbuf_t *pktbuf);
ssize_t pktbuf_fill_from_file(pktbuf_t *pktbuf, int fd);
bool pktbuf_is_complete(pktbuf_t *pktbuf);
packet_t *pktbuf_pop_packet(pktbuf_t *pktbuf);
void pktbuf_destroy(pktbuf_t *pktbuf);

/* Byte at logical offset i, 0 <= i < size */
static inline uint8_t pktbuf_at(const pktbuf_t *pktbuf, int i)
{
    return pktbuf->data[(pktbuf->head + i) & ((1 << pktbuf->cap) - 1)];
}

#endif
