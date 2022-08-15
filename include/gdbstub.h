#ifndef GDBSTUB
#define GDBSTUB

#include <stdbool.h>
#include "conn.h"

typedef enum {
    EVENT_NONE,
    EVENT_CONT,
} event_t;

typedef enum {
    ACT_CONT,
    ACT_SHUTDOWN,
} action_t;

struct target_ops {
    action_t (*cont)(void *args);
};

typedef struct {
    conn_t conn;
    pktbuf_t in;
    struct target_ops *ops;
} gdbstub_t;

bool gdbstub_init(gdbstub_t *gdbstub, struct target_ops *ops, char *s);
bool gdbstub_run(gdbstub_t *gdbstub, void *args);
void gdbstub_close(gdbstub_t *gdbstub);

#endif
