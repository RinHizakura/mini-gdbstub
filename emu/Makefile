CFLAGS = -I../include -Wall -Wextra
LDFLAGS = -Wl,-rpath="$(CURDIR)/../" -L.. -ldl -lgdbstub

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OUT ?= build
BIN = emu
LIBGDBSTUB = libgdbstub.so
SHELL_HACK := $(shell mkdir -p $(OUT))

CSRCS = $(shell find ./src -name '*.c')
_COBJ =  $(notdir $(CSRCS))
COBJ = $(_COBJ:%.c=$(OUT)/%.o)

vpath %.c $(sort $(dir $(CSRCS)))

all: $(COBJ) $(BIN)

$(OUT)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(BIN): $(COBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

check: $(BIN)
	 riscv32-unknown-elf-gcc -Wl,-Ttext=0x0 -nostdlib -o test.obj ./test/test.s
	 riscv32-unknown-elf-objcopy -O binary test.obj test.bin

clean:
	$(RM) $(COBJ)
	$(RM) $(BIN)
