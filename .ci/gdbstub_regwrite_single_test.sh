#!/usr/bin/env bash

# GDB Stub Single Register Write Test
#
# Tests single register write (P packet) with read-back verification.
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_regwrite_single_test.sh
#   CROSS_COMPILE=riscv64-unknown-elf- .ci/gdbstub_regwrite_single_test.sh
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   CROSS_COMPILE   Toolchain prefix (e.g., riscv64-unknown-elf-)
#   RISCV_GDB       Path to GDB executable (auto-detected if not set)
#   GDB_PORT        Port for GDB connection (default: 1234)
#

TESTCASE="GDB Single Register Write Test"
source "$(dirname "$0")/test_common.sh"
TMPFILE=$(create_temp_file "gdbstub_regwrite_single_test")

run_single_reg_write_test()
{
    init_test

    # Create GDB command script
    gdb_script_header > "$TMPFILE.gdb"
    cat >> "$TMPFILE.gdb" << 'EOF'

# Stop at a known point
break add
continue

# Read initial t0 value
printf "BEFORE_T0=%lx\n", $t0

# Write a known value to t0 (register x5)
set $t0 = 0x42

# Read back t0
printf "AFTER_T0=%lx\n", $t0

# Continue to completion
continue
quit
EOF

    run_gdb_test_script "$TMPFILE.gdb" "$TESTCASE"

    if grep -q "AFTER_T0=42" "$TMPFILE"; then
        print_info "t0 written and read back: 0x42"
        test_pass "$TESTCASE ($ARCH)"
        return 0
    else
        test_fail "$TESTCASE" "t0 read-back mismatch"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

run_prerequisites_test || exit 1
run_single_reg_write_test || exit 1
print_test_summary
