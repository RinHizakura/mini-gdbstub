#!/usr/bin/env bash
# G packet commit path: read every register, write them back with t0
# changed, read again. t0 must carry the new value and nothing else may move.
source "$(dirname "$0")/test_common.sh"
test_begin "GDB Bulk Register Round-trip Test"

# Phase A: capture the register file
gdb_session << 'EOF'
break add
continue
maintenance packet g
continue
quit
EOF
blob=$(extract_reg_blob)
check "register dump captured" "${#blob}" "$((REG_NUM * REG_HEX))"
[[ $FAILURES -eq 0 ]] || test_end

# Phase B: t0 is x5; 0xdeadbeef in little-endian, padded to the register size
t0_off=$((5 * REG_HEX))
new_t0=efbeadde
[[ $REG_HEX -eq 16 ]] && new_t0+=00000000
sent="${blob:0:$t0_off}${new_t0}${blob:$((t0_off + REG_HEX))}"

gdb_session << EOF
break add
continue
maintenance packet G$sent
maintenance packet g
continue
quit
EOF
expect_output "G packet accepted" 'received: "OK"'
got=$(extract_reg_blob)
check "t0 reads back as 0xdeadbeef" "${got:$t0_off:$REG_HEX}" "$new_t0"
# x0 is hardwired to zero and ignores writes, so compare x1..x31 and pc
check "other registers intact" "${got:$REG_HEX}" "${sent:$REG_HEX}"

test_end
