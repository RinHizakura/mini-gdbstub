CFLAGS = -Iinclude -Wall -Wextra -MMD #-Werror

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

O ?= build
ifeq ($(O), $(CURDIR)/build)
    OUT := $(CURDIR)
else
    OUT := $(O)
endif

LIBGDBSTUB = $(OUT)/libgdbstub.a
SHELL_HACK := $(shell mkdir -p $(OUT))
GIT_HOOKS := .git/hooks/applied

LIBSRCS = $(shell find ./lib -name '*.c')
_LIB_OBJ =  $(notdir $(LIBSRCS))
LIB_OBJ = $(_LIB_OBJ:%.c=$(OUT)/%.o)

TEST_OBJ = $(OUT)/test.obj
TEST_BIN = $(OUT)/test.bin

vpath %.c $(sort $(dir $(LIBSRCS)))
.PHONY: all debug test clean

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

$(TEST_OBJ): tests/test.c
	riscv32-unknown-elf-gcc -march=rv32g -Wl,-Ttext=0x0 -nostdlib -g -o $@ $<

$(TEST_BIN): $(TEST_OBJ)
	riscv32-unknown-elf-objcopy -O binary $< $@

build-emu: $(LIBGDBSTUB)
	$(MAKE) -C emu

run-gdbstub: $(TEST_BIN) build-emu
	emu/build/emu $(TEST_BIN)

GDBSTUB_COMM = 127.0.0.1:1234
run-gdb: $(TEST_OBJ)
	riscv32-unknown-elf-gdb                     \
		-ex "file $(TEST_OBJ)"              \
		-ex "set debug remote 1"            \
		-ex "target remote $(GDBSTUB_COMM)" \

clean:
	$(RM) $(LIB_OBJ)
	$(RM) $(LIBGDBSTUB)
	$(RM) $(TEST_BIN)
	$(RM) $(TEST_OBJ)
	$(RM) $(OUT)/*.d

-include $(OUT)/*.d
