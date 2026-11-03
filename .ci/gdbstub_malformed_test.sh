#!/usr/bin/env bash

# GDB Stub Malformed Packet Test
#
# Talks to the stub over a raw socket and sends packets that GDB itself
# never produces: missing separators, bad hex fields, oversized lengths.
# Each one must be answered with an error (or an empty reply for an
# unsupported qXfer object) and the stub must still serve a valid
# request afterwards.
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_malformed_test.sh
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   GDB_PORT        Port for the stub connection (default: 1234)
#

TESTCASE="GDB Malformed Packet Test"
source "$(dirname "$0")/test_common.sh"
TMPFILE=$(create_temp_file "gdbstub_malformed_test")

# RSP checksum: sum of the payload bytes modulo 256, as two hex digits
rsp_csum()
{
    local s="$1" sum=0 i c
    for ((i = 0; i < ${#s}; i++)); do
        printf -v c '%d' "'${s:i:1}"
        sum=$(((sum + c) & 0xff))
    done
    printf '%02x' "$sum"
}

# Send a packet and return the reply payload (without framing or acks)
rsp_exchange()
{
    local payload="$1" reply="" ch
    printf '$%s#%s' "$payload" "$(rsp_csum "$payload")" >&3
    while IFS= read -r -t 3 -n1 ch <&3; do
        reply+="$ch"
        [[ "$reply" == *'#'?? ]] && break
    done
    printf '+' >&3
    reply="${reply#*\$}" # drop the ack and the '$' framing
    echo "${reply%#??}"
}

# expect_reply <packet> <expected reply>
expect_reply()
{
    local got
    got=$(rsp_exchange "$1")
    if [[ "$got" == "$2" ]]; then
        print_info "$1 -> '$got'"
        return 0
    fi
    print_error "$1 -> '$got' (expected '$2')"
    return 1
}

run_malformed_test()
{
    init_test
    test_start "Testcase: $TESTCASE ($ARCH)"

    print_step "Starting emulator..."
    "$EMUDIR/emu" "$TEST_BIN" > "$TMPFILE" 2>&1 &
    local emu_pid=$!
    register_pid $emu_pid

    if ! wait_for_port "$GDB_PORT" 5 || ! kill -0 $emu_pid 2> /dev/null; then
        test_fail "$TESTCASE" "Emulator failed to start"
        return 1
    fi

    exec 3<> "/dev/tcp/127.0.0.1/$GDB_PORT"

    local failed=0
    expect_reply 'p' 'E16' || failed=1
    expect_reply 'Pzz' 'E16' || failed=1
    expect_reply 'P0' 'E16' || failed=1
    expect_reply 'm0' 'E16' || failed=1
    expect_reply 'm0,ffffffff' 'E16' || failed=1
    expect_reply 'M0,4:00' 'E16' || failed=1
    expect_reply 'X0,4:00' 'E16' || failed=1
    expect_reply 'vCont' 'E16' || failed=1
    expect_reply 'Hgx' 'E16' || failed=1 # not a hex thread id
    expect_reply 'T2' 'E03' || failed=1  # emu has one CPU
    expect_reply 'Z0,0' 'E16' || failed=1
    expect_reply 'qXfer:features:read:target.xml:zz' 'E16' || failed=1
    expect_reply 'qXfer:features:read:target.xml:ffff,10' 'E16' || failed=1
    expect_reply 'qXfer:features:read:other.xml:0,10' '' || failed=1

    # The stub must still be alive and answer a well-formed request
    local mem
    mem=$(rsp_exchange 'm0,4')
    if [[ "$mem" =~ ^[0-9a-f]{8}$ ]] && kill -0 $emu_pid 2> /dev/null; then
        print_info "stub still serves m0,4 -> '$mem'"
    else
        print_error "stub did not survive: m0,4 -> '$mem'"
        failed=1
    fi

    exec 3>&-

    if [[ $failed -eq 0 ]]; then
        test_pass "$TESTCASE ($ARCH)"
        return 0
    fi
    test_fail "$TESTCASE" "Unexpected reply to a malformed packet"
    print_error "Emulator output:"
    cat "$TMPFILE" >&2
    return 1
}

run_prerequisites_test || exit 1
run_malformed_test || exit 1
print_test_summary
