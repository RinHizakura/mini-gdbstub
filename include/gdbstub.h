#ifndef GDBSTUB
#define GDBSTUB

typedef struct {
    int socket;
} gdbstub_t;

void gdbstub_init(gdbstub_t *gdbstub);
#endif
