#!/usr/bin/env bash

# GDB Stub Breakpoint Test
#
# Tests GDB breakpoint functionality: set breakpoint, hit it, continue.
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_bp_test.sh
#   CROSS_COMPILE=riscv64-unknown-elf- .ci/gdbstub_bp_test.sh
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
TMPFILE="/tmp/gdbstub_bp_test_$$"

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

# Run breakpoint test
run_breakpoint_test()
{
    local gdb_arch
    gdb_arch=$(get_gdb_arch)

    test_start "GDB Breakpoint Test ($ARCH)"

    # Find GDB
    local gdb
    if [[ -n "${RISCV_GDB:-}" ]]; then
        gdb="$RISCV_GDB"
    else
        gdb=$(find_riscv_gdb) || {
            test_fail "GDB Breakpoint Test" "No suitable GDB found"
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
        test_fail "GDB Breakpoint Test" "Emulator failed to start"
        return 1
    fi

    # Create GDB command script
    cat > "$TMPFILE.gdb" << EOF
set pagination off
set confirm off
set architecture $gdb_arch
file $TEST_OBJ
target remote :$GDB_PORT

# Set breakpoint on 'add' function
break add
printf "Breakpoint set on 'add' function\\n"

# Continue to breakpoint
continue
printf "Hit breakpoint, PC = %p\\n", \$pc

# Continue to completion
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
            test_fail "GDB Breakpoint Test" "GDB session timed out after ${gdb_timeout}s"
            return 1
        fi
        # GDB may return non-zero on remote close, check output
        if ! grep -q "Remote connection closed\|Remote communication error" "$TMPFILE"; then
            test_fail "GDB Breakpoint Test" "GDB session failed"
            print_error "GDB output:"
            cat "$TMPFILE" >&2
            return 1
        fi
    fi

    # Verify test completed
    wait $emu_pid 2> /dev/null || true

    # Check GDB output for breakpoint set and hit
    local breakpoint_set=false
    local breakpoint_hit=false

    if grep -q "Breakpoint.*at" "$TMPFILE"; then
        breakpoint_set=true
        print_info "Breakpoint was set successfully"
    fi

    # Check for breakpoint hit (GDB prints "Breakpoint N, function...")
    if grep -qE "Breakpoint [0-9]+,.*add|Hit breakpoint" "$TMPFILE"; then
        breakpoint_hit=true
        local pc_at_bp
        pc_at_bp=$(grep -E "Hit breakpoint|Breakpoint [0-9]+," "$TMPFILE" | head -1) || true
        if [[ -n "$pc_at_bp" ]]; then
            print_info "$pc_at_bp"
        fi
    fi

    # Require BOTH breakpoint set AND hit for test to pass
    if [[ "$breakpoint_set" == "true" ]] && [[ "$breakpoint_hit" == "true" ]]; then
        test_pass "GDB Breakpoint Test ($ARCH)"
        return 0
    elif [[ "$breakpoint_set" == "false" ]]; then
        test_fail "GDB Breakpoint Test" "Breakpoint was not set"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    else
        test_fail "GDB Breakpoint Test" "Breakpoint was set but never hit"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

main()
{
    print_header "GDB Stub Breakpoint Test"
    print_info "Architecture: $ARCH"
    print_info "Emulator: $EMUDIR/emu"

    check_prerequisites || exit 1
    run_breakpoint_test || exit 1

    print_test_summary
}

main "$@"
