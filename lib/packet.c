#include "packet.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void pktbuf_clear(pktbuf_t *pktbuf)
{
    pktbuf->end_pos = NULL;
    pktbuf->size = 0;
}

void pktbuf_init(pktbuf_t *pktbuf)
{
    memset(pktbuf->data, 0, MAX_PACKET_SIZE);
    pktbuf_clear(pktbuf);
}

void pktbuf_fill(pktbuf_t *pktbuf, uint8_t *buf, ssize_t len)
{
    if (len < 0)
        return;

    assert(pktbuf->size + len < MAX_PACKET_SIZE);
    memcpy(pktbuf->data + pktbuf->size, buf, len);
    pktbuf->size += len;
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
    pktbuf->end_pos = memchr(pktbuf->data, '#', pktbuf->size);

    if (pktbuf->end_pos == NULL)
        return false;

    return true;
}

packet_t *pktbuf_top_packet(pktbuf_t *pktbuf)
{
    if (pktbuf->end_pos == NULL)
        return NULL;

    packet_t *pkt = malloc(sizeof(packet_t));
    pkt->start = pktbuf->data;
    pkt->end = pktbuf->end_pos;
    return pkt;
}
