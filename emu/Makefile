CFLAGS = -I../include -Wall -Wextra
LDFLAGS =

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

O ?= build
OUT := $(O)

BIN = $(OUT)/emu
SHELL_HACK := $(shell mkdir -p $(OUT))

CSRCS = $(shell find ./src -name '*.c')
_COBJ =  $(notdir $(CSRCS))
COBJ = $(_COBJ:%.c=$(OUT)/%.o)

TEST_OBJ = $(OUT)/test.obj
TEST_BIN = $(OUT)/test.bin

LIBGDBSTUB = ../build/libgdbstub.a

vpath %.c $(sort $(dir $(CSRCS)))
.PHONY: all clean

all: $(BIN) $(TEST_BIN) $(TEST_BIN)

$(OUT)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(TEST_OBJ): tests/test.c
	riscv64-unknown-elf-gcc -march=rv64g -Wl,-Ttext=0x0 -nostdlib -g -o $@ $<

$(TEST_BIN): $(TEST_OBJ)
	riscv64-unknown-elf-objcopy -O binary $< $@

$(BIN): $(COBJ) $(LIBGDBSTUB)
	$(CC) $^ -o $@ $(LDFLAGS)

clean:
	$(RM) $(TEST_OBJ)
	$(RM) $(TEST_BIN)
	$(RM) $(COBJ)
	$(RM) $(BIN)
