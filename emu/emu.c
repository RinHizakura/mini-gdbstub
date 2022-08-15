#include <stdio.h>
#include "gdbstub.h"

struct emu {
    int reg[2];
    gdbstub_t gdbstub;
};

action_t emu_cont(void *args)
{
    struct emu *emu = (struct emu *) args;
    // do somethig here......
    return ACT_SHUTDOWN;
}

struct target_ops emu_ops = {
    .cont = emu_cont,
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
