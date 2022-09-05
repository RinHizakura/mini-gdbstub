#include "utils/csum.h"

uint8_t compute_checksum(char *buf, size_t len)
{
    uint8_t csum = 0;
    for (size_t i = 0; i < len; ++i)
        csum += buf[i];
    return csum;
}
