CFLAGS = -Iinclude -Wall -Wextra -MMD #-Werror

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

O ?= build
OUT := $(O)
# One output directory per architecture: the emulator objects depend on
# ARCH through -DRV32, so switching ARCH must not reuse them
EMU_OUT = $(abspath $(OUT)/emu/$(ARCH))

LIBGDBSTUB = $(OUT)/libgdbstub.a
SHELL_HACK := $(shell mkdir -p $(OUT))
GIT_HOOKS := .git/hooks/applied

LIBSRCS = $(shell find ./src -name '*.c')
_LIB_OBJ =  $(notdir $(LIBSRCS))
LIB_OBJ = $(_LIB_OBJ:%.c=$(OUT)/%.o)

TEST_OBJ = $(EMU_OUT)/emu_test.obj
TEST_BIN = $(EMU_OUT)/emu_test.bin

vpath %.c $(sort $(dir $(LIBSRCS)))
.PHONY: all debug clean check

all: CFLAGS += -O3
all: LDFLAGS += -O3
all: $(LIBGDBSTUB)

debug: CFLAGS += -O3 -g -DDEBUG
debug: LDFLAGS += -O3
debug: $(LIBGDBSTUB)

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo

$(OUT)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(LIBGDBSTUB): $(LIB_OBJ)
	$(AR) -rcs $@ $^


# Architecture selection: ARCH=rv32 or ARCH=rv64 (default)
ARCH ?= rv64

# Validate ARCH value
ifneq ($(ARCH),rv32)
ifneq ($(ARCH),rv64)
$(error Invalid ARCH=$(ARCH). Supported values: rv32, rv64)
endif
endif

# CROSS_COMPILE prefix (Linux kernel style)
ifeq ($(ARCH),rv32)
CROSS_COMPILE ?= riscv32-unknown-elf-
EMU_RV32 := 1
else
CROSS_COMPILE ?= riscv64-unknown-elf-
EMU_RV32 :=
endif

RISCV_CC := $(CROSS_COMPILE)gcc
RISCV_OBJCOPY := $(CROSS_COMPILE)objcopy
RISCV_GDB := $(CROSS_COMPILE)gdb

build-emu: $(LIBGDBSTUB)
	$(MAKE) -C emu O=$(EMU_OUT) $(if $(EMU_RV32),RV32=1) \
		RISCV_CC=$(RISCV_CC) RISCV_OBJCOPY=$(RISCV_OBJCOPY)

run-gdbstub: build-emu
	$(EMU_OUT)/emu $(TEST_BIN)

GDBSTUB_COMM = 127.0.0.1:1234
run-gdb: build-emu
	$(RISCV_GDB)                                \
		-ex "file $(TEST_OBJ)"              \
		-ex "set debug remote 1"            \
		-ex "target remote $(GDBSTUB_COMM)" \

TEST_PKTBUF = $(OUT)/test_pktbuf
$(TEST_PKTBUF): tests/test_pktbuf.c src/packet.c include/packet.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined \
		-Wl,--wrap=realloc,--wrap=calloc \
		tests/test_pktbuf.c src/packet.c -o $@

check: $(TEST_PKTBUF)
	$(TEST_PKTBUF)

clean:
	$(RM) -r $(OUT)/emu
	$(RM) $(LIB_OBJ)
	$(RM) $(LIBGDBSTUB) $(TEST_PKTBUF)
	$(RM) $(OUT)/*.d

-include $(OUT)/*.d
