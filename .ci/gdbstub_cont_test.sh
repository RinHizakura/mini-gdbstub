#!/usr/bin/env bash

set -e

GDB=riscv64-unknown-elf-gdb
EMUDIR=build/emu
TEST_NAME=emu_test
TEST_OBJ=$EMUDIR/$TEST_NAME.obj
TEST_BIN=$EMUDIR/$TEST_NAME.bin

echo $EMUDIR/emu $TEST_BIN
$EMUDIR/emu $TEST_BIN &
PID=$!

# Ensure emulator is still running.
if ! ps -p $PID > /dev/null; then
    echo "Fail to start emulator"
    exit 1
fi

OPTS=
tmpfile=/tmp/emu_test
OPTS+="-ex 'file $TEST_OBJ' "
OPTS+="-ex 'target remote :1234' "
OPTS+="-ex 'p \$pc' "
OPTS+="-ex 'continue' "

echo ${GDB} --batch ${OPTS}
eval "${GDB} --batch ${OPTS}" > ${tmpfile}

# Pass and wait
wait $PID
exit 0
