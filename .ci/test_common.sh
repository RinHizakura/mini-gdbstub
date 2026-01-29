#!/usr/bin/env bash

set -euo pipefail

source "$(dirname "$0")/common.sh"

# Configuration
ARCH="${ARCH:-rv64}"

# Create secure temporary file with optional test name suffix.
# Usage: TMPFILE=$(create_temp_file "testname")
create_temp_file()
{
    local name="${1:-gdbstub_test}"
    mktemp "${TMPDIR:-/tmp}/${name}.XXXXXX"
}

# Generate common GDB script header (preamble).
# Usage: gdb_script_header >> "$script_file"
gdb_script_header()
{
    local gdb_arch
    gdb_arch=$(get_gdb_arch)
    cat << EOF
set pagination off
set confirm off
set architecture $gdb_arch
file "$TEST_OBJ"
target remote :$GDB_PORT
EOF
}

# Initialize test environment: set gdb_arch and register cleanup.
# Usage: init_test [cleanup_function]
init_test()
{
    local cleanup_func="${1:-cleanup_test}"
    gdb_arch=$(get_gdb_arch)
    register_cleanup "$cleanup_func"
}
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
    local test_script="$1"
    local testcase="$2"

    test_start "Testcase: $testcase ($ARCH)"

    # Find GDB
    local gdb
    if [[ -n "${RISCV_GDB:-}" ]]; then
        gdb="$RISCV_GDB"
    else
        gdb=$(find_riscv_gdb) || {
            test_fail "$testcase" "No suitable GDB found"
            return 1
        }
    fi

    print_info "Using GDB: $gdb"
    print_info "Architecture: ${gdb_arch:-$(get_gdb_arch)}"

    # Start emulator in background
    print_step "Starting emulator..."
    "$EMUDIR/emu" "$TEST_BIN" &
    local emu_pid=$!
    register_pid $emu_pid

    # Wait for emulator to be ready
    sleep 0.5
    if ! kill -0 $emu_pid 2> /dev/null; then
        test_fail "$testcase" "Emulator failed to start"
        return 1
    fi

    # Run GDB with timeout to prevent hangs
    print_step "Running GDB session..."
    local gdb_timeout=30
    if ! timeout "$gdb_timeout" "$gdb" --batch -x "$test_script" > "$TMPFILE" 2>&1; then
        local exit_code=$?
        # timeout returns 124 on timeout
        if [[ $exit_code -eq 124 ]]; then
            test_fail "$testcase" "GDB session timed out after ${gdb_timeout}s"
            return 1
        fi
        # GDB may return non-zero on remote close, check output
        if ! grep -q "Remote connection closed\|Remote communication error" "$TMPFILE"; then
            test_fail "$testcase" "GDB session failed"
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
    print_header "$TESTCASE"
    print_info "Architecture: $ARCH"
    print_info "Emulator: $EMUDIR/emu"

    check_prerequisites
}

# Expected register count: x0..x31 + pc = 33
REG_NUM=33

# Register hex width per architecture
get_reg_hex_len()
{
    case "$ARCH" in
        rv32 | RV32) echo 8 ;;  # 4 bytes = 8 hex chars
        rv64 | RV64) echo 16 ;; # 8 bytes = 16 hex chars
        *) echo 16 ;;
    esac
}

# Extract register hex blob from maintenance packet output.
# Extracts the quoted content from 'received: "..."' lines, then filters
# for pure hex strings of at least 64 characters (smallest valid register
# dump: 8 regs * 4 bytes * 2 hex = 64). This avoids matching short
# protocol responses (OK, E22, qSupported, etc.).
extract_reg_blob()
{
    local file="$1"
    local occurrence="${2:-1}"
    grep 'received: "' "$file" \
        | sed 's/.*received: "\([^"]*\)".*/\1/' \
        | grep -iE '^[0-9a-f]{64,}$' \
        | sed -n "${occurrence}p"
}

# Normalize hex string to lowercase for portable comparison.
hex_lower()
{
    tr '[:upper:]' '[:lower:]'
}

# Kill any emulator still running from a previous test within this script.
# The emulator accepts exactly one connection (single-shot), so we must
# ensure the previous instance is fully gone before starting a new one.
kill_prev_emulator()
{
    for pid in "${CLEANUP_PIDS[@]:-}"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2> /dev/null; then
            kill "$pid" 2> /dev/null || true
            wait "$pid" 2> /dev/null || true
        fi
    done
    # Clear the list so we don't attempt to kill stale or reused PIDs.
    CLEANUP_PIDS=()
    # Brief pause for the OS to release the port (SO_REUSEADDR helps but
    # the TIME_WAIT state can still cause transient bind failures).
    sleep 0.3
}
