#!/usr/bin/env bash
# 'detach' sends D and the stub answers OK before closing the connection.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB Detach Test"

GDB_DEBUG=1 gdb_session << 'EOF'
printf "Connected, PC = %p\n", $pc
detach
quit
EOF
expect_output "connected" 'Connected, PC = '
expect_output "D packet sent" 'Sending packet: \$D#'
expect_output "D answered OK" 'Packet received: OK' <(reply_to '\$D#')

test_end
