# mini-gdbstub

`mini-gdbstub` is an implementation of the
[GDB Remote Serial Protocol](https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html)
that gives your emulators debugging capabilities.

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

The `struct target_ops` is made up of function pointers. Each member function represents an
abstraction of your emulator's operation. For example, `mini-gdbstub` will use `read_mem` to
read the memory or use `set_bp` to set breakpoint on emulator.

```cpp
struct target_ops {
    gdb_action_t (*cont)(void *args);
    gdb_action_t (*stepi)(void *args);
    size_t (*read_reg)(void *args, int regno);
    void (*write_reg)(void *args, int regno, size_t value);
    void (*read_mem)(void *args, size_t addr, size_t len, void *val);
    void (*write_mem)(void *args, size_t addr, size_t len, void *val);
    bool (*set_bp)(void *args, size_t addr, bp_type_t type);
    bool (*del_bp)(void *args, size_t addr, bp_type_t type);
    void (*on_interrupt)(void *args);
};
```

For `cont` and `stepi` which are used to process the execution of emulator, their return type
should be `gdb_action_t`. After performing the relevant operation, you should return `ACT_RESUME`
to continue debugging; otherwise, return `ACT_SHUTDOWN` to finish debugging. The library
typically uses `ACT_NONE` to take no action.

```cpp
typedef enum {
    ACT_NONE,
    ACT_RESUME,
    ACT_SHUTDOWN,
} gdb_action_t;
```

Another structure you have to declare is `arch_info_t`. You must explicitly specify "mini-gdbstub"
about size in bytes (`reg_byte`) and the number of target registers (`reg_num`) within `arch_info_t`
structure while integrating into your emulator. The `target_desc` is an optional member which could be
`TARGET_RV32` or `TARGET_RV64` if the emulator is RISC-V 32-bit or 64-t instruction set architecture,
otherwise you would simply set it to `NULL`.
* Although the value of `reg_num` and `reg_byte` may be determined by `target_desc`, those
members are still required to be filled correctly.

```cpp
typedef struct {
    char *target_desc;
    int reg_num;
    size_t reg_byte;
} arch_info_t;
```

After startup, we can use `gdbstub_run` to run the emulator as gdbstub. The `args`
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
Additionally, it is advised that you check the reference emulator in the directory `emu,` which
demonstrates how to integrate `mini-gdbstub` into your project.

## Reference
### Project
* [bet4it/gdbserver](https://github.com/bet4it/gdbserver)
* [devbored/minigdbstub](https://github.com/devbored/minigdbstub)
### Article
* [Howto: GDB Remote Serial Protocol](https://www.embecosm.com/appnotes/ean4/embecosm-howto-rsp-server-ean4-issue-2.html)
* [TLMBoy: Implementing the GDB Remote Serial Protocol](https://www.chciken.com/tlmboy/2022/04/03/gdb-z80.html)
