#include "packet.h"
#include <assert.h>
#include <string.h>

static void packet_clear(struct packet *pkt)
{
    pkt->end_pos = NULL;
    pkt->size = 0;
}

void packet_init(struct packet *pkt)
{
    memset(pkt->data, 0, MAX_PACKET_SIZE);
    packet_clear(pkt);
}

void packet_fill(struct packet *pkt, uint8_t *buf, ssize_t len)
{
    if (len < 0)
        return;

    assert(pkt->size + len < MAX_PACKET_SIZE);
    memcpy(pkt->data + pkt->size, buf, len);
    pkt->size += len;
}

bool packet_is_complete(struct packet *pkt)
{
    int head = -1;

    /* skip to the head of next packet */
    for (int i = 0; i < pkt->size; i++) {
        if (pkt->data[i] == '$') {
            head = i;
            break;
        }
    }

    if (head < 0) {
        packet_clear(pkt);
        return false;
    } else if (head > 0) {
        /* moving memory for a valid packet */
        memmove(pkt->data, pkt->data + head, pkt->size - head);
        pkt->size -= head;
    }

    /* check the end of the buffer */
    pkt->end_pos = memchr(pkt->data, '#', pkt->size);

    if (pkt->end_pos == NULL)
        return false;

    return true;
}
