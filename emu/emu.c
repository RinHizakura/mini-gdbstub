#include <stdio.h>
#include "gdbstub.h"

struct emu {
    int reg[2];
    gdbstub_t gdbstub;
};

int main()
{
    struct emu emu;
    if (!gdbstub_init(&emu.gdbstub, "127.0.0.1:1234")) {
        fprintf(stderr, "Fail to create socket.\n");
        return -1;
    }

    printf("Init done.\n");

    gdbstub_close(&emu.gdbstub);

    return 0;
}
