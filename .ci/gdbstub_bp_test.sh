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

TESTCASE="GDB Breakpoint Test"
source "$(dirname "$0")/test_common.sh"
TMPFILE=$(create_temp_file "gdbstub_bp_test")

run_breakpoint_test()
{
    init_test

    # Create GDB command script
    gdb_script_header > "$TMPFILE.gdb"
    cat >> "$TMPFILE.gdb" << 'EOF'

# Set breakpoint on 'add' function
break add
printf "Breakpoint set on 'add' function\n"

# Continue to breakpoint
continue
printf "Hit breakpoint, PC = %p\n", $pc

# Continue to completion
continue
quit
EOF

    run_gdb_test_script "$TMPFILE.gdb" "$TESTCASE"

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
        test_pass "$TESTCASE ($ARCH)"
        return 0
    elif [[ "$breakpoint_set" == "false" ]]; then
        test_fail "$TESTCASE" "Breakpoint was not set"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    else
        test_fail "$TESTCASE" "Breakpoint was set but never hit"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

run_prerequisites_test || exit 1
run_breakpoint_test || exit 1
print_test_summary
