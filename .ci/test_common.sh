#!/usr/bin/env bash
#
# Shared harness for the stub tests. A test is a short script:
#
#   source "$(dirname "$0")/test_common.sh"
#   test_begin "GDB Continue Test"
#   gdb_session << 'EOF'
#   printf "PC = %p\n", $pc
#   continue
#   EOF
#   expect_output "PC was read" 'PC = 0x'
#   test_end
#
# Checks never abort the test: they log their outcome and count failures,
# so every expectation gets reported. test_end turns the count into the
# verdict. Only a missing prerequisite or a dead emulator ends a test early.
#
# Environment Variables:
#   ARCH            Target architecture: rv32 or rv64 (default: rv64)
#   GDB_PORT        Port the stub listens on (default: 1234)
#   RISCV_GDB       GDB executable (default: auto-detected)
#

set -euo pipefail
source "$(dirname "$0")/common.sh"

ARCH="${ARCH:-rv64}"
GDB_PORT="${GDB_PORT:-1234}"
EMUDIR="build/emu/$ARCH"
TEST_OBJ="$EMUDIR/emu_test.obj"
TEST_BIN="$EMUDIR/emu_test.bin"
REG_NUM=33 # x0..x31 + pc
case "$ARCH" in
    rv32 | RV32) GDB_ARCH="riscv:rv32" REG_HEX=8 ;;
    *) GDB_ARCH="riscv:rv64" REG_HEX=16 ;;
esac

FAILURES=0
GDB_RUNS=0
GDB_OUT="" # transcript of the latest gdb_session
WORK=$(mktemp -d "${TMPDIR:-/tmp}/gdbstub_test.XXXXXX")
EMU_LOG="$WORK/emu.log"
cleanup_work() { rm -rf "$WORK"; }
register_cleanup cleanup_work

# test_begin <name>: announce the test and make sure the emulator exists
test_begin()
{
    TESTCASE="$1 ($ARCH)"
    print_header "$1"
    print_info "Architecture: $ARCH"
    test_start "$TESTCASE"
    for f in "$EMUDIR/emu" "$TEST_BIN" "$TEST_OBJ"; do
        [[ -e "$f" ]] || die "missing $f (run: make build-emu ARCH=$ARCH)"
    done
}

# die <reason>: nothing further can be checked, so fail now
die()
{
    test_fail "$TESTCASE" "$1"
    dump_logs
    exit 1
}

dump_logs()
{
    if [[ -n "$GDB_OUT" ]]; then
        print_error "GDB output:"
        cat "$GDB_OUT" >&2
    fi
    if [[ -s "$EMU_LOG" ]]; then
        print_error "Emulator output:"
        cat "$EMU_LOG" >&2
    fi
}

# test_end: report the verdict and exit with it
test_end()
{
    exec 3>&-
    if [[ $FAILURES -eq 0 ]]; then
        test_pass "$TESTCASE"
    else
        test_fail "$TESTCASE" "$FAILURES check(s) failed"
        dump_logs
    fi
    print_test_summary || true
    exit $((FAILURES > 0))
}

# check <label> <got> <glob pattern>
check()
{
    if [[ "$2" == $3 ]]; then
        print_info "$1 -> '$2'"
    else
        print_error "$1 -> '$2' (expected $3)"
        FAILURES=$((FAILURES + 1))
    fi
}

# expect_output <label> <ERE> [file]: the GDB transcript must match
expect_output()
{
    if grep -qE "$2" "${3:-$GDB_OUT}"; then
        print_info "$1"
    else
        print_error "$1 (no match for /$2/)"
        FAILURES=$((FAILURES + 1))
    fi
}

# reply_to <ERE>: the transcript from the matching "Sending packet" line to
# the next "Packet received" line, for checking what answered a packet
reply_to()
{
    sed -n "/Sending packet:.*$1/,/Packet received:/p" "$GDB_OUT"
}

