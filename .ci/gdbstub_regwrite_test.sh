#!/usr/bin/env bash

# GDB Stub Register Write Test
#
# Tests register write functionality including:
# - Single register write (P packet) with read-back verification
# - Bulk register write validation (G packet with wrong length -> E22)
# - Bulk register write round-trip (valid G packet commit + read-back)
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_regwrite_test.sh
#   CROSS_COMPILE=riscv64-unknown-elf- .ci/gdbstub_regwrite_test.sh
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   CROSS_COMPILE   Toolchain prefix (e.g., riscv64-unknown-elf-)
#   RISCV_GDB       Path to GDB executable (auto-detected if not set)
#   GDB_PORT        Port for GDB connection (default: 1234)
#
# Stub register layout (validated by this test):
#   The 'g' packet returns registers as: x0, x1, ..., x31, pc
#   33 registers total, each REGSZ bytes (4 for RV32, 8 for RV64).
#   t0 = x5, so its offset in the hex blob is 5 * (REGSZ * 2).
#
# Note on P vs G packet selection:
#   GDB may use either P (single register) or G (bulk write) depending on
#   stub capabilities. Test 1 exercises write_reg via GDB's set command
#   (typically P). Tests 2-3 exercise process_reg_write via raw G packets.

TMPFILE="/tmp/gdbstub_regwrite_test_$$"
TESTCASE="GDB Register Write Test"
source "$(dirname "$0")/test_common.sh"

# Expected register count: x0..x31 + pc = 33
REG_NUM=33

# Override cleanup to handle extra temp files
cleanup_test()
{
    rm -f "$TMPFILE" "$TMPFILE.gdb" "$TMPFILE.phase_a" "$TMPFILE.phase_b"
    cleanup_emulator
}
register_cleanup cleanup_test

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

# Find a suitable GDB (shared across tests)
find_gdb()
{
    if [[ -n "${RISCV_GDB:-}" ]]; then
        echo "$RISCV_GDB"
    else
        find_riscv_gdb
    fi
}

# Run a GDB batch session against the emulator.
# Args: $1=gdb_path $2=gdb_script $3=output_file $4=test_name
run_gdb_session()
{
    local gdb="$1"
    local script="$2"
    local output="$3"
    local name="$4"
    local gdb_timeout=30

    if ! timeout "$gdb_timeout" "$gdb" --batch -x "$script" > "$output" 2>&1; then
        local exit_code=$?
        if [[ $exit_code -eq 124 ]]; then
            test_fail "$name" "GDB session timed out"
            return 1
        fi
        if ! grep -q "Remote connection closed\|Remote communication error" "$output"; then
            test_fail "$name" "GDB session failed"
            print_error "GDB output:"
            cat "$output" >&2
            return 1
        fi
    fi
    return 0
}

