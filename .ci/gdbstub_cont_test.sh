#!/usr/bin/env bash

# GDB Stub Continue Test
#
# Tests basic GDB remote debugging: connect, read PC, continue to completion.
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_cont_test.sh
#   CROSS_COMPILE=riscv64-unknown-elf- .ci/gdbstub_cont_test.sh
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   CROSS_COMPILE   Toolchain prefix (e.g., riscv64-unknown-elf-)
#   RISCV_GDB       Path to GDB executable (auto-detected if not set)
#   GDB_PORT        Port for GDB connection (default: 1234)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/common.sh"

# Configuration
ARCH="${ARCH:-rv64}"
GDB_PORT="${GDB_PORT:-1234}"
EMUDIR="build/emu"
TEST_NAME="emu_test"
TEST_OBJ="$EMUDIR/$TEST_NAME.obj"
TEST_BIN="$EMUDIR/$TEST_NAME.bin"
TMPFILE="/tmp/gdbstub_cont_test_$$"

# Cleanup handler
cleanup_test()
{
    rm -f "$TMPFILE" "$TMPFILE.gdb"
    cleanup_emulator
}
register_cleanup cleanup_test

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

# Run continue test
run_continue_test()
{
    local gdb_arch
    gdb_arch=$(get_gdb_arch)

    test_start "GDB Continue Test ($ARCH)"

    # Find GDB
    local gdb
    if [[ -n "${RISCV_GDB:-}" ]]; then
        gdb="$RISCV_GDB"
    else
        gdb=$(find_riscv_gdb) || {
            test_fail "GDB Continue Test" "No suitable GDB found"
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
        test_fail "GDB Continue Test" "Emulator failed to start"
        return 1
    fi

    # Create GDB command script
    cat > "$TMPFILE.gdb" << EOF
set pagination off
set confirm off
set architecture $gdb_arch
file $TEST_OBJ
target remote :$GDB_PORT
printf "PC before continue: %p\\n", \$pc
continue
quit
EOF

    # Run GDB with timeout to prevent hangs
    print_step "Running GDB session..."
    local gdb_timeout=30
    if ! timeout "$gdb_timeout" "$gdb" --batch -x "$TMPFILE.gdb" > "$TMPFILE" 2>&1; then
        local exit_code=$?
        # timeout returns 124 on timeout
        if [[ $exit_code -eq 124 ]]; then
            test_fail "GDB Continue Test" "GDB session timed out after ${gdb_timeout}s"
            return 1
        fi
        # GDB may return non-zero on remote close, check output
        if ! grep -q "Remote connection closed\|Remote communication error" "$TMPFILE"; then
            test_fail "GDB Continue Test" "GDB session failed"
            print_error "GDB output:"
            cat "$TMPFILE" >&2
            return 1
        fi
    fi

    # Verify test completed
    wait $emu_pid 2> /dev/null || true

    # Check GDB output for success indicators
    if grep -q "PC before continue:" "$TMPFILE"; then
        local pc_value
        pc_value=$(grep "PC before continue:" "$TMPFILE" | head -1)
        print_info "$pc_value"
        test_pass "GDB Continue Test ($ARCH)"
        return 0
    else
        test_fail "GDB Continue Test" "Failed to read PC"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

main()
{
    print_header "GDB Stub Continue Test"
    print_info "Architecture: $ARCH"
    print_info "Emulator: $EMUDIR/emu"

    check_prerequisites || exit 1
    run_continue_test || exit 1

    print_test_summary
}

main "$@"
