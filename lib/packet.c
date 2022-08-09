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
    memset(pktbuf->data, 0, MAX_PACKET_SIZE + 1);
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

    /* FIXME: Move end position to the packet checksum. We should
     * read until the checksum instead of assumming that they must exist. */
    pktbuf->end_pos += CSUM_SIZE;
    assert(pktbuf->end_pos - pktbuf->data <= pktbuf->size);
    return true;
}

packet_t *pktbuf_pop_packet(pktbuf_t *pktbuf)
{
    if (pktbuf->end_pos == NULL)
        return NULL;

    /* FIXME: As you can see, we keep moving memory frequently
     * which is not a good practice. */
    int old_pkt_size = (pktbuf->end_pos - pktbuf->data);
    packet_t *pkt = calloc(1, sizeof(packet_t));
    memcpy(pkt->data, pktbuf->data, old_pkt_size);
    pkt->end = pkt->data + (pktbuf->end_pos - pktbuf->data);

    memmove(pktbuf->data, pktbuf->end_pos + 1, pktbuf->size - old_pkt_size);
    pktbuf->size -= old_pkt_size;
    pktbuf->end_pos = NULL;
    return pkt;
}
