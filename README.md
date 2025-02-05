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
abstraction of your emulator's operation. The following lists the requirement
that should be provided for each method:

Method         | Description
---------------|------------------
`cont`         | Run the emulator until hitting breakpoint or exit.
`stepi`        | Do one step on the emulator. You may define your own step for the emulator. For example, the common design is executing one instruction.
`read_reg`     | Read the value of the register specified by `regno` to `*value`. Return zero if the operation success, otherwise return an errno for the corresponding error.
`write_reg`    | Write value `value` to the register specified by `regno`. Return zero if the operation success, otherwise return an errno for the corresponding error.
`read_mem`     | Read the memory according to the address specified by `addr` with size `len` to the buffer `*val`. Return zero if the operation success, otherwise return an errno for the corresponding error.
`write_mem`    | Write data in the buffer `val` with size `len` to the memory which address is specified by `addr`. Return zero if the operation success, otherwise return an errno for the corresponding error.
`set_bp`       | Set type `type` breakpoint on the address specified by `addr`. Return true if we set the breakpoint successfully, otherwise return false.
`del_bp`       | Delete type `type` breakpoint on the address specified by `addr`. Return true if we delete the breakpoint successfully, otherwise return false.
`on_interrupt` | Do something when receiving interrupt from GDB client. This method will run concurrently with `cont`, so you should be careful if there're shared data between them. You will need a lock or something similar to avoid data race.

```cpp
struct target_ops {
    gdb_action_t (*cont)(void *args);
    gdb_action_t (*stepi)(void *args);
    int (*read_reg)(void *args, int regno, size_t *value);
    int (*write_reg)(void *args, int regno, size_t value);
    int (*read_mem)(void *args, size_t addr, size_t len, void *val);
    int (*write_mem)(void *args, size_t addr, size_t len, void *val);
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

For `set_bp` and `del_bp`, the type of breakpoint which should be set or deleted is described
in the type `bp_type_t`. In fact, its value will always be `BP_SOFTWARE` currently.

```cpp
typedef enum {
    BP_SOFTWARE = 0,
} bp_type_t;
```

Another structure you have to declare is `arch_info_t`. You must explicitly specify about the
following field within `arch_info_t` while integrating into your emulator:
* `smp`: Number of target's CPU
* `reg_byte`: Register's size in bytes
* `reg_num`: Number of target's registers

The `target_desc` is an optional member which could be
`TARGET_RV32` or `TARGET_RV64` if the emulator is RISC-V 32-bit or 64-t instruction set architecture.
Alternatively, it can be a custom Target Description document string used by gdb. 
If none of these apply, simply set it to NULL.

* Although the value of `reg_num` and `reg_byte` may be determined by `target_desc`, those
members are still required to be filled correctly.

```cpp
typedef struct {
    char *target_desc;
    int smp;
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
