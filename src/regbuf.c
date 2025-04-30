#include "regbuf.h"

#include <stdlib.h>

bool regbuf_init(regbuf_t *reg)
{
    /* Default to 8 bytes register size. */
    reg->sz = 8;
    reg->buf = malloc(reg->sz);

    if (!reg->buf)
        return false;

    return true;
}

void *regbuf_get(regbuf_t *reg, size_t reg_sz)
{
    if (reg_sz <= reg->sz)
        return reg->buf;

    free(reg->buf);

    while (reg_sz > reg->sz) {
        reg->sz <<= 1;
    }

    reg->buf = malloc(reg->sz);
    return reg->buf;
}

void regbuf_destroy(regbuf_t *reg)
{
    free(reg->buf);
}
