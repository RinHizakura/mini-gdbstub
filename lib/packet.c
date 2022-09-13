#include "packet.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void pktbuf_clear(pktbuf_t *pktbuf)
{
    pktbuf->end_pos = -1;
    pktbuf->size = 0;
}

#define DEFAULT_CAP (10)
void pktbuf_init(pktbuf_t *pktbuf)
{
    pktbuf->cap = DEFAULT_CAP;
    pktbuf->data = calloc(1, (1 << pktbuf->cap) * sizeof(uint8_t));
    pktbuf_clear(pktbuf);
}

ssize_t pktbuf_fill_from_file(pktbuf_t *pktbuf, int fd)
{
    assert((1 << pktbuf->cap) >= pktbuf->size);

    /* enlarge the buffer to read from file when it is full */
    if ((1 << pktbuf->cap) == pktbuf->size) {
        pktbuf->cap++;
        pktbuf->data =
            realloc(pktbuf->data, (1 << pktbuf->cap) * sizeof(uint8_t));
    }

    int left = (1 << pktbuf->cap) - pktbuf->size;
    uint8_t *buf = pktbuf->data + pktbuf->size;
    ssize_t nread = read(fd, buf, left);

    pktbuf->size += nread;
    return nread;
}

bool pktbuf_is_complete(pktbuf_t *pktbuf)
{
    int head = -1;

    /* skip to the head of next packet */
    for (int i = 0; i < pktbuf->size; i++) {
        if (pktbuf->data[i] == '$') {
            head = i;
            break;
        }
    }

    if (head < 0) {
        pktbuf_clear(pktbuf);
        return false;
    } else if (head > 0) {
        /* moving memory for a valid packet */
        memmove(pktbuf->data, pktbuf->data + head, pktbuf->size - head);
        pktbuf->size -= head;
    }

    /* check the end of the buffer */
    uint8_t *end_pos_ptr = memchr(pktbuf->data, '#', pktbuf->size);
    if (end_pos_ptr == NULL)
        return false;

    /* FIXME: Move end position to the packet checksum. We should
     * read until the checksum instead of assumming that they must exist. */
    pktbuf->end_pos = (end_pos_ptr - pktbuf->data) + CSUM_SIZE;
    assert(pktbuf->end_pos <= pktbuf->size);
    return true;
}

packet_t *pktbuf_pop_packet(pktbuf_t *pktbuf)
{
    if (pktbuf->end_pos == -1)
        return NULL;

    int old_pkt_size = pktbuf->end_pos + 1;
    packet_t *pkt = calloc(1, sizeof(packet_t) + old_pkt_size + 1);
    memcpy(pkt->data, pktbuf->data, old_pkt_size);
    pkt->end_pos = pktbuf->end_pos;
    pkt->data[old_pkt_size] = 0;

    memmove(pktbuf->data, pktbuf->data + old_pkt_size + 1,
            pktbuf->size - old_pkt_size);
    pktbuf->size -= old_pkt_size;
    pktbuf->end_pos = -1;
    return pkt;
}

void pktbuf_destroy(pktbuf_t *pktbuf)
{
    free(pktbuf->data);
}
