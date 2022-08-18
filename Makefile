CFLAGS = -Iinclude -Wall -Wextra -MMD -DRISCV32_EMU #-DDEBUG -Werror
LDFLAGS = -Wl,-rpath="$(CURDIR)" -L. -lgdbstub

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OUT ?= build
LIBGDBSTUB = libgdbstub.so
SHELL_HACK := $(shell mkdir -p $(OUT))

GIT_HOOKS := .git/hooks/applied

LIBSRCS = $(shell find ./lib -name '*.c')
_LIB_OBJ =  $(notdir $(LIBSRCS))
LIB_OBJ = $(_LIB_OBJ:%.c=$(OUT)/%.o)

vpath %.c $(sort $(dir $(LIBSRCS)))
.PHONY: all test clean

all: $(GIT_HOOKS) $(LIBGDBSTUB)

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo

$(OUT)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(LIBGDBSTUB): $(LIB_OBJ)
	$(CC) -shared $(LIB_OBJ) -o $@

test: $(LIBGDBSTUB)
	$(MAKE) -C emu

clean:
	$(RM) $(LIB_OBJ)
	$(RM) $(LIBGDBSTUB)
	$(RM) $(OUT)/*.d

-include $(OUT)/*.d
