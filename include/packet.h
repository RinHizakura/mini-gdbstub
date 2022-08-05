#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <unistd.h>

#define MAX_PACKET_SIZE (0x4000)

/* A naive packet buffer: maintain a big array to fill the packet */
struct packet {
    int size;
    uint8_t data[MAX_PACKET_SIZE];
};

void packet_fill(struct packet *pkt, uint8_t *buf, ssize_t len);
#endif