# start_emu [binary]: (re)start the emulator and wait until it listens. The
# stub serves a single connection, so a previous instance is stopped first.
start_emu()
{
    if [[ ${#CLEANUP_PIDS[@]} -gt 0 ]]; then
        cleanup_emulator
        wait 2> /dev/null || true
        CLEANUP_PIDS=()
        sleep 0.3 # let the port go
    fi
    "$EMUDIR/emu" "${1:-$TEST_BIN}" >> "$EMU_LOG" 2>&1 &
    EMU_PID=$!
    register_pid $EMU_PID
    wait_for_port "$GDB_PORT" 5 && kill -0 $EMU_PID 2> /dev/null ||
        die "emulator failed to start"
}

# gdb_session [binary] << 'EOF' ... EOF
# Run the GDB commands on stdin, after the connection preamble, against a
# fresh emulator. The transcript lands in $GDB_OUT. GDB_DEBUG=1 turns on
# "set debug remote" before connecting so the handshake is logged too.
gdb_session()
{
    GDB=${GDB:-${RISCV_GDB:-$(find_riscv_gdb)}} || die "no GDB found"
    start_emu "$@"
    {
        echo "set pagination off"
        echo "set confirm off"
        echo "set architecture $GDB_ARCH"
        echo "file \"$TEST_OBJ\""
        [[ -n "${GDB_DEBUG:-}" ]] && echo "set debug remote 1"
        echo "target remote :$GDB_PORT"
        cat
    } > "$WORK/gdb.cmd"
    GDB_OUT="$WORK/gdb.$((++GDB_RUNS)).out"

    print_step "Running GDB ($GDB)..."
    timeout 30 "$GDB" --batch -x "$WORK/gdb.cmd" > "$GDB_OUT" 2>&1 || {
        case $? in
            124) die "GDB session timed out" ;;
            *) grep -q "Remote connection closed\|Remote communication error" \
                "$GDB_OUT" || die "GDB exited with an error" ;;
        esac
    }
    wait $EMU_PID 2> /dev/null || true
}

# extract_reg_blob [occurrence]: the Nth register dump a "maintenance packet
# g" produced, in lower case. A dump is the only reply of 64+ hex digits.
extract_reg_blob()
{
    grep 'received: "' "$GDB_OUT" |
        sed 's/.*received: "\([^"]*\)".*/\1/' |
        grep -iE '^[0-9a-f]{64,}$' |
        sed -n "${1:-1}p" | tr 'A-F' 'a-f'
}

# --- Raw Remote Serial Protocol helpers; the socket is open on fd 3 ---

# connect_emu [binary]: start the emulator and open the socket on fd 3
connect_emu()
{
    start_emu "$@"
    exec 3<> "/dev/tcp/127.0.0.1/$GDB_PORT"
}

# RSP checksum: sum of the payload bytes modulo 256, as two hex digits
rsp_csum()
{
    local s="$1" sum=0 i c
    for ((i = 0; i < ${#s}; i++)); do
        printf -v c '%d' "'${s:i:1}"
        sum=$(((sum + c) & 0xff))
    done
    printf '%02x' "$sum"
}

# Send one packet without waiting for a reply
rsp_send()
{
    printf '$%s#%s' "$1" "$(rsp_csum "$1")" >&3
}

# Read until a packet ends ("#xx") or 3 s pass, then ack it. Returns the
# raw bytes, acks included; -d '' keeps a newline byte (an escaped '*').
rsp_recv()
{
    local reply="" ch
    while IFS= read -r -t 3 -n1 -d '' ch <&3; do
        reply+="$ch"
        [[ "$reply" == *'#'?? ]] && break
    done
    printf '+' >&3
    echo "$reply"
}

# Send a packet and return the raw reply
rsp_exchange_raw()
{
    rsp_send "$1"
    rsp_recv
}

# Send a packet and return the reply payload (without framing or acks)
rsp_exchange()
{
    local raw
    raw=$(rsp_exchange_raw "$1")
    raw="${raw#*\$}"
    echo "${raw%#??}"
}

# expect_reply <packet> <expected reply>
expect_reply()
{
    check "$1" "$(rsp_exchange "$1")" "$2"
}
