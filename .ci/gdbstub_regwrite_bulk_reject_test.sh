#!/usr/bin/env bash
# A G packet of the wrong length is rejected with E16 and changes nothing.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB Bulk Register Reject Test"

gdb_session << 'EOF'
break add
continue
maintenance packet g
maintenance packet G00
maintenance packet g
continue
quit
EOF
expect_output "wrong-length G rejected with E16" 'received: "E16"'
before=$(extract_reg_blob 1)
check "register dump captured" "${#before}" "$((REG_NUM * REG_HEX))"
check "registers unchanged after the rejected G" "$(extract_reg_blob 2)" "$before"

test_end
