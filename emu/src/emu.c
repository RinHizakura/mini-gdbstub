#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gdbstub.h"

#define MAX_MEM_SIZE 1024

struct emu {
    size_t x[32];
    size_t pc;
    uint8_t *mem;
    gdbstub_t gdbstub;
};

action_t emu_cont(void *args)
{
    struct emu *emu = (struct emu *) args;
    emu->x[1] = 0x88;
    return ACT_RESUME;
}

action_t emu_stepi(void *args)
{
    struct emu *emu = (struct emu *) args;
    static int step_cnt = 0;
    emu->x[step_cnt % 32]++;
    step_cnt++;
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

size_t emu_read_mem(void *args, size_t addr, size_t len)
{
    struct emu *emu = (struct emu *) args;
    size_t val = 0;
    for (size_t i = 0; i < len; i++) {
        if (addr + i < MAX_MEM_SIZE) {
            val |= emu->mem[addr + i];
        }
    }
    return val;
}

struct target_ops emu_ops = {
    .read_reg = emu_read_reg,
    .read_mem = emu_read_mem,
    .cont = emu_cont,
    .stepi = emu_stepi,
};

int main()
{
    struct emu emu;
    for (int i = 0; i < 32; i++)
        emu.x[i] = i;
    emu.pc = 0;
    emu.mem = malloc(MAX_MEM_SIZE);
    memset(emu.mem, 0, MAX_MEM_SIZE);

    if (!gdbstub_init(&emu.gdbstub, &emu_ops, "127.0.0.1:1234")) {
        fprintf(stderr, "Fail to create socket.\n");
        return -1;
    }

    if (!gdbstub_run(&emu.gdbstub, (void *) &emu)) {
        fprintf(stderr, "Fail to run in debug mode.\n");
        return -1;
    }
    gdbstub_close(&emu.gdbstub);
    free(emu.mem);

    return 0;
}
