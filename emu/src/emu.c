#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdbstub.h"

#define read_len(bit, ptr, value)                             \
    do {                                                      \
        value = 0;                                            \
        for (int i = 0; i < ((bit) >> 3); i++)                \
            value |= ((uint64_t) (*((ptr) + i))) << (i << 3); \
    } while (0)

#define write_len(bit, ptr, value)                     \
    do {                                               \
        for (int i = 0; i < ((bit) >> 3); i++)         \
            *((ptr) + i) = (value >> (i << 3)) & 0xff; \
    } while (0)

#define MEM_SIZE (1024)
struct mem {
    uint8_t *mem;
    size_t code_size;
};

struct emu {
    struct mem m;
    uint64_t x[32];
    uint64_t pc;

    bool bp_is_set;
    uint64_t bp_addr;

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

static void emu_exec(struct emu *emu, uint32_t inst)
{
    uint8_t opcode = inst & 0x7f;
    uint8_t rd = (inst >> 7) & 0x1f;
    uint8_t rs1 = (inst >> 15) & 0x1f;
    uint8_t rs2 = (inst >> 20) & 0x1f;
    uint8_t funct3 = (inst >> 12) & 0x7;
    uint8_t funct7 = (inst >> 25) & 0x7f;

    uint8_t *ptr;
    uint64_t imm, value;

#ifdef DEBUG
    printf("[%4lx] opcode: %2x, funct3: %x, funct7: %2x\n", emu->pc - 4, opcode,
           funct3, funct7);
#endif

    switch (opcode) {
    case 0x3:
        switch (funct3) {
        case 0x2:
            // lw
            imm = (int32_t) (inst & 0xfff00000) >> 20;
            ptr = emu->m.mem + emu->x[rs1] + imm;
            read_len(32, ptr, value);
            emu->x[rd] = value;
            return;
        default:
            break;
        }
        break;
    case 0x13:
        switch (funct3) {
        case 0x0:
            // addi
            imm = (int32_t) (inst & 0xfff00000) >> 20;
            emu->x[rd] = emu->x[rd] + imm;
            return;
        case 0x2:
            // slti
            imm = (int32_t) (inst & 0xfff00000) >> 20;
            emu->x[rd] = (int64_t) emu->x[rs1] < (int64_t) imm ? 1 : 0;
            return;
        default:
            break;
        }
        break;
    case 0x23:
        switch (funct3) {
        case 0x2:
            // sw
            imm = (((int32_t) (inst & 0xfe000000) >> 20) |
                   (int32_t) ((inst >> 7) & 0x1f));
            ptr = emu->m.mem + emu->x[rs1] + imm;
            write_len(32, ptr, emu->x[rs2]);
            return;
        case 0x3:
            // sd
            imm = (((int32_t) (inst & 0xfe000000) >> 20) |
                   (int32_t) ((inst >> 7) & 0x1f));
            ptr = emu->m.mem + emu->x[rs1] + imm;
            write_len(64, ptr, emu->x[rs2]);
            return;
        default:
            break;
        }
        break;
    case 0x33:
        switch (funct3) {
        case 0x0:
            switch (funct7) {
            case 0x00:
                // add
                emu->x[rd] = emu->x[rs1] + emu->x[rs2];
                return;
            default:
                break;
            }
            break;
        default:
            break;
        }
        break;
    case 0x3b:
        switch (funct3) {
        case 0x0:
            switch (funct7) {
            case 0x00:
                // addw
                emu->x[rd] =
                    (int32_t) ((uint32_t) emu->x[rs1] + (uint32_t) emu->x[rs2]);
                return;
            default:
                break;
            }
            return;
        default:
            break;
        }
        break;
    case 0x6f:
        // jal
        emu->x[rd] = emu->pc;
        // imm[20|10:1|11|19:12] = inst[31|30:21|20|19:12]
        imm = ((int32_t) (inst & 0x80000000) >> 11)  // 20
              | (int32_t) ((inst & 0xff000))         // 19:12
              | (int32_t) ((inst >> 9) & 0x800)      // 11
              | (int32_t) ((inst >> 20) & 0x7fe);    // 10:1
        emu->pc = emu->pc + imm - 4;
        return;
    default:
        break;
    }

    printf("Not implemented or invalid instruction@%lx\n", emu->pc - 4);
    printf("opcode:%x, funct3:%x, funct7:%x\n", opcode, funct3, funct7);
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

static size_t emu_get_reg_bytes(int regno __attribute__((unused)))
{
    return 8;
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
    .get_reg_bytes = emu_get_reg_bytes,
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
                      (arch_info_t){
                          .smp = 1,
                          .reg_num = 33,
                          .target_desc = TARGET_RV64,
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
