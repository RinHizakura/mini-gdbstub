CFLAGS = -Iinclude -Wall -Wextra #-Werror
LDFLAGS = -Wl,-rpath="$(CURDIR)" -L. -ldl -lgdbstub

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OUT ?= build
LIBGDBSTUB = libgdbstub.so
SHELL_HACK := $(shell mkdir -p $(OUT))

GIT_HOOKS := .git/hooks/applied

LIBSRCS = $(shell find ./lib -name '*.c')
_LIB_OBJ =  $(notdir $(LIBSRCS))
LIB_OBJ = $(_LIB_OBJ:%.c=$(OUT)/%.o)

CSRCS = $(shell find ./emu -name '*.c')
_COBJ =  $(notdir $(CSRCS))
COBJ = $(_COBJ:%.c=$(OUT)/%.o)

TESTS = $(OUT)/emu

vpath %.c $(sort $(dir $(CSRCS)))
vpath %.c $(sort $(dir $(LIBSRCS)))

all: $(GIT_HOOKS) $(LIBGDBSTUB) $(TESTS)

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo

$(OUT)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(LIBGDBSTUB): $(LIB_OBJ)
	$(CC) -shared $(LIB_OBJ) -o $@

$(TESTS): %: %.o $(LIBGDBSTUB)
	$(CC) $^ -o $@ $(LDFLAGS)

test: all
	$(OUT)/emu

clean:
	$(RM) $(LIB_OBJ)
	$(RM) $(COBJ)
	$(RM) $(TESTS)
	$(RM) $(LIBGDBSTUB)

-include $(OUT)/*.d
