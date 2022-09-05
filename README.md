# mini-gdbstub

[WIP] `mini-gdbstub` is an implementation of the
[GDB Remote Serial Protocol](https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html)
to help you intergrate debugging feature to the emulator.

## Usage

The very first thing you should do is to build the statically-linked library `libgdbstub.a`.
```
make
```

To use the library in your project, you should include the file `gdbstub.h` first.
Then, you have to initialize the pre-allocated structure `gdbstub_t` with `gdbstub_init`.

```cpp
bool gdbstub_init(gdbstub_t *gdbstub, struct target_ops *ops, arch_info_t arch, char *s);
```

The parameters `s` is the easiest one to understand. It is a string of the socket
which your emulator would like to bind as gdb server.

The `struct target_ops` is a structure of function pointers. Each member function represents an
abstraction of your emulator's operation. For example, `mini-gdbstub` will use `read_mem` to
read the memory or use `set_bp` to set breakpoint on emulator.

```cpp
struct target_ops {
    gdb_action_t (*cont)(void *args);
    gdb_action_t (*stepi)(void *args);
    size_t (*read_reg)(void *args, int regno);
    void (*read_mem)(void *args, size_t addr, size_t len, void *val);
    void (*write_mem)(void *args, size_t addr, size_t len, void *val);
    bool (*set_bp)(void *args, size_t addr, bp_type_t type);
    bool (*del_bp)(void *args, size_t addr, bp_type_t type);
};
```

For `cont` and `stepi` which are used to process the execution of emulator, their return type
should be `gdb_action_t`. You should return `ACT_RESUME` if we are going to keep on the
debugging after the corresponding operation, otherwise you can return `ACT_SHUTDOWN` to end
up the debugging process. `ACT_NONE` is usually used by the library to do no action.

```cpp
typedef enum {
    ACT_NONE,
    ACT_RESUME,
    ACT_SHUTDOWN,
} gdb_action_t;
```

Another structure you have to declare is `arch_info_t`. In this structure, you are required
to tell `mini-gdbstub` the register byte size `reg_byte` and the number of registers `reg_num`
of your emulator directly. The `target_desc` is an optional member which could be
`TARGET_RV32` or `TARGET_RV64` if the emulator is RISC-V32 or RISC-V64 architecture, otherwise
you can just simply set it to `NULL`.
* Although the value of `reg_num` and `reg_byte` may be known by `target_desc`, those
two member are still required to be filled correctly

```cpp
typedef struct {
    char *target_desc;
    int reg_num;
    size_t reg_byte;
} arch_info_t;
```

We can use `gdbstub_run` to run the emulator as gdbstub after the initialization. The `args`
can be used to pass the argument to any function in `struct target_ops`.

```cpp
bool gdbstub_run(gdbstub_t *gdbstub, void *args);
```

When exiting from `gdbstub_run`, `gdbstub_close` should be called to recycle the resource on
the initialization.

```cpp
void gdbstub_close(gdbstub_t *gdbstub);
```

Finally, you can build you project with the statically-linked library `libgdbstub.a` now!
You are also recommanded to reference to the example in the directory `emu`, which is a simple
emulator that shows you how to intergrate `mini-gdbstub` in your project.

## Reference
### Project
* [bet4it/gdbserver](https://github.com/bet4it/gdbserver)
* [devbored/minigdbstub](https://github.com/devbored/minigdbstub)
### Article
* [Howto: GDB Remote Serial Protocol](https://www.embecosm.com/appnotes/ean4/embecosm-howto-rsp-server-ean4-issue-2.html)
* [TLMBoy: Implementing the GDB Remote Serial Protocol](https://www.chciken.com/tlmboy/2022/04/03/gdb-z80.html)
