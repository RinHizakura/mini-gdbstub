# mini-gdbstub

[WIP] An implementation of the
[GDB Remote Serial Protocol](https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html)
to help you adding debug mode on emulator

## Usage

First of all, you should build the library for the shared object file `libgdbstub.so`.
```
make
```

If you want a simple example to test the library, we give you a mock emulator to
simply play with it.
```
make test
```

Once the compiles are completed, you can run the mock emulator under
the debug mode.
```
$ ./emu/build/emu <binary file>
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

## Reference
### Project
* [bet4it/gdbserver](https://github.com/bet4it/gdbserver)
* [devbored/minigdbstub](https://github.com/devbored/minigdbstub)
### Article
* [Howto: GDB Remote Serial Protocol](https://www.embecosm.com/appnotes/ean4/embecosm-howto-rsp-server-ean4-issue-2.html)
* [TLMBoy: Implementing the GDB Remote Serial Protocol](https://www.chciken.com/tlmboy/2022/04/03/gdb-z80.html)
