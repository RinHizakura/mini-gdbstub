#include "packet.h"
#include <assert.h>
#include <string.h>

void packet_fill(struct packet *pkt, uint8_t *buf, ssize_t len)
{
    if (len < 0)
        return;

    assert(pkt->size + len < MAX_PACKET_SIZE);
    memcpy(pkt->data + pkt->size, buf, len);
    pkt->size += len;
}
