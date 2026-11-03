#!/usr/bin/env bash
# Replies GDB relies on but never shows: feature advertisement,
# acknowledgment discipline, thread ids, binary escaping of qXfer data, and
# the stop reply after an interrupt. The emulator runs a one-instruction
# infinite loop ("j .", same encoding on rv32 and rv64) so it can be
# interrupted.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB Protocol Conformance Test"
printf '\x6f\x00\x00\x00' > "$WORK/loop.bin"
connect_emu "$WORK/loop.bin"

# Exactly one ack precedes a reply. PacketSize is hex; GDB sizes its 'm'
# requests to half of it, so 0x7fd bytes must work and 0x7fe not.
check 'qSupported' "$(rsp_exchange_raw 'qSupported:vContSupported+')" '+$*PacketSize=ffa;*#??'
mem=$(rsp_exchange 'm0,7fd')
check 'm0,7fd' "${#mem} hex digits" '4090 hex digits'
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
xml="<target version=\"1.0\"><architecture>$GDB_ARCH</architecture></target>"
expect_reply 'qXfer:features:read:target.xml:0,1000' $'l<!-- }\x03 }\x04 }] }\n -->'"$xml"
expect_reply 'qXfer:features:read:target.xml:0,6' $'m<!-- }\x03'
expect_reply 'qXfer:features:read:target.xml:6,2' $'m }\x04'

# After QStartNoAckMode the stub stops acknowledging
check 'QStartNoAckMode' "$(rsp_exchange_raw 'QStartNoAckMode')" '+$OK#9a'
check '? (no-ack)' "$(rsp_exchange_raw '?')" '$T05#b9'

# Ctrl-C while running stops with SIGINT, and '?' repeats that reply
rsp_send 'vCont;c'
sleep 0.2
printf '\003' >&3
check 'Ctrl-C' "$(rsp_recv)" '$T02#b6'
expect_reply '?' 'T02'

# The signal forms GDB sends once vCont is in use are accepted too
rsp_send 'vCont;C05'
sleep 0.2
printf '\003' >&3
check 'vCont;C05 then Ctrl-C' "$(rsp_recv)" '$T02#b6'
expect_reply 'vCont;S05' 'T05'
expect_reply 'vCont;s' 'T05'
expect_reply '?' 'T05'

# An X payload may carry escaped '#' (0x03 on the wire) without being
# mistaken for an interrupt: the target stays stopped and the data lands
check 'X with escaped #' "$(rsp_exchange_raw $'X10,2:}\x03}\x03')" '$OK#9a'
expect_reply 'm10,2' '2323'
expect_reply '?' 'T05'

test_end
