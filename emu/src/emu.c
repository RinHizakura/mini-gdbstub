#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gdbstub.h"

#define MEM_SIZE (1024)
struct mem {
    uint8_t *mem;
    size_t code_size;
};

struct emu {
    struct mem m;
    size_t x[32];
    size_t pc;

    bool bp_is_set;
    size_t bp_addr;

    gdbstub_t gdbstub;
};

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

void emu_read_mem(void *args, size_t addr, size_t len, void *val)
{
    struct emu *emu = (struct emu *) args;
    if (addr + len > MEM_SIZE) {
        len = MEM_SIZE - addr;
    }
    memcpy(val, (void *) emu->m.mem + addr, len);
}

#define asr_i64(value, amount) \
    (value < 0 ? ~(~value >> amount) : value >> amount)
static void exec(struct emu *emu, uint32_t inst)
{
    uint8_t opcode = inst & 0x7f;
    uint8_t rd = (inst >> 7) & 0x1f;
    uint8_t rs1 = ((inst >> 15) & 0x1f);
    uint8_t rs2 = ((inst >> 20) & 0x1f);
    uint64_t imm = asr_i64((int) (inst & 0xfff00000), 20);

    switch (opcode) {
    // addi
    case 0x13:
        emu->x[rd] = emu->x[rd] + imm;
        break;
    // add
    case 0x33:
        emu->x[rd] = emu->x[rs1] + emu->x[rs2];
        break;
    default:
        printf("Not implemented or invalid opcode 0x%x\n", opcode);
        break;
    }
}

gdb_action_t emu_cont(void *args)
{
    struct emu *emu = (struct emu *) args;
    while (emu->pc < emu->m.code_size && emu->pc != emu->bp_addr) {
        uint32_t inst;
        emu_read_mem(args, emu->pc, 4, &inst);
        emu->pc += 4;
        exec(emu, inst);
    }

    return ACT_RESUME;
}

gdb_action_t emu_stepi(void *args)
{
    struct emu *emu = (struct emu *) args;
    if (emu->pc < emu->m.code_size) {
        uint32_t inst;
        emu_read_mem(args, emu->pc, 4, &inst);
        emu->pc += 4;
        exec(emu, inst);
    }

    return ACT_RESUME;
}

bool emu_set_swbp(void *args, size_t addr)
{
    struct emu *emu = (struct emu *) args;
    if (emu->bp_is_set)
        return false;

    emu->bp_is_set = true;
    emu->bp_addr = addr;
    return true;
}

struct target_ops emu_ops = {
    .read_reg = emu_read_reg,
    .read_mem = emu_read_mem,
    .cont = emu_cont,
    .stepi = emu_stepi,
    .set_swbp = emu_set_swbp,
};

int init_mem(struct mem *m, const char *filename)
{
    if (!filename) {
        return -1;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    size_t sz = ftell(fp) * sizeof(uint8_t);
    rewind(fp);

    m->mem = malloc(MEM_SIZE);
    if (!m->mem) {
        fclose(fp);
        return -1;
    }
    memset(m->mem, 0, MEM_SIZE);
    size_t read_size = fread(m->mem, sizeof(uint8_t), sz, fp);

    if (read_size != sz) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    m->code_size = read_size;
    return 0;
}

void free_mem(struct mem *m)
{
    free(m->mem);
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        return -1;
    }

    struct emu emu;
    memset(&emu, 0, sizeof(struct emu));
    emu.pc = 0;
    emu.x[2] = MEM_SIZE;
    emu.bp_addr = -1;
    if (init_mem(&emu.m, argv[1]) == -1) {
        return -1;
    }

    if (!gdbstub_init(&emu.gdbstub, &emu_ops, "127.0.0.1:1234")) {
        fprintf(stderr, "Fail to create socket.\n");
        return -1;
    }

    if (!gdbstub_run(&emu.gdbstub, (void *) &emu)) {
        fprintf(stderr, "Fail to run in debug mode.\n");
        return -1;
    }
    gdbstub_close(&emu.gdbstub);
    free_mem(&emu.m);

    return 0;
}
