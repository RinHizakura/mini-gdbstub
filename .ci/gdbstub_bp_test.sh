#!/usr/bin/env bash
# Set a software breakpoint on 'add', hit it, continue to completion.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB Breakpoint Test"

gdb_session << 'EOF'
break add
continue
printf "Hit breakpoint, PC = %p\n", $pc
continue
quit
EOF
expect_output "breakpoint set" 'Breakpoint [0-9]+ at'
expect_output "breakpoint hit in add" 'Breakpoint [0-9]+, add \('

test_end
