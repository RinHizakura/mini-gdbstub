#ifndef GDBSTUB
#define GDBSTUB

#include <stdbool.h>

typedef struct {
    int socket_fd;
    int listen_fd;
} gdbstub_t;

bool gdbstub_init(gdbstub_t *gdbstub, char *s);
void gdbstub_close(gdbstub_t *gdbstub);
#endif
