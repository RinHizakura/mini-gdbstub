#!/usr/bin/env bash

set -euo pipefail

source "$(dirname "$0")/common.sh"

# Configuration
ARCH="${ARCH:-rv64}"
GDB_PORT="${GDB_PORT:-1234}"
EMUDIR="build/emu"
TEST_NAME="emu_test"
TEST_OBJ="$EMUDIR/$TEST_NAME.obj"
TEST_BIN="$EMUDIR/$TEST_NAME.bin"

# Cleanup handler
cleanup_test()
{
    rm -f "$TMPFILE" "$TMPFILE.gdb"
    cleanup_emulator
}

# Determine GDB architecture string
get_gdb_arch()
{
    case "$ARCH" in
        rv32 | RV32) echo "riscv:rv32" ;;
        rv64 | RV64) echo "riscv:rv64" ;;
        *) echo "riscv:rv64" ;;
    esac
}

# Verify test prerequisites
check_prerequisites()
{
    test_start "Prerequisites check"

    if [[ ! -x "$EMUDIR/emu" ]]; then
        test_fail "Prerequisites" "Emulator not found: $EMUDIR/emu"
        print_info "Run: make build-emu (or make build-emu-rv32 for RV32)"
        return 1
    fi

    if [[ ! -f "$TEST_BIN" ]]; then
        test_fail "Prerequisites" "Test binary not found: $TEST_BIN"
        return 1
    fi

    if [[ ! -f "$TEST_OBJ" ]]; then
        test_fail "Prerequisites" "Test object not found: $TEST_OBJ"
        return 1
    fi

    test_pass "Prerequisites"
    return 0
}

run_gdb_test_script()
{
    TEST_SCRIPT=$1
    TESTCASE=$2

    test_start "Testcase: $TESTCASE ($ARCH)"

    # Find GDB
    local gdb
    if [[ -n "${RISCV_GDB:-}" ]]; then
        gdb="$RISCV_GDB"
    else
        gdb=$(find_riscv_gdb) || {
            test_fail $TESTCASE "No suitable GDB found"
            return 1
        }
    fi

    print_info "Using GDB: $gdb"
    print_info "Architecture: $gdb_arch"

    # Start emulator in background
    print_step "Starting emulator..."
    "$EMUDIR/emu" "$TEST_BIN" &
    local emu_pid=$!
    register_pid $emu_pid

    # Wait for emulator to be ready
    sleep 0.5
    if ! kill -0 $emu_pid 2> /dev/null; then
        test_fail $TESTCASE "Emulator failed to start"
        return 1
    fi

    # Run GDB with timeout to prevent hangs
    print_step "Running GDB session..."
    local gdb_timeout=30
    if ! timeout "$gdb_timeout" "$gdb" --batch -x $TEST_SCRIPT > "$TMPFILE" 2>&1; then
        local exit_code=$?
        # timeout returns 124 on timeout
        if [[ $exit_code -eq 124 ]]; then
            test_fail $TESTCASE "GDB session timed out after ${gdb_timeout}s"
            return 1
        fi
        # GDB may return non-zero on remote close, check output
        if ! grep -q "Remote connection closed\|Remote communication error" "$TMPFILE"; then
            test_fail $TESTCASE "GDB session failed"
            print_error "GDB output:"
            cat "$TMPFILE" >&2
            return 1
        fi
    fi

    # Verify test completed
    wait $emu_pid 2> /dev/null || true
}

run_prerequisites_test()
{
    print_header $TESTCASE
    print_info "Architecture: $ARCH"
    print_info "Emulator: $EMUDIR/emu"

    check_prerequisites
}
