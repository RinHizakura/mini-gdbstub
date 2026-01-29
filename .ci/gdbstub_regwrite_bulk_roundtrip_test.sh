#!/usr/bin/env bash

# GDB Stub Bulk Register Round-trip Test
#
# Tests bulk register write round-trip (valid G packet commit path).
#
# Two-phase test:
#   Phase A: Start emulator, read all registers via 'g', capture hex blob.
#   Phase B: Start fresh emulator, modify t0 in the captured blob, send via
#            'G', read back via 'g', verify:
#            - G packet returned OK
#            - t0 has the new value
#            - all other registers match what was sent (full blob integrity)
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_regwrite_bulk_roundtrip_test.sh
#   CROSS_COMPILE=riscv64-unknown-elf- .ci/gdbstub_regwrite_bulk_roundtrip_test.sh
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   CROSS_COMPILE   Toolchain prefix (e.g., riscv64-unknown-elf-)
#   RISCV_GDB       Path to GDB executable (auto-detected if not set)
#   GDB_PORT        Port for GDB connection (default: 1234)
#

TESTCASE="GDB Bulk Register Round-trip Test"
source "$(dirname "$0")/test_common.sh"

# BASE_TMPFILE is the primary temp file; TMPFILE may be swapped for phases.
BASE_TMPFILE=$(create_temp_file "gdbstub_regwrite_bulk_roundtrip_test")
TMPFILE="$BASE_TMPFILE"

# Override cleanup to handle extra temp files.
# Uses BASE_TMPFILE to ensure we clean up the original file set
# even if TMPFILE is currently swapped to a .phase_X variant.
cleanup_roundtrip()
{
    rm -f "$BASE_TMPFILE" "$BASE_TMPFILE.gdb" "$BASE_TMPFILE.phase_a" "$BASE_TMPFILE.phase_b"
    cleanup_emulator
}

