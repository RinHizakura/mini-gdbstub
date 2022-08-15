#include <stdio.h>
#include "gdbstub.h"

struct emu {
    size_t x[32];
    size_t pc;
    gdbstub_t gdbstub;
};

action_t emu_cont(void *args)
{
    struct emu *emu = (struct emu *) args;
    emu->x[1] = 0x88;
    return ACT_RESUME;
}

size_t emu_read_reg(void *args, int regno)
{
    struct emu *emu = (struct emu *) args;
    if (regno > 32) {
        return -1;
    } else if (regno == 32) {
        return emu->pc;
    } else {
        return emu->x[regno];
    }
}

struct target_ops emu_ops = {
    .read_reg = emu_read_reg,
    .cont = emu_cont,
};

int main()
{
    struct emu emu;
    for (int i = 0; i < 32; i++)
        emu.x[i] = i;
    emu.pc = 0x12345678;

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
