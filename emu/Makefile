CFLAGS = -I../include -Wall -Wextra
ifdef RV32
CFLAGS += -DRV32
endif
LDFLAGS =

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

O ?= build
OUT := $(O)

BIN = $(OUT)/emu
SHELL_HACK := $(shell mkdir -p $(OUT))

CSRCS = $(shell find ./src -name '*.c')
_COBJ =  $(notdir $(CSRCS))
COBJ = $(_COBJ:%.c=$(OUT)/%.o)

TEST_NAME = emu_test
TEST_OBJ = $(OUT)/$(TEST_NAME).obj
TEST_BIN = $(OUT)/$(TEST_NAME).bin

LIBGDBSTUB = ../build/libgdbstub.a

vpath %.c $(sort $(dir $(CSRCS)))
.PHONY: all clean

all: $(BIN) $(TEST_BIN)

$(OUT)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

RISCV_CC ?= riscv64-unknown-elf-gcc
RISCV_OBJCOPY ?= riscv64-unknown-elf-objcopy
ifdef RV32
RISCV_ARCH ?= rv32im
RISCV_ABI ?= ilp32
else
RISCV_ARCH ?= rv64im
RISCV_ABI ?= lp64
endif

$(TEST_OBJ): tests/emu_test.c
	$(RISCV_CC) -march=$(RISCV_ARCH) -mabi=$(RISCV_ABI) -Wl,-Ttext=0x0 -nostdlib -g -o $@ $<

$(TEST_BIN): $(TEST_OBJ)
	$(RISCV_OBJCOPY) -O binary $< $@

$(BIN): $(COBJ) $(LIBGDBSTUB)
	$(CC) $^ -o $@ $(LDFLAGS)

clean:
	$(RM) $(TEST_OBJ)
	$(RM) $(TEST_BIN)
	$(RM) $(COBJ)
	$(RM) $(BIN)
