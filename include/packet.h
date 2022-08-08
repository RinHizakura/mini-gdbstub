#ifndef PACKET_H
#define PACKET_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#define MAX_PACKET_SIZE (0x200)

/* A naive packet buffer: maintain a big array to fill the packet */
struct packet {
    int size;         /* the size for all valid characters in data buffer */
    uint8_t *end_pos; /* the end position of the first packet in data buffer */
    uint8_t data[MAX_PACKET_SIZE];
};

void packet_init(struct packet *pkt);
void packet_fill(struct packet *pkt, uint8_t *buf, ssize_t len);
bool packet_is_complete(struct packet *pkt);
#endif
