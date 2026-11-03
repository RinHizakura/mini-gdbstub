#!/usr/bin/env bash
# Connect, read the PC, and let the program run to completion.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB Continue Test"

gdb_session << 'EOF'
printf "PC before continue: %p\n", $pc
continue
quit
EOF
expect_output "PC read before continue" 'PC before continue: '

test_end
