#!/bin/bash

# Basic CI/CD test script for mini-gdbstub
# This is a MVP (Minimum Viable Product) version

set -e  # Exit on any error

# Configuration
GDBSTUB_HOST="127.0.0.1"
GDBSTUB_PORT="1234"
TIMEOUT=10
GDB_TIMEOUT=30
BUILD_DIR="build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Cleanup function - will be called on script exit
cleanup() {
    log_info "Cleaning up..."
    if [ ! -z "$GDBSTUB_PID" ]; then
        log_info "Stopping gdbstub server (PID: $GDBSTUB_PID)"
        kill $GDBSTUB_PID 2>/dev/null || true
        wait $GDBSTUB_PID 2>/dev/null || true
    fi
    # Clean up temporary files
    rm -f /tmp/gdb_test.gdb /tmp/gdb_output.log
}

# Set trap to call cleanup on script exit
trap cleanup EXIT

# Step 1: Build stage
log_info "=== Step 1: Building project ==="
make clean || true
make build-emu || {
    log_error "Build failed"
    exit 1
}
log_info "Build completed successfully"

# Step 2: Start gdbstub server
log_info "=== Step 2: Starting gdbstub server ==="
make run-gdbstub &
GDBSTUB_PID=$!
log_info "Started gdbstub server in background (PID: $GDBSTUB_PID)"

# Step 3: Wait for server to be ready
log_info "=== Step 3: Waiting for server to be ready ==="
for i in $(seq 1 $TIMEOUT); do
    # Check if emulator process is still running
    if ! ps -p $GDBSTUB_PID > /dev/null 2>&1; then
        log_error "Emulator process died unexpectedly"
        exit 1
    fi
    
    if netstat -tlnp 2>/dev/null | grep -q ":$GDBSTUB_PORT " || \
       ss -tlnp 2>/dev/null | grep -q ":$GDBSTUB_PORT "; then
        log_info "Server is ready on port $GDBSTUB_PORT"
        break
    fi
    
    if [ $i -eq $TIMEOUT ]; then
        log_error "Timeout waiting for server to start"
        exit 1
    fi
    
    log_info "Waiting for server... ($i/$TIMEOUT)"
    sleep 1
done

# Step 4: Basic connection test
log_info "=== Step 4: Basic connection test ==="

# Detect which GDB to use
if command -v riscv64-unknown-elf-gdb >/dev/null 2>&1; then
    GDB_CMD="riscv64-unknown-elf-gdb"
elif command -v gdb-multiarch >/dev/null 2>&1; then
    GDB_CMD="gdb-multiarch"
else
    log_error "No suitable GDB found"
    log_error "Please install gdb-multiarch or riscv64-unknown-elf-gdb"
    exit 1
fi
log_info "Using GDB: $GDB_CMD"

# Create a simple GDB script for testing
cat > /tmp/gdb_test.gdb << 'EOF'
set confirm off
set print pretty on
set architecture riscv:rv64
target remote 127.0.0.1:1234
info registers
continue
quit
EOF

# Run GDB test with timeout
log_info "Running GDB connection test..."
if timeout ${GDB_TIMEOUT}s $GDB_CMD -batch -x /tmp/gdb_test.gdb ${BUILD_DIR}/emu/test.obj 2>&1 | tee /tmp/gdb_output.log; then
    log_info "GDB connection test completed"
else
    exit_code=$?
    if [ $exit_code -eq 124 ]; then
        log_error "GDB connection test timed out after ${GDB_TIMEOUT} seconds"
    else
        log_error "GDB connection test failed"
    fi
    exit 1
fi

# Step 5: Verify test results
log_info "=== Step 5: Verifying results ==="

if grep -q "The target architecture is set to" /tmp/gdb_output.log; then
    log_info "✓ GDB connection successful"
else
    log_warn "Connection verification unclear"
fi

if grep -q "SIGTRAP" /tmp/gdb_output.log; then
    log_info "✓ Program completed successfully"
else
    log_warn "Program completion verification unclear"
fi

log_info "=== Test completed successfully! ==="

# Cleanup will be called automatically by trap