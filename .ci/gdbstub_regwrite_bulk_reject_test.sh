#!/usr/bin/env bash

# GDB Stub Bulk Register Reject Test
#
# Tests bulk register write rejection (G packet with wrong length -> E22).
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_regwrite_bulk_reject_test.sh
#   CROSS_COMPILE=riscv64-unknown-elf- .ci/gdbstub_regwrite_bulk_reject_test.sh
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   CROSS_COMPILE   Toolchain prefix (e.g., riscv64-unknown-elf-)
#   RISCV_GDB       Path to GDB executable (auto-detected if not set)
#   GDB_PORT        Port for GDB connection (default: 1234)
#

TESTCASE="GDB Bulk Register Reject Test"
source "$(dirname "$0")/test_common.sh"
TMPFILE=$(create_temp_file "gdbstub_regwrite_bulk_reject_test")

run_bulk_reg_reject_test()
{
    init_test

    # Create GDB command script
    gdb_script_header > "$TMPFILE.gdb"
    cat >> "$TMPFILE.gdb" << 'EOF'

# Stop at a known point
break add
continue

# Read all registers via 'g' packet (baseline)
maintenance packet g

# Send G packet with wrong length (should get E22)
maintenance packet G00

# Read registers again to confirm state unchanged
maintenance packet g

continue
quit
EOF

    run_gdb_test_script "$TMPFILE.gdb" "$TESTCASE"

    local test_ok=true

    # Wrong-length G packet should return an error (E22 = EINVAL)
    if grep -q 'received: "E' "$TMPFILE"; then
        local err_response
        err_response=$(grep 'received: "E' "$TMPFILE" | head -1 | sed 's/.*received: "\(E[0-9]*\)".*/\1/')
        print_info "Wrong-length G packet correctly rejected with $err_response"
    else
        print_error "Wrong-length G packet was not rejected"
        test_ok=false
    fi

    # Register state must be unchanged (first g read == second g read)
    local read1 read2
    read1=$(extract_reg_blob "$TMPFILE" 1 | hex_lower)
    read2=$(extract_reg_blob "$TMPFILE" 2 | hex_lower)

    if [[ -n "$read1" ]] && [[ -n "$read2" ]]; then
        if [[ "$read1" == "$read2" ]]; then
            print_info "Register state preserved after rejected G packet"
        else
            print_error "Register state changed after rejected G packet"
            test_ok=false
        fi
    else
        print_warning "Could not parse register reads for comparison"
        test_ok=false
    fi

    if [[ "$test_ok" == "true" ]]; then
        test_pass "$TESTCASE ($ARCH)"
        return 0
    else
        test_fail "$TESTCASE" "See errors above"
        print_error "Full GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

run_prerequisites_test || exit 1
run_bulk_reg_reject_test || exit 1
print_test_summary
