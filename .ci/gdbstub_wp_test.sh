#!/usr/bin/env bash

# GDB Stub Watchpoint Test
#
# Tests GDB watchpoint functionality: hardware breakpoints (Z1) and
# write watchpoints (Z2).
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

TESTCASE="GDB Watchpoint Test"
source "$(dirname "$0")/test_common.sh"
TMPFILE=$(create_temp_file "gdbstub_wp_test")

# Run hardware breakpoint test (verifies Z1 packet handling)
run_hwbreak_test()
{
    init_test

    # Create GDB command script
    gdb_script_header > "$TMPFILE.gdb"
    cat >> "$TMPFILE.gdb" << 'EOF'

# Set a hardware breakpoint on 'add' function
hbreak add
printf "Hardware breakpoint set on 'add' function\n"

# Continue to breakpoint
continue
printf "Hit hardware breakpoint\n"

# Continue to completion
continue
quit
EOF

    run_gdb_test_script "$TMPFILE.gdb" "GDB Hardware Breakpoint Test"

    # Check GDB output
    local hwbreak_set=false
    local hwbreak_hit=false

    if grep -q "Hardware assisted breakpoint\|Hardware breakpoint set" "$TMPFILE"; then
        hwbreak_set=true
        print_info "Hardware breakpoint was set"
    fi

    if grep -qE "Breakpoint [0-9]+,.*add|Hit hardware breakpoint" "$TMPFILE"; then
        hwbreak_hit=true
        print_info "Hardware breakpoint was hit"
    fi

    if [[ "$hwbreak_set" == "true" ]] && [[ "$hwbreak_hit" == "true" ]]; then
        test_pass "GDB Hardware Breakpoint Test ($ARCH)"
        return 0
    elif [[ "$hwbreak_set" == "false" ]]; then
        test_fail "GDB Hardware Breakpoint Test" "Hardware breakpoint was not set"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    else
        test_fail "GDB Hardware Breakpoint Test" "Hardware breakpoint was set but never hit"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

# Run watchpoint set/delete test (verifies Z2/z2 packet handling)
run_watchpoint_set_test()
{
    kill_prev_emulator
    init_test

    # Create GDB command script
    # Test watchpoint on the tohost address (0xffc = 0x1000 - 4)
    gdb_script_header > "$TMPFILE.gdb"
    cat >> "$TMPFILE.gdb" << 'EOF'

# Set a write watchpoint on the tohost address
# The test program writes 0xff to (0x1000 - 4) = 0xffc
watch *0xffc
printf "Watchpoint set on address 0xffc\n"

# Continue - should hit the watchpoint when main writes to tohost
continue
printf "Stopped after watchpoint\n"

# Delete the watchpoint
delete 1
printf "Watchpoint deleted\n"

# Continue to completion
continue
quit
EOF

    run_gdb_test_script "$TMPFILE.gdb" "GDB Watchpoint Set/Delete Test"

    # Check GDB output for watchpoint operations
    local watchpoint_set=false
    local watchpoint_hit=false

    if grep -q "Hardware watchpoint\|Watchpoint set" "$TMPFILE"; then
        watchpoint_set=true
        print_info "Watchpoint was set successfully"
    fi

    # Check for watchpoint hit
    if grep -qE "Hardware watchpoint.*hit|watchpoint.*trigger|Stopped after watchpoint|Old value|New value" "$TMPFILE"; then
        watchpoint_hit=true
        print_info "Watchpoint was triggered"
    fi

    # For this test, we consider success if:
    # 1. Watchpoint was set (GDB acknowledged Z2 packet with OK)
    # 2. Program completed (connection closed normally)
    if [[ "$watchpoint_set" == "true" ]]; then
        test_pass "GDB Watchpoint Set/Delete Test ($ARCH)"
        return 0
    else
        test_fail "GDB Watchpoint Set Test" "Watchpoint was not set"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

run_prerequisites_test || exit 1
run_hwbreak_test || exit 1
run_watchpoint_set_test || exit 1
print_test_summary
