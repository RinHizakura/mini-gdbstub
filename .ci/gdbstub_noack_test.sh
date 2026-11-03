#!/usr/bin/env bash
# QStartNoAckMode is advertised, GDB negotiates it, and the session works.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB No-Ack Mode Test"

GDB_DEBUG=1 gdb_session << 'EOF'
printf "PC = %p\n", $pc
quit
EOF
expect_output "QStartNoAckMode+ advertised" 'Packet received: .*QStartNoAckMode\+'
expect_output "QStartNoAckMode answered OK" 'Packet received: OK' <(reply_to 'QStartNoAckMode')
expect_output "session works without acks" 'PC = '

test_end
