# mini-gdbstub

[WIP] An implementation of the
[GDB Remote Serial Protocol](https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html#Remote-Protocol)
to help you adding debug mode on emulator

## Usage

First of all, you should build the library for the shared object file `libgdbstub.so`.
This command also give you a mock emulator to simply test the functionality.
```
make
```

Once the compiles are completed, you can run the mock emulator under
the debug mode.
```
$ ./build/emu
```

Then, you can open the GDB and iteract with the emulator by some commands!

**WARNING!! most of the commands are unavailable or bug :(**
```
$ riscv32-unknown-linux-gnu-gdb
(gdb) set debug remote 1
(gdb) target remote :1234
(gdb) info registers
(gdb) continue
```