run_bulk_reg_roundtrip_test()
{
    local orig_testcase="$TESTCASE"

    init_test cleanup_roundtrip

    # Register size will be detected from actual blob (emulator may differ from ARCH)
    local reg_hex_len t0_offset

    # -- Phase A: Capture register state --
    print_step "Phase A: Capturing register state..."

    # Use BASE_TMPFILE for the GDB script to ensure consistent path
    gdb_script_header > "$BASE_TMPFILE.gdb"
    cat >> "$BASE_TMPFILE.gdb" << 'EOF'
break add
continue
maintenance packet g
continue
quit
EOF

    # Swap TMPFILE so run_gdb_test_script writes to .phase_a
    TMPFILE="$BASE_TMPFILE.phase_a"

    run_gdb_test_script "$BASE_TMPFILE.gdb" "$orig_testcase (Phase A)"

    # Restore TMPFILE immediately after use
    TMPFILE="$BASE_TMPFILE"

    # Extract the hex blob and normalize to lowercase
    local hex_blob
    hex_blob=$(extract_reg_blob "$BASE_TMPFILE.phase_a" 1 | hex_lower)
    if [[ -z "$hex_blob" ]]; then
        test_fail "$orig_testcase" "Failed to capture register blob"
        print_error "Phase A output:"
        cat "$BASE_TMPFILE.phase_a" >&2
        return 1
    fi

    local blob_len=${#hex_blob}
    print_info "Captured register blob: $blob_len hex chars"

    # Derive register size from actual blob (emulator's compile-time setting)
    if [[ $((blob_len % REG_NUM)) -ne 0 ]]; then
        test_fail "$orig_testcase" \
            "Blob length $blob_len not divisible by REG_NUM ($REG_NUM)"
        return 1
    fi
    reg_hex_len=$((blob_len / REG_NUM))

    # Validate register size is sensible (4 or 8 bytes)
    if [[ $reg_hex_len -ne 8 ]] && [[ $reg_hex_len -ne 16 ]]; then
        test_fail "$orig_testcase" \
            "Unexpected register size: $reg_hex_len hex chars (expected 8 or 16)"
        return 1
    fi

    local detected_arch="rv64"
    [[ $reg_hex_len -eq 8 ]] && detected_arch="rv32"
    print_info "Detected emulator register size: $detected_arch ($reg_hex_len hex/reg)"

    # t0 is x5 in RISC-V; offset = register_index * hex_chars_per_register
    t0_offset=$((5 * reg_hex_len))

    # -- Modify t0 to a known value --
    # Write 0xdeadbeef in little-endian byte order (size matches detected arch)
    local new_t0
    if [[ $reg_hex_len -eq 8 ]]; then
        new_t0="efbeadde" # 32-bit LE
    else
        new_t0="efbeadde00000000" # 64-bit LE
    fi

    local prefix="${hex_blob:0:$t0_offset}"
    local suffix="${hex_blob:$((t0_offset + reg_hex_len))}"
    local modified_blob="${prefix}${new_t0}${suffix}"

    # Verify we didn't change the blob length
    if [[ ${#modified_blob} -ne $blob_len ]]; then
        test_fail "$orig_testcase" \
            "Modified blob length mismatch (${#modified_blob} != $blob_len)"
        return 1
    fi

    print_info "Modified t0 at offset $t0_offset to 0xdeadbeef (LE)"

    # -- Phase B: Write modified blob and verify --
    kill_prev_emulator
    print_step "Phase B: Writing modified registers via G packet..."

    gdb_script_header > "$BASE_TMPFILE.gdb"
    cat >> "$BASE_TMPFILE.gdb" << EOF
break add
continue
maintenance packet G${modified_blob}
maintenance packet g
continue
quit
EOF

    # Swap TMPFILE so run_gdb_test_script writes to .phase_b
    TMPFILE="$BASE_TMPFILE.phase_b"

    run_gdb_test_script "$BASE_TMPFILE.gdb" "$orig_testcase (Phase B)"

    # Restore TMPFILE
    TMPFILE="$BASE_TMPFILE"

    local test_ok=true

    # G packet should return OK
    if grep -q 'received: "OK"' "$BASE_TMPFILE.phase_b"; then
        print_info "Valid G packet accepted with OK"
    else
        print_error "Valid G packet was not accepted"
        test_ok=false
    fi

    # Read back the full register blob and compare against what we sent.
    # This verifies:
    #   - t0 has the new value (targeted register was committed)
    #   - all other registers match (no partial/corrupt writes)
    local readback_blob
    readback_blob=$(extract_reg_blob "$BASE_TMPFILE.phase_b" 1 | hex_lower)
    if [[ -n "$readback_blob" ]]; then
        # Check t0 specifically for a clear diagnostic
        local readback_t0="${readback_blob:$t0_offset:$reg_hex_len}"
        if [[ "$readback_t0" == "$new_t0" ]]; then
            print_info "t0 read-back matches: 0xdeadbeef (LE: $new_t0)"
        else
            print_error "t0 read-back mismatch: expected $new_t0, got $readback_t0"
            test_ok=false
        fi

        # Compare full blob: readback must match the modified blob we sent.
        # x0 is hardwired to zero in RISC-V and ignores writes, so exclude
        # the first register slot from comparison.
        local sent_rest="${modified_blob:$reg_hex_len}"
        local read_rest="${readback_blob:$reg_hex_len}"
        if [[ "$sent_rest" == "$read_rest" ]]; then
            print_info "Full register blob integrity verified (x1..x31,pc)"
        else
            print_error "Register blob mismatch: other registers were corrupted"
            # Find which register(s) differ for diagnostics
            local offset=$reg_hex_len
            for i in $(seq 1 $((REG_NUM - 1))); do
                local sent_reg="${modified_blob:$offset:$reg_hex_len}"
                local read_reg="${readback_blob:$offset:$reg_hex_len}"
                if [[ "$sent_reg" != "$read_reg" ]]; then
                    print_error "  reg $i: sent=$sent_reg got=$read_reg"
                fi
                offset=$((offset + reg_hex_len))
            done
            test_ok=false
        fi
    else
        print_error "Failed to extract read-back register blob"
        test_ok=false
    fi

    if [[ "$test_ok" == "true" ]]; then
        test_pass "$orig_testcase ($ARCH)"
        return 0
    else
        test_fail "$orig_testcase" "See errors above"
        print_error "Phase B output:"
        cat "$BASE_TMPFILE.phase_b" >&2
        return 1
    fi
}

run_prerequisites_test || exit 1
run_bulk_reg_roundtrip_test || exit 1
print_test_summary
