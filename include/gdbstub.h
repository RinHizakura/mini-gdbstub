#ifndef GDBSTUB_H
#define GDBSTUB_H

#include <stdbool.h>
#include <stddef.h>

#define TARGET_RV32 \
    "<target version=\"1.0\"><architecture>riscv:rv32</architecture></target>"
#define TARGET_RV64 \
    "<target version=\"1.0\"><architecture>riscv:rv64</architecture></target>"
#define TARGET_X86_64 \
    "<target "        \
    "version=\"1.0\"><architecture>i386:x86-64</architecture></target>"

typedef enum {
    EVENT_NONE,
    EVENT_CONT,
    EVENT_DETACH,
    EVENT_STEP,
} gdb_event_t;

typedef enum {
    ACT_NONE,
    ACT_RESUME,
    ACT_SHUTDOWN,
} gdb_action_t;

/**
 * Breakpoint/watchpoint types corresponding to GDB Z-packets.
 *
 * For breakpoints (BP_SOFTWARE, BP_HARDWARE):
 *   - 'len' parameter is the instruction size in bytes (architecture-specific)
 *   - Typically 2 for compressed instructions, 4 for standard RISC-V
 *
 * For watchpoints (WP_WRITE, WP_READ, WP_ACCESS):
 *   - 'len' parameter is the memory region size in bytes to watch
 *   - Must be validated by implementation (typical max: 8 bytes)
 */
typedef enum {
    BP_SOFTWARE = 0, /* Z0: Software breakpoint */
    BP_HARDWARE = 1, /* Z1: Hardware breakpoint */
    WP_WRITE = 2,    /* Z2: Write watchpoint */
    WP_READ = 3,     /* Z3: Read watchpoint */
    WP_ACCESS = 4,   /* Z4: Access watchpoint (read/write) */
} bp_type_t;

/**
 * Target operations interface.
 *
 * Implementations must validate all address/length parameters and return
 * appropriate error codes for invalid ranges. Memory operations should
 * return 0 on success, or a positive errno value on failure.
 */
struct target_ops {
    gdb_action_t (*cont)(void *args);
    gdb_action_t (*stepi)(void *args);
    size_t (*get_reg_bytes)(int regno);
    int (*read_reg)(void *args, int regno, void *value);
    int (*write_reg)(void *args, int regno, const void *value);
    int (*read_mem)(void *args, size_t addr, size_t len, void *val);
    int (*write_mem)(void *args, size_t addr, size_t len, const void *val);

    /**
     * Set a breakpoint or watchpoint.
     * @param addr Target address
     * @param len  For BP_*: instruction size; for WP_*: memory region size
     * @param type Breakpoint/watchpoint type
     * @return true on success, false on failure (e.g., no hw resources)
     */
    bool (*set_bp)(void *args, size_t addr, size_t len, bp_type_t type);

    /**
     * Delete a breakpoint or watchpoint.
     * @param addr Target address (must match set_bp call)
     * @param len  Must match the len used in set_bp
     * @param type Breakpoint/watchpoint type
     * @return true on success, false on failure
     */
    bool (*del_bp)(void *args, size_t addr, size_t len, bp_type_t type);

    void (*on_interrupt)(void *args);

    void (*set_cpu)(void *args, int cpuid);
    int (*get_cpu)(void *args);
};

typedef struct gdbstub_private gdbstub_private_t;

typedef struct {
    const char *target_desc; /* XML target description (may be NULL) */
    int smp;                 /* Number of CPUs (0 or 1 for single-core) */
    int reg_num;             /* Number of registers */
} arch_info_t;

typedef struct {
    struct target_ops *ops;
    arch_info_t arch;
    gdbstub_private_t *priv;
} gdbstub_t;

bool gdbstub_init(gdbstub_t *gdbstub,
                  struct target_ops *ops,
                  arch_info_t arch,
                  const char *s);
bool gdbstub_run(gdbstub_t *gdbstub, void *args);
void gdbstub_close(gdbstub_t *gdbstub);

#endif