# Test 1: Single register write (P packet) with read-back verification
run_single_reg_write_test()
{
    local gdb_arch
    gdb_arch=$(get_gdb_arch)

    test_start "Single Register Write Test ($ARCH)"

    local gdb
    gdb=$(find_gdb) || {
        test_fail "Single Register Write Test" "No suitable GDB found"
        return 1
    }

    print_info "Using GDB: $gdb"
    print_info "Architecture: $gdb_arch"

    # Start emulator in background
    kill_prev_emulator
    print_step "Starting emulator..."
    "$EMUDIR/emu" "$TEST_BIN" &
    local emu_pid=$!
    register_pid $emu_pid

    # Wait for emulator to be ready.
    # NOTE: Do NOT use nc/wait_for_port here. The emulator accepts exactly
    # one connection; a probe connection would consume the accept() slot.
    sleep 0.5
    if ! kill -0 $emu_pid 2> /dev/null; then
        test_fail "Single Register Write Test" "Emulator failed to start"
        return 1
    fi

    cat > "$TMPFILE.gdb" << EOF
set pagination off
set confirm off
set architecture $gdb_arch
file $TEST_OBJ
target remote :$GDB_PORT

# Stop at a known point
break add
continue

# Read initial t0 value
printf "BEFORE_T0=%lx\\n", \$t0

# Write a known value to t0 (register x5)
set \$t0 = 0x42

# Read back t0
printf "AFTER_T0=%lx\\n", \$t0

# Continue to completion
continue
quit
EOF

    print_step "Running single register write test..."
    run_gdb_session "$gdb" "$TMPFILE.gdb" "$TMPFILE" "Single Register Write Test" || return 1
    wait $emu_pid 2> /dev/null || true

    if grep -q "AFTER_T0=42" "$TMPFILE"; then
        print_info "t0 written and read back: 0x42"
        test_pass "Single Register Write Test ($ARCH)"
        return 0
    else
        test_fail "Single Register Write Test" "t0 read-back mismatch"
        print_error "GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

# Test 2: Bulk register write rejection (G packet with wrong length -> E22)
run_bulk_reg_reject_test()
{
    local gdb_arch
    gdb_arch=$(get_gdb_arch)

    test_start "Bulk Register Reject Test ($ARCH)"

    local gdb
    gdb=$(find_gdb) || {
        test_fail "Bulk Register Reject Test" "No suitable GDB found"
        return 1
    }

    # Start emulator in background
    kill_prev_emulator
    print_step "Starting emulator..."
    "$EMUDIR/emu" "$TEST_BIN" &
    local emu_pid=$!
    register_pid $emu_pid

    sleep 0.5
    if ! kill -0 $emu_pid 2> /dev/null; then
        test_fail "Bulk Register Reject Test" "Emulator failed to start"
        return 1
    fi

    cat > "$TMPFILE.gdb" << EOF
set pagination off
set confirm off
set architecture $gdb_arch
file $TEST_OBJ
target remote :$GDB_PORT

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

    print_step "Running bulk register reject test..."
    run_gdb_session "$gdb" "$TMPFILE.gdb" "$TMPFILE" "Bulk Register Reject Test" || return 1
    wait $emu_pid 2> /dev/null || true

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
        test_pass "Bulk Register Reject Test ($ARCH)"
        return 0
    else
        test_fail "Bulk Register Reject Test" "See errors above"
        print_error "Full GDB output:"
        cat "$TMPFILE" >&2
        return 1
    fi
}

# Test 3: Bulk register write round-trip (valid G packet commit path)
#
# Two-phase test:
#   Phase A: Start emulator, read all registers via 'g', capture hex blob.
#   Phase B: Start fresh emulator, modify t0 in the captured blob, send via
#            'G', read back via 'g', verify:
#            - G packet returned OK
#            - t0 has the new value
#            - all other registers match what was sent (full blob integrity)
run_bulk_reg_roundtrip_test()
{
    local gdb_arch reg_hex_len t0_offset expected_blob_len
    gdb_arch=$(get_gdb_arch)
    reg_hex_len=$(get_reg_hex_len)
    # t0 is x5 in RISC-V; offset = register_index * hex_chars_per_register
    t0_offset=$((5 * reg_hex_len))
    # Expected blob: REG_NUM registers * reg_hex_len hex chars each
    expected_blob_len=$((REG_NUM * reg_hex_len))

    test_start "Bulk Register Round-trip Test ($ARCH)"

    local gdb
    gdb=$(find_gdb) || {
        test_fail "Bulk Register Round-trip Test" "No suitable GDB found"
        return 1
    }

    # -- Phase A: Capture register state --
    kill_prev_emulator
    print_step "Phase A: Capturing register state..."
    "$EMUDIR/emu" "$TEST_BIN" &
    local emu_pid=$!
    register_pid $emu_pid

    sleep 0.5
    if ! kill -0 $emu_pid 2> /dev/null; then
        test_fail "Bulk Register Round-trip Test" "Phase A emulator failed to start"
        return 1
    fi

    cat > "$TMPFILE.gdb" << EOF
set pagination off
set confirm off
set architecture $gdb_arch
file $TEST_OBJ
target remote :$GDB_PORT
break add
continue
maintenance packet g
continue
quit
EOF

    run_gdb_session "$gdb" "$TMPFILE.gdb" "$TMPFILE.phase_a" \
        "Bulk Register Round-trip Test" || return 1
    wait $emu_pid 2> /dev/null || true

    # Extract the hex blob and normalize to lowercase
    local hex_blob
    hex_blob=$(extract_reg_blob "$TMPFILE.phase_a" 1 | hex_lower)
    if [[ -z "$hex_blob" ]]; then
        test_fail "Bulk Register Round-trip Test" "Failed to capture register blob"
        print_error "Phase A output:"
        cat "$TMPFILE.phase_a" >&2
        return 1
    fi

    local blob_len=${#hex_blob}
    print_info "Captured register blob: $blob_len hex chars"

    # Validate blob length matches expected register count
    if [[ $blob_len -ne $expected_blob_len ]]; then
        test_fail "Bulk Register Round-trip Test" \
            "Blob length $blob_len != expected $expected_blob_len ($REG_NUM regs * $reg_hex_len hex/reg)"
        return 1
    fi

    # -- Modify t0 to a known value --
    # Write 0xdeadbeef in little-endian byte order
    local new_t0
    case "$ARCH" in
        rv32 | RV32) new_t0="efbeadde" ;;
        rv64 | RV64) new_t0="efbeadde00000000" ;;
        *) new_t0="efbeadde00000000" ;;
    esac

    local prefix="${hex_blob:0:$t0_offset}"
    local suffix="${hex_blob:$((t0_offset + reg_hex_len))}"
    local modified_blob="${prefix}${new_t0}${suffix}"

    # Verify we didn't change the blob length
    if [[ ${#modified_blob} -ne $blob_len ]]; then
        test_fail "Bulk Register Round-trip Test" \
            "Modified blob length mismatch (${#modified_blob} != $blob_len)"
        return 1
    fi

    print_info "Modified t0 at offset $t0_offset to 0xdeadbeef (LE)"

    # -- Phase B: Write modified blob and verify --
    kill_prev_emulator
    print_step "Phase B: Writing modified registers via G packet..."

    "$EMUDIR/emu" "$TEST_BIN" &
    emu_pid=$!
    register_pid $emu_pid

    sleep 0.5
    if ! kill -0 $emu_pid 2> /dev/null; then
        test_fail "Bulk Register Round-trip Test" "Phase B emulator failed to start"
        return 1
    fi

    cat > "$TMPFILE.gdb" << EOF
set pagination off
set confirm off
set architecture $gdb_arch
file $TEST_OBJ
target remote :$GDB_PORT
break add
continue
maintenance packet G${modified_blob}
maintenance packet g
continue
quit
EOF

    run_gdb_session "$gdb" "$TMPFILE.gdb" "$TMPFILE.phase_b" \
        "Bulk Register Round-trip Test" || return 1
    wait $emu_pid 2> /dev/null || true

    local test_ok=true

    # G packet should return OK
    if grep -q 'received: "OK"' "$TMPFILE.phase_b"; then
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
    readback_blob=$(extract_reg_blob "$TMPFILE.phase_b" 1 | hex_lower)
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
        test_pass "Bulk Register Round-trip Test ($ARCH)"
        return 0
    else
        test_fail "Bulk Register Round-trip Test" "See errors above"
        print_error "Phase B output:"
        cat "$TMPFILE.phase_b" >&2
        return 1
    fi
}

run_prerequisites_test || exit 1
run_single_reg_write_test || true
run_bulk_reg_reject_test || true
run_bulk_reg_roundtrip_test || true
print_test_summary
