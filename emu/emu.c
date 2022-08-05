#include "gdbstub.h"

struct emu {
    int reg[2];
    gdbstub_t gdbstub;
};

int main()
{
    struct emu emu;
    gdbstub_init(&emu.gdbstub);

    return 0;
}
