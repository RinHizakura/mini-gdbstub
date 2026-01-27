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

#define MEM_SIZE (0x1000)
#define TOHOST_ADDR (MEM_SIZE - 4)
#ifdef RV32
#define REGSZ 4  // 32-bit registers = 4 bytes
#else
#define REGSZ 8  // 64-bit registers = 8 bytes
#endif

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

typedef struct inst {
    uint64_t inst;

    uint8_t rd;
    uint8_t rs1;
    uint8_t rs2;
    uint8_t funct3;
    uint8_t funct7;
} inst_t;

static inline int opcode_3(struct emu *emu, inst_t *inst)
{
    uint8_t *ptr;
    uint64_t value;
    uint64_t imm = (int32_t) (inst->inst & 0xfff00000) >> 20;

    switch (inst->funct3) {
    case 0x2:
        // lw
        ptr = emu->m.mem + emu->x[inst->rs1] + imm;
        read_len(32, ptr, value);
        emu->x[inst->rd] = value;
        return 0;
    case 0x3:
        // ld
        ptr = emu->m.mem + emu->x[inst->rs1] + imm;
        read_len(64, ptr, value);
        emu->x[inst->rd] = value;
        return 0;
    default:
        break;
    }

    return -1;
}

static inline int opcode_13(struct emu *emu, inst_t *inst)
{
    uint64_t imm = (int32_t) (inst->inst & 0xfff00000) >> 20;

    switch (inst->funct3) {
    case 0x0:
        // addi
        emu->x[inst->rd] = emu->x[inst->rs1] + imm;
        return 0;
    case 0x2:
        // slti
        emu->x[inst->rd] = (int64_t) emu->x[inst->rs1] < (int64_t) imm ? 1 : 0;
        return 0;
    default:
        break;
    }

    return -1;
}

static inline int opcode_17(struct emu *emu, inst_t *inst)
{
    // auipc
    uint64_t imm = (int32_t) (inst->inst & 0xfffff000);
    emu->x[inst->rd] = emu->pc + imm - 4;
    return 0;
}

static inline int opcode_1b(struct emu *emu, inst_t *inst)
{
    uint64_t imm = (int32_t) (inst->inst & 0xfff00000) >> 20;

    switch (inst->funct3) {
    case 0x0:
        // addiw
        emu->x[inst->rd] =
            (int32_t) (((uint32_t) emu->x[inst->rs1] + (uint32_t) imm));
        return 0;
    default:
        break;
    }

    return -1;
}

static inline int opcode_23(struct emu *emu, inst_t *inst)
{
    uint8_t *ptr;
    uint64_t imm = (((int32_t) (inst->inst & 0xfe000000) >> 20) |
                    (int32_t) ((inst->inst >> 7) & 0x1f));

    switch (inst->funct3) {
    case 0x0:
        // sb
        ptr = emu->m.mem + emu->x[inst->rs1] + imm;
        write_len(8, ptr, emu->x[inst->rs2]);
        return 0;
    case 0x2:
        // sw
        ptr = emu->m.mem + emu->x[inst->rs1] + imm;
        write_len(32, ptr, emu->x[inst->rs2]);
        return 0;
    case 0x3:
        // sd
        ptr = emu->m.mem + emu->x[inst->rs1] + imm;
        write_len(64, ptr, emu->x[inst->rs2]);
        return 0;
    default:
        break;
    }

    return -1;
}

static inline int opcode_33(struct emu *emu, inst_t *inst)
{
    switch (inst->funct3) {
    case 0x0:
        switch (inst->funct7) {
        case 0x00:
            // add
            emu->x[inst->rd] = emu->x[inst->rs1] + emu->x[inst->rs2];
            return 0;
        default:
            break;
        }
        break;
    default:
        break;
    }

    return -1;
}

static inline int opcode_37(struct emu *emu, inst_t *inst)
{
    // lui
    uint64_t imm = (int32_t) (inst->inst & 0xfffff000);
    emu->x[inst->rd] = imm;
    return 0;
}

static inline int opcode_3b(struct emu *emu, inst_t *inst)
{
    switch (inst->funct3) {
    case 0x0:
        switch (inst->funct7) {
        case 0x00:
            // addw
            emu->x[inst->rd] = (int32_t) ((uint32_t) emu->x[inst->rs1] +
                                          (uint32_t) emu->x[inst->rs2]);
            return 0;
        default:
            break;
        }
        break;
    default:
        break;
    }

    return -1;
}

static inline int opcode_67(struct emu *emu, inst_t *inst)
{
    // jalr
    uint64_t imm = (int32_t) (inst->inst & 0xfff00000) >> 20;
    emu->x[inst->rd] = emu->pc;
    emu->pc = (emu->x[inst->rs1] + imm) & ~1;

    return 0;
}

