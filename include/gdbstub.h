#ifndef GDBSTUB
#define GDBSTUB

#include <stdbool.h>
#include "conn.h"

typedef enum {
    EVENT_NONE,
    EVENT_CONT,
    EVENT_DETACH,
    EVENT_STEP,
} gdb_event_t;

typedef enum {
    ACT_NONE,
    ACT_RESUME,
    ACT_SHUTDOWN,
} gdb_action_t;

typedef enum {
    BP_SOFTWARE = 0,
} bp_type_t;

struct target_ops {
    gdb_action_t (*cont)(void *args);
    gdb_action_t (*stepi)(void *args);
    size_t (*read_reg)(void *args, int regno);
    void (*read_mem)(void *args, size_t addr, size_t len, void *val);
    bool (*set_bp)(void *args, size_t addr, bp_type_t type);
    bool (*rm_bp)(void *args, size_t addr, bp_type_t type);
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
