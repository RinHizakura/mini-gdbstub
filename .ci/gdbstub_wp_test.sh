#!/usr/bin/env bash
# Z1 and Z2: hit a hardware breakpoint on 'add', then a write watchpoint on
# the tohost byte. GDB prints Old/New value only when the stop reply names
# the watchpoint, so that is the evidence the stop was attributed to it.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB Hardware Breakpoint and Watchpoint Test"

gdb_session << 'EOF'
hbreak add
continue
watch *(char *) 0xffc
continue
continue
quit
EOF
expect_output "hardware breakpoint set" 'Hardware assisted breakpoint [0-9]+ at'
expect_output "hardware breakpoint hit in add" 'Breakpoint [0-9]+, add \('
expect_output "watchpoint set on 0xffc" 'Hardware watchpoint [0-9]+: \*\(char \*\) 0xffc'
expect_output "stop attributed to the watchpoint" 'Old value = 0'
expect_output "new value reported" 'New value = 255'

test_end
