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

TESTCASE="GDB Continue Test"
source "$(dirname "$0")/test_common.sh"
TMPFILE=$(create_temp_file "gdbstub_cont_test")

run_continue_test()
{
    init_test

    # Create GDB command script
    gdb_script_header > "$TMPFILE.gdb"
    cat >> "$TMPFILE.gdb" << 'EOF'
printf "PC before continue: %p\n", $pc
continue
quit
EOF

    run_gdb_test_script "$TMPFILE.gdb" "$TESTCASE"

    # Check GDB output for success indicators
    if grep -q "PC before continue:" "$TMPFILE"; then
        local pc_value
        pc_value=$(grep "PC before continue:" "$TMPFILE" | head -1)
        print_info "$pc_value"
        test_pass "$TESTCASE ($ARCH)"
        return 0
    else
        test_fail "$TESTCASE" "Failed to read PC"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

run_prerequisites_test || exit 1
run_continue_test || exit 1
print_test_summary
