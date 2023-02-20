CFLAGS = -I../include -Wall -Wextra

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OUT ?= build
BIN = $(OUT)/emu
SHELL_HACK := $(shell mkdir -p $(OUT))

CSRCS = $(shell find ./src -name '*.c')
_COBJ =  $(notdir $(CSRCS))
COBJ = $(_COBJ:%.c=$(OUT)/%.o)

LIBGDBSTUB = ../build/libgdbstub.a

vpath %.c $(sort $(dir $(CSRCS)))

all: clean $(BIN)

$(OUT)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(BIN): $(COBJ) $(LIBGDBSTUB)
	$(CC) $^ -o $@ $(LDFLAGS)

clean:
	$(RM) $(COBJ)
	$(RM) $(BIN)
