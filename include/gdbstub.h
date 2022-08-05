#ifndef GDBSTUB
#define GDBSTUB

#include <stdbool.h>

typedef enum {
    EVENT_NONE,
} event_t;

typedef enum {
    ACT_CONT,
    ACT_SHUTDOWN,
} action_t;

typedef struct gdbstub gdbstub_t;
typedef action_t (*handle_event_func)(event_t e, void *args);
struct gdbstub {
    int socket_fd;
    int listen_fd;

    handle_event_func event_cb;
};

bool gdbstub_init(gdbstub_t *gdbstub, handle_event_func event_cb, char *s);
bool gdbstub_run(gdbstub_t *gdbstub, void *args);
void gdbstub_close(gdbstub_t *gdbstub);

#endif
