#!/usr/bin/env bash
# Packets GDB never sends: missing separators, bad hex, oversized lengths.
# Each is answered with an error and the stub keeps serving afterwards.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB Malformed Packet Test"
connect_emu

expect_reply 'p' 'E16'
expect_reply 'Pzz' 'E16'
expect_reply 'P0' 'E16'
expect_reply 'm0' 'E16'
expect_reply 'm0,ffffffff' 'E16'
expect_reply 'M0,4:00' 'E16'
expect_reply 'X0,4:00' 'E16'
expect_reply 'vCont' 'E16'
expect_reply 'Hgx' 'E16' # not a hex thread id
expect_reply 'T2' 'E03'  # emu has one CPU
expect_reply 'Z0,0' 'E16'
expect_reply 'qXfer:features:read:target.xml:zz' 'E16'
expect_reply 'qXfer:features:read:target.xml:ffff,10' 'l'
expect_reply 'qXfer:features:read:other.xml:0,10' 'E00'

# The stub must still be alive and answer a well-formed request
mem=$(rsp_exchange 'm0,4')
check "m0,4 still served" "${#mem} hex digits" '8 hex digits'
kill -0 $EMU_PID 2> /dev/null || die "emulator died"

test_end
