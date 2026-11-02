#!/usr/bin/env bash

# GDB Stub Hardware Breakpoint and Watchpoint Test
#
# Tests the Z1 and Z2 packets: set a hardware breakpoint on 'add' and hit
# it, then set a write watchpoint on the tohost address, continue, and
# verify GDB attributes the stop to the watchpoint by printing the old and
# new values.
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_wp_test.sh
#   CROSS_COMPILE=riscv64-unknown-elf- .ci/gdbstub_wp_test.sh
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   CROSS_COMPILE   Toolchain prefix (e.g., riscv64-unknown-elf-)
#   RISCV_GDB       Path to GDB executable (auto-detected if not set)
#   GDB_PORT        Port for GDB connection (default: 1234)
#

TESTCASE="GDB Hardware Breakpoint and Watchpoint Test"
source "$(dirname "$0")/test_common.sh"
TMPFILE=$(create_temp_file "gdbstub_wp_test")

run_watchpoint_test()
{
    init_test

    # Create GDB command script
    gdb_script_header > "$TMPFILE.gdb"
    cat >> "$TMPFILE.gdb" << 'EOF2'

# Hardware breakpoint on 'add' (Z1)
hbreak add
continue
printf "Hit hardware breakpoint, PC = %p\n", $pc

# Watch the tohost byte (0x1000 - 4) that main writes before exiting (Z2)
watch *(char *) 0xffc
printf "Watchpoint set\n"

# Continue to the watchpoint
continue
printf "Stopped at PC = %p\n", $pc

# Continue to completion
continue
quit
EOF2

    run_gdb_test_script "$TMPFILE.gdb" "$TESTCASE"

    local hwbreak_hit=false
    local watchpoint_set=false
    local watchpoint_hit=false

    if grep -q "Hardware assisted breakpoint [0-9]* at" "$TMPFILE" &&
        grep -qE "Breakpoint [0-9]+, add \(" "$TMPFILE"; then
        hwbreak_hit=true
        print_info "Hardware breakpoint was set and hit"
    fi

    if grep -q "Hardware watchpoint [0-9]*: \*(char \*) 0xffc" "$TMPFILE"; then
        watchpoint_set=true
        print_info "Hardware watchpoint was set successfully"
    fi

    # GDB prints Old/New value only when the stop reply names the watchpoint
    if grep -q "Old value = 0" "$TMPFILE" && grep -q "New value = 255" "$TMPFILE"; then
        watchpoint_hit=true
        print_info "Watchpoint hit reported with old and new values"
    fi

    if [[ "$hwbreak_hit" == "true" ]] && [[ "$watchpoint_set" == "true" ]] &&
        [[ "$watchpoint_hit" == "true" ]]; then
        test_pass "$TESTCASE ($ARCH)"
        return 0
    elif [[ "$hwbreak_hit" == "false" ]]; then
        test_fail "$TESTCASE" "Hardware breakpoint was not set or not hit"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    elif [[ "$watchpoint_set" == "false" ]]; then
        test_fail "$TESTCASE" "Hardware watchpoint was not set"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    else
        test_fail "$TESTCASE" "Watchpoint was set but the stop was not attributed to it"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

run_prerequisites_test || exit 1
run_watchpoint_test || exit 1
print_test_summary
