#!/usr/bin/env bash

# GDB Stub Detach Test
#
# Tests that the stub properly responds to detach command:
# - Verifies detach command completes successfully
# - Per RSP spec, stub should send OK before closing connection
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_detach_test.sh
#   CROSS_COMPILE=riscv64-unknown-elf- .ci/gdbstub_detach_test.sh
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   CROSS_COMPILE   Toolchain prefix (e.g., riscv64-unknown-elf-)
#   RISCV_GDB       Path to GDB executable (auto-detected if not set)
#   GDB_PORT        Port for GDB connection (default: 1234)
#

TESTCASE="GDB Detach Test"
source "$(dirname "$0")/test_common.sh"
TMPFILE=$(create_temp_file "gdbstub_detach_test")

run_detach_test()
{
    init_test

    # Create GDB command script
    gdb_script_header > "$TMPFILE.gdb"
    cat >> "$TMPFILE.gdb" << 'EOF'

# Enable debug logging to verify D packet and OK response
set debug remote 1

# Read PC to verify connection works
printf "Connected, PC = %p\n", $pc

# Detach from target - should complete cleanly
# GDB sends 'D' packet, stub should reply 'OK' before shutdown
detach
printf "Detach completed\n"

quit
EOF

    run_gdb_test_script "$TMPFILE.gdb" "$TESTCASE"

    # Check that connection was established
    local connected=false
    local detach_packet_sent=false
    local detach_ok_received=false
    local detach_clean=false

    if grep -q "Connected, PC = " "$TMPFILE"; then
        connected=true
        print_info "Connection established successfully"
    fi

    # With 'set debug remote 1', verify D packet was sent
    # GDB logs: Sending packet: $D#44
    if grep -qE 'Sending packet:.*\$D#' "$TMPFILE"; then
        detach_packet_sent=true
        print_info "D (detach) packet sent"
    fi

    # Verify stub responded with OK to the D packet
    # Check for OK response after D packet in sequence
    if [[ "$detach_packet_sent" == "true" ]]; then
        if awk '/Sending packet:.*\$D#/,/Packet received:/ {print}' "$TMPFILE" | grep -q "Packet received: OK"; then
            detach_ok_received=true
            print_info "OK response received for D packet (per RSP spec)"
        fi
    fi

    # Check for GDB-level detach confirmation messages
    if grep -qE "Detach completed|Ending remote debugging|Detaching from" "$TMPFILE"; then
        detach_clean=true
        print_info "GDB confirmed detach"
    fi

    # Strict check: require D packet sent AND OK received
    # This ensures the stub properly implements the detach protocol
    if [[ "$connected" == "true" ]] && [[ "$detach_packet_sent" == "true" ]] && [[ "$detach_ok_received" == "true" ]]; then
        test_pass "$TESTCASE ($ARCH)"
        return 0
    elif [[ "$connected" == "false" ]]; then
        test_fail "$TESTCASE" "Failed to connect"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    elif [[ "$detach_packet_sent" == "false" ]]; then
        test_fail "$TESTCASE" "D packet was not sent"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    elif [[ "$detach_ok_received" == "false" ]]; then
        test_fail "$TESTCASE" "Stub did not respond OK to D packet"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    else
        test_fail "$TESTCASE" "Detach did not complete cleanly"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

run_prerequisites_test || exit 1
run_detach_test || exit 1
print_test_summary