static inline int opcode_6f(struct emu *emu, inst_t *inst)
{
    uint64_t imm;

    // jal
    emu->x[inst->rd] = emu->pc;
    // imm[20|10:1|11|19:12] = inst[31|30:21|20|19:12]
    imm = ((int32_t) (inst->inst & 0x80000000) >> 11)  // 20
          | (int32_t) ((inst->inst & 0xff000))         // 19:12
          | (int32_t) ((inst->inst >> 9) & 0x800)      // 11
          | (int32_t) ((inst->inst >> 20) & 0x7fe);    // 10:1
    emu->pc = emu->pc + imm - 4;
    return 0;
}

static int emu_exec(struct emu *emu, uint32_t raw_inst)
{
    int ret = -1;
    uint8_t opcode = raw_inst & 0x7f;

    inst_t inst;

    // Emulate register x0 to 0
    emu->x[0] = 0;

    inst.inst = raw_inst;
    inst.rd = (raw_inst >> 7) & 0x1f;
    inst.rs1 = (raw_inst >> 15) & 0x1f;
    inst.rs2 = (raw_inst >> 20) & 0x1f;
    inst.funct3 = (raw_inst >> 12) & 0x7;
    inst.funct7 = (raw_inst >> 25) & 0x7f;

#ifdef DEBUG
    printf("[%4lx] opcode: %2x, funct3: %x, funct7: %2x\n", emu->pc - 4, opcode,
           inst.funct3, inst.funct7);
#endif

    switch (opcode) {
    case 0x3:
        ret = opcode_3(emu, &inst);
        break;
    case 0x13:
        ret = opcode_13(emu, &inst);
        break;
    case 0x17:
        ret = opcode_17(emu, &inst);
        break;
    case 0x1b:
        ret = opcode_1b(emu, &inst);
        break;
    case 0x23:
        ret = opcode_23(emu, &inst);
        break;
    case 0x33:
        ret = opcode_33(emu, &inst);
        break;
    case 0x37:
        ret = opcode_37(emu, &inst);
        break;
    case 0x3b:
        ret = opcode_3b(emu, &inst);
        break;
    case 0x67:
        ret = opcode_67(emu, &inst);
        break;
    case 0x6f:
        ret = opcode_6f(emu, &inst);
        break;
    default:
        break;
    }

#ifdef DEBUG
    static char *abi_name[] = {
        "z",  "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
        "a1", "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
        "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};

    for (size_t i = 0; i < 32; i++) {
        printf("x%-2ld(%-3s) = 0x%-16lx, ", i, abi_name[i], emu->x[i]);
        if (!((i + 1) & 1))
            printf("\n");
    }
    printf("\n");
#endif

    if (ret != 0) {
        printf("Not implemented or invalid instruction@%llx\n",
               (unsigned long long) (emu->pc - 4));
        printf("opcode:%x, funct3:%x, funct7:%x\n", opcode, inst.funct3,
               inst.funct7);
    }

    return ret;
}

static void emu_init(struct emu *emu)
{
    memset(emu, 0, sizeof(struct emu));
    emu->pc = 0;
    emu->x[2] = TOHOST_ADDR;
    emu->bp_addr = -1;
    emu_halt(emu);
}

static int init_mem(struct mem *m, const char *filename)
{
    if (!filename) {
        return -1;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp)
        return -1;

    fseek(fp, 0, SEEK_END);
    size_t sz = ftell(fp) * sizeof(uint8_t);
    rewind(fp);

    /* We leave extra four bytes as the hint stop the emulator, so
     * the size of the binary should not exceed this. */
    if (sz > TOHOST_ADDR)
        return -1;

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
    return REGSZ;
}

static int emu_read_reg(void *args, int regno, void *reg_value)
{
    struct emu *emu = (struct emu *) args;
    if (regno > 32) {
        return EFAULT;
    }

    if (regno == 32) {
        memcpy(reg_value, &emu->pc, REGSZ);
    } else {
        memcpy(reg_value, &emu->x[regno], REGSZ);
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
        memcpy(&emu->pc, data, REGSZ);
    } else {
        memcpy(&emu->x[regno], data, REGSZ);
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
    int ret;
    struct emu *emu = (struct emu *) args;
    uint8_t *tohost_addr = emu->m.mem + TOHOST_ADDR;

    emu_start_run(emu);
    while (emu->pc < emu->m.code_size && emu->pc != emu->bp_addr &&
           !emu_is_halt(emu)) {
        uint32_t inst;
        uint8_t value;
        emu_read_mem(args, emu->pc, 4, &inst);
        emu->pc += 4;
        ret = emu_exec(emu, inst);
        if (ret < 0)
            break;

        /* We assume the binary that run on this emulator will
         * be stopped after writing the specific memory address.
         * In this way, we can simply design our testing binary. */
        read_len(8, tohost_addr, value);
        if (value)
            return ACT_SHUTDOWN;
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
#ifdef RV32
                          .target_desc = TARGET_RV32,
#else
                          .target_desc = TARGET_RV64,
#endif
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
