#!/usr/bin/env bash

# GDB Stub Protocol Conformance Test
#
# Talks to the stub over a raw socket and checks the replies GDB relies on
# but never shows: feature advertisement, acknowledgment discipline,
# thread ids, binary escaping of qXfer data, and the stop reply after an
# interrupt. The emulator runs a one-instruction infinite loop so that the
# target can be interrupted.
#
# Usage:
#   ARCH=rv64 .ci/gdbstub_protocol_test.sh
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   GDB_PORT        Port for the stub connection (default: 1234)
#

TESTCASE="GDB Protocol Conformance Test"
source "$(dirname "$0")/test_common.sh"
TMPFILE=$(create_temp_file "gdbstub_protocol_test")

cleanup_protocol_test()
{
    rm -f "$TMPFILE.bin"
    cleanup_test
}

run_protocol_test()
{
    init_test cleanup_protocol_test
    test_start "Testcase: $TESTCASE ($ARCH)"

    # "j .": the target spins until interrupted (same encoding on rv32/rv64)
    printf '\x6f\x00\x00\x00' > "$TMPFILE.bin"

    print_step "Starting emulator..."
    "$EMUDIR/emu" "$TMPFILE.bin" > "$TMPFILE" 2>&1 &
    local emu_pid=$!
    register_pid $emu_pid

    if ! wait_for_port "$GDB_PORT" 5 || ! kill -0 $emu_pid 2> /dev/null; then
        test_fail "$TESTCASE" "Emulator failed to start"
        return 1
    fi

    exec 3<> "/dev/tcp/127.0.0.1/$GDB_PORT"
    local raw xml

    # Exactly one ack precedes a reply. PacketSize is hex; GDB sizes its
    # 'm' requests to half of it, so 0x7fd bytes must work and 0x7fe not.
    raw=$(rsp_exchange_raw 'qSupported:vContSupported+')
    expect_raw 'qSupported' "$raw" '+$*PacketSize=ffa;*#??'
    raw=$(rsp_exchange 'm0,7fd')
    expect_raw 'm0,7fd' "${#raw} hex digits" '4090 hex digits'
    expect_reply 'm0,7fe' 'E16'

    # Thread ids are hex, start at 1, and 0 / -1 mean any / all
    expect_reply '?' 'T05'
    expect_reply 'qfThreadInfo' 'm1'
    expect_reply 'qsThreadInfo' 'l'
    expect_reply 'qC' 'QC1'
    expect_reply 'Hg0' 'OK'
    expect_reply 'Hg1' 'OK'
    expect_reply 'Hg-1' 'OK'
    expect_reply 'Hc-1' 'OK'
    expect_reply 'T1' 'OK'

    # qXfer data is binary-escaped, and a chunk never splits an escape pair
    xml="<target version=\"1.0\"><architecture>$(get_gdb_arch)</architecture></target>"
    expect_reply 'qXfer:features:read:target.xml:0,1000' \
        $'l<!-- }\x03 }\x04 }] }\n -->'"$xml"
    expect_reply 'qXfer:features:read:target.xml:0,6' $'m<!-- }\x03'
    expect_reply 'qXfer:features:read:target.xml:6,2' $'m }\x04'

    # After QStartNoAckMode the stub stops acknowledging
    raw=$(rsp_exchange_raw 'QStartNoAckMode')
    expect_raw 'QStartNoAckMode' "$raw" '+$OK#9a'
    raw=$(rsp_exchange_raw '?')
    expect_raw '? (no-ack)' "$raw" '$T05#b9'

    # Ctrl-C while running stops with SIGINT, and '?' repeats that reply;
    # a step afterwards is an ordinary trap again
    rsp_send 'vCont;c'
    sleep 0.2
    printf '\003' >&3
    raw=$(rsp_recv)
    expect_raw 'Ctrl-C' "$raw" '$T02#b6'
    expect_reply '?' 'T02'

    # The signal forms GDB sends once vCont is in use are accepted too
    rsp_send 'vCont;C05'
    sleep 0.2
    printf '\003' >&3
    raw=$(rsp_recv)
    expect_raw 'vCont;C05 then Ctrl-C' "$raw" '$T02#b6'
    expect_reply 'vCont;S05' 'T05'
    expect_reply 'vCont;s' 'T05'
    expect_reply '?' 'T05'

    # The X payload may carry escaped '#' (0x03 on the wire) without
    # being mistaken for an interrupt: the target must still be stopped
    raw=$(rsp_exchange_raw $'X10,2:}\x03}\x03')
    expect_raw 'X with escaped #' "$raw" '$OK#9a'
    expect_reply 'm10,2' '2323'
    expect_reply '?' 'T05'

    exec 3>&-

    if [[ $RSP_FAILED -eq 0 ]]; then
        test_pass "$TESTCASE ($ARCH)"
        return 0
    fi
    test_fail "$TESTCASE" "$RSP_FAILED protocol reply mismatch(es)"
    print_error "Emulator output:"
    cat "$TMPFILE" >&2
    return 1
}

run_prerequisites_test || exit 1
run_protocol_test || exit 1
print_test_summary
