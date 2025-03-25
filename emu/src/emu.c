#include <errno.h>
#include <stddef.h>
#include <stdint.h>
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

    bool halt;

    gdbstub_t gdbstub;
};

static inline void emu_halt(struct emu *emu)
{
    __atomic_store_n(&emu->halt, true, __ATOMIC_RELAXED);
}

static inline void emu_start_run(struct emu *emu)
{
    __atomic_store_n(&emu->halt, false, __ATOMIC_RELAXED);
}

static inline bool emu_is_halt(struct emu *emu)
{
    return __atomic_load_n(&emu->halt, __ATOMIC_RELAXED);
}

#define asr_i64(value, amount) \
    (value < 0 ? ~(~value >> amount) : value >> amount)
static void emu_exec(struct emu *emu, uint32_t inst)
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

static void emu_init(struct emu *emu)
{
    memset(emu, 0, sizeof(struct emu));
    emu->pc = 0;
    emu->x[2] = MEM_SIZE;
    emu->bp_addr = -1;
    emu_halt(emu);
}

static int init_mem(struct mem *m, const char *filename)
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

static void free_mem(struct mem *m)
{
    free(m->mem);
}

static size_t emu_get_reg_rize(int regno __attribute__((unused)))
{
    return 4;
}

static int emu_read_reg(void *args, int regno, void *reg_value)
{
    struct emu *emu = (struct emu *) args;
    if (regno > 32) {
        return EFAULT;
    }

    if (regno == 32) {
        memcpy(reg_value, &emu->pc, 4);
    } else {
        memcpy(reg_value, &emu->x[regno], 4);
    }
    return 0;
}

static int emu_write_reg(void *args, int regno, void *data)
{
    struct emu *emu = (struct emu *) args;

    if (regno > 32) {
        return EFAULT;
    }

    if (regno == 32) {
        memcpy(&emu->pc, data, 4);
    } else {
        memcpy(&emu->x[regno], data, 4);
    }
    return 0;
}

static int emu_read_mem(void *args, size_t addr, size_t len, void *val)
{
    struct emu *emu = (struct emu *) args;
    if (addr + len > MEM_SIZE) {
        return EFAULT;
    }
    memcpy(val, (void *) emu->m.mem + addr, len);
    return 0;
}

static int emu_write_mem(void *args, size_t addr, size_t len, void *val)
{
    struct emu *emu = (struct emu *) args;
    if (addr + len > MEM_SIZE) {
        return EFAULT;
    }
    memcpy((void *) emu->m.mem + addr, val, len);
    return 0;
}

static gdb_action_t emu_cont(void *args)
{
    struct emu *emu = (struct emu *) args;

    emu_start_run(emu);
    while (emu->pc < emu->m.code_size && emu->pc != emu->bp_addr &&
           !emu_is_halt(emu)) {
        uint32_t inst;
        emu_read_mem(args, emu->pc, 4, &inst);
        emu->pc += 4;
        emu_exec(emu, inst);
    }

    return ACT_RESUME;
}

static gdb_action_t emu_stepi(void *args)
{
    struct emu *emu = (struct emu *) args;

    emu_start_run(emu);
    if (emu->pc < emu->m.code_size) {
        uint32_t inst;
        emu_read_mem(args, emu->pc, 4, &inst);
        emu->pc += 4;
        emu_exec(emu, inst);
    }

    return ACT_RESUME;
}

static bool emu_set_bp(void *args, size_t addr, bp_type_t type)
{
    struct emu *emu = (struct emu *) args;
    if (type != BP_SOFTWARE || emu->bp_is_set)
        return false;

    emu->bp_is_set = true;
    emu->bp_addr = addr;
    return true;
}

static bool emu_del_bp(void *args, size_t addr, bp_type_t type)
{
    struct emu *emu = (struct emu *) args;

    // It's fine when there's no matching breakpoint, just doing nothing
    if (type != BP_SOFTWARE || !emu->bp_is_set || emu->bp_addr != addr)
        return true;

    emu->bp_is_set = false;
    emu->bp_addr = 0;
    return true;
}

static void emu_on_interrupt(void *args)
{
    struct emu *emu = (struct emu *) args;
    emu_halt(emu);
}

struct target_ops emu_ops = {
    .get_reg_rize = emu_get_reg_rize,
    .read_reg = emu_read_reg,
    .write_reg = emu_write_reg,
    .read_mem = emu_read_mem,
    .write_mem = emu_write_mem,
    .cont = emu_cont,
    .stepi = emu_stepi,
    .set_bp = emu_set_bp,
    .del_bp = emu_del_bp,
    .on_interrupt = emu_on_interrupt,
};

int main(int argc, char *argv[])
{
    if (argc != 2) {
        return -1;
    }

    struct emu emu;
    emu_init(&emu);

    if (init_mem(&emu.m, argv[1]) == -1) {
        return -1;
    }

    if (!gdbstub_init(&emu.gdbstub, &emu_ops,
                      (arch_info_t) {
                          .smp = 1,
                          .reg_num = 33,
                          .target_desc = TARGET_RV32,
                      },
                      "127.0.0.1:1234")) {
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
