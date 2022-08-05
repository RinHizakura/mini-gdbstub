#include <stdio.h>
#include "gdbstub.h"

struct emu {
    int reg[2];
    gdbstub_t gdbstub;
};

action_t emu_handle_event(event_t e, void *args)
{
    struct emu *emu = (struct emu *) args;
    if (e == EVENT_NONE)
        return ACT_SHUTDOWN;

    return ACT_CONT;
}

struct target_ops emu_ops = {
    .handle_event = emu_handle_event,
};

int main()
{
    struct emu emu;
    if (!gdbstub_init(&emu.gdbstub, &emu_ops, "127.0.0.1:1234")) {
        fprintf(stderr, "Fail to create socket.\n");
        return -1;
    }

    if (!gdbstub_run(&emu.gdbstub, (void *) &emu)) {
        fprintf(stderr, "Fail to run in debug mode.\n");
        return -1;
    }
    gdbstub_close(&emu.gdbstub);

    return 0;
}
