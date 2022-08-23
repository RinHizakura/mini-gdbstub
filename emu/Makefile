CFLAGS = -I../include -Wall -Wextra
LDFLAGS = -Wl,-rpath="$(CURDIR)/../" -L.. -ldl -lgdbstub

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OUT ?= build
BIN = $(OUT)/emu
SHELL_HACK := $(shell mkdir -p $(OUT))

CSRCS = $(shell find ./src -name '*.c')
_COBJ =  $(notdir $(CSRCS))
COBJ = $(_COBJ:%.c=$(OUT)/%.o)

TEST_OBJ = test.obj
TEST_BIN = test.bin

vpath %.c $(sort $(dir $(CSRCS)))

all: clean $(COBJ) $(BIN) $(TEST_BIN)

$(OUT)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(BIN): $(COBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

$(TEST_OBJ):
	riscv32-unknown-elf-gcc -march=rv32g -Wl,-Ttext=0x0 -nostdlib -g -o $(TEST_OBJ) ./test/test.c

$(TEST_BIN): $(TEST_OBJ)
	riscv32-unknown-elf-objcopy -O binary test.obj $(TEST_BIN)

clean:
	$(RM) $(COBJ)
	$(RM) $(BIN)
	$(RM) $(TEST_BIN)
	$(RM) $(TEST_OBJ)