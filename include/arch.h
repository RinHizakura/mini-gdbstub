#ifndef ARCH_H
#define ARCH_H

#ifdef RISCV32_EMU
/* - https://github.com/bminor/binutils-gdb/blob/master/gdb/riscv-tdep.h
 * - https://github.com/bminor/binutils-gdb/tree/master/gdb/features/riscv */
#define ARCH_REG_NUM (33)
#define TAEGET_DESC \
    "l<target "     \
    "version=\"1.0\"><architecture>riscv:rv32</architecture></target>"

#else
#error "not supported architecture config"
#endif

#endif
