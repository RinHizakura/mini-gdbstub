#!/usr/bin/env bash
# P packet: write t0 and read it back.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB Single Register Write Test"

gdb_session << 'EOF'
break add
continue
set $t0 = 0x42
printf "AFTER_T0=%lx\n", $t0
continue
quit
EOF
expect_output "t0 written and read back as 0x42" 'AFTER_T0=42'

test_end
