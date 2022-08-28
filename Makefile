CFLAGS = -Iinclude -Wall -Wextra -MMD -Werror
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
	$(CC) -shared $(LIB_OBJ) -o $@

test: $(LIBGDBSTUB)
	$(MAKE) -C emu

clean:
	$(RM) $(LIB_OBJ)
	$(RM) $(LIBGDBSTUB)
	$(RM) $(OUT)/*.d

-include $(OUT)/*.d
