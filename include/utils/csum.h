#ifndef CSUM_H
#define CSUM_H

#include <stddef.h>
#include <stdint.h>

uint8_t compute_checksum(char *buf, size_t len);

#endif
