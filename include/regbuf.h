#ifndef REG_H
#define REG_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    void *buf;
    size_t sz;
} regbuf_t;

bool regbuf_init(regbuf_t *reg);
void *regbuf_get(regbuf_t *reg, size_t reg_sz);
void regbuf_destroy(regbuf_t *reg);
#endif
