# Common utilities for CI scripts
# Source this file: . "$(dirname "$0")/common.sh"

# Enable strict mode only when executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    set -euo pipefail
fi

# Platform detection

PLATFORM=""
MACH=""

check_platform()
{
    PLATFORM=$(uname -s)
    MACH=$(uname -m)

    case "$PLATFORM" in
        Linux)
            case "$MACH" in
                x86_64 | aarch64) ;;
                *)
                    print_error "Unsupported Linux architecture: $MACH"
                    exit 1
                    ;;
            esac
            ;;
        Darwin)
            case "$MACH" in
                arm64 | x86_64) ;;
                *)
                    print_error "Unsupported macOS architecture: $MACH"
                    exit 1
                    ;;
            esac
            ;;
        *)
            print_error "Unsupported platform: $PLATFORM"
            exit 1
            ;;
    esac
}

# Parallel jobs based on CPU count
if command -v nproc &> /dev/null; then
    PARALLEL_JOBS="-j$(nproc)"
elif command -v sysctl &> /dev/null; then
    PARALLEL_JOBS="-j$(sysctl -n hw.ncpu)"
else
    PARALLEL_JOBS="-j2"
fi

# Output formatting

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color
BOLD='\033[1m'

print_success()
{
    echo -e "${GREEN}[OK] $*${NC}"
}

print_error()
{
    echo -e "${RED}[ERROR] $*${NC}" >&2
}

print_warning()
{
    echo -e "${YELLOW}[WARN] $*${NC}"
}

print_info()
{
    echo -e "${BLUE}[INFO] $*${NC}"
}

print_step()
{
    echo -e "${CYAN}[STEP] $*${NC}"
}

print_header()
{
    echo -e "\n${BOLD}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${BOLD}  $*${NC}"
    echo -e "${BOLD}═══════════════════════════════════════════════════════════════${NC}\n"
}

# Test utilities

# Test result tracking
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_TOTAL=0
TEST_START_TIME=0
declare -a TEST_RESULTS=()

test_start()
{
    local test_name="$1"
    TEST_START_TIME=$(date +%s%N 2> /dev/null || date +%s)
    print_step "Running: $test_name"
}

test_pass()
{
    local test_name="$1"
    local duration=""
    if [[ -n "${TEST_START_TIME:-}" ]]; then
        local end_time=$(date +%s%N 2> /dev/null || date +%s)
        if [[ "$TEST_START_TIME" =~ ^[0-9]+$ ]] && [[ "$end_time" =~ ^[0-9]+$ ]]; then
            if [[ ${#TEST_START_TIME} -gt 10 ]]; then
                duration=" ($(((end_time - TEST_START_TIME) / 1000000))ms)"
            else
                duration=" ($((end_time - TEST_START_TIME))s)"
            fi
        fi
    fi
    print_success "PASS: $test_name$duration"
    ((TESTS_PASSED++))
    ((TESTS_TOTAL++))
    TEST_RESULTS+=("PASS: $test_name$duration")
}

test_fail()
{
    local test_name="$1"
    local reason="${2:-}"
    print_error "FAIL: $test_name${reason:+ - $reason}"
    ((TESTS_FAILED++))
    ((TESTS_TOTAL++))
    TEST_RESULTS+=("FAIL: $test_name${reason:+ - $reason}")
}

test_skip()
{
    local test_name="$1"
    local reason="${2:-}"
    print_warning "SKIP: $test_name${reason:+ - $reason}"
    TEST_RESULTS+=("SKIP: $test_name${reason:+ - $reason}")
}

print_test_summary()
{
    print_header "Test Summary"

    for result in "${TEST_RESULTS[@]}"; do
        if [[ "$result" == PASS:* ]]; then
            echo -e "  ${GREEN}$result${NC}"
        elif [[ "$result" == FAIL:* ]]; then
            echo -e "  ${RED}$result${NC}"
        else
            echo -e "  ${YELLOW}$result${NC}"
        fi
    done

    echo ""
    echo -e "  ${BOLD}Total: $TESTS_TOTAL | Passed: ${GREEN}$TESTS_PASSED${NC} | Failed: ${RED}$TESTS_FAILED${NC}"

    if [[ $TESTS_FAILED -gt 0 ]]; then
        echo -e "\n  ${RED}${BOLD}TESTS FAILED${NC}"
        return 1
    else
        echo -e "\n  ${GREEN}${BOLD}ALL TESTS PASSED${NC}"
        return 0
    fi
}

# Assert command succeeds
ASSERT()
{
    if ! "$@"; then
        print_error "Assertion failed: $*"
        exit 1
    fi
}

# Cleanup utilities

declare -a CLEANUP_FUNCTIONS=()
declare -a CLEANUP_PIDS=()

register_cleanup()
{
    CLEANUP_FUNCTIONS+=("$1")
}

register_pid()
{
    CLEANUP_PIDS+=("$1")
}

cleanup()
{
    # Kill registered PIDs
    for pid in "${CLEANUP_PIDS[@]:-}"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2> /dev/null; then
            kill "$pid" 2> /dev/null || true
            wait "$pid" 2> /dev/null || true
        fi
    done

    # Run registered cleanup functions
    for func in "${CLEANUP_FUNCTIONS[@]:-}"; do
        $func 2> /dev/null || true
    done
}

cleanup_emulator()
{
    # Kill registered emulator PIDs only (avoid killing unrelated processes)
    for pid in "${CLEANUP_PIDS[@]:-}"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2> /dev/null; then
            kill "$pid" 2> /dev/null || true
        fi
    done
}

# Register cleanup on exit
trap cleanup EXIT

# Download utilities

DOWNLOAD_TOOL=""

detect_download_tool()
{
    if [[ -n "$DOWNLOAD_TOOL" ]]; then
        return
    fi

    if command -v curl &> /dev/null; then
        DOWNLOAD_TOOL="curl"
    elif command -v wget &> /dev/null; then
        DOWNLOAD_TOOL="wget"
    else
        print_error "Neither curl nor wget found"
        exit 1
    fi
}

# Download to stdout
download_to_stdout()
{
    local url="$1"
    detect_download_tool

    case "$DOWNLOAD_TOOL" in
        curl) curl -fsSL --retry 5 --retry-delay 2 --max-time 60 "$url" ;;
        wget) wget -qO- --tries=5 --timeout=60 "$url" ;;
    esac
}

# Download to file
download_to_file()
{
    local url="$1"
    local output="$2"
    detect_download_tool

    case "$DOWNLOAD_TOOL" in
        curl) curl -fsSL --retry 5 --retry-delay 2 --max-time 300 -o "$output" "$url" ;;
        wget) wget -q --tries=5 --timeout=300 -O "$output" "$url" ;;
    esac
}

# Download with progress
download_with_progress()
{
    local url="$1"
    local output="$2"
    detect_download_tool

    case "$DOWNLOAD_TOOL" in
        curl) curl -fL --retry 5 --retry-delay 2 --max-time 600 -# -o "$output" "$url" ;;
        wget) wget --tries=5 --timeout=600 --show-progress -O "$output" "$url" ;;
    esac
}

# Check URL accessibility
check_url()
{
    local url="$1"
    detect_download_tool

    case "$DOWNLOAD_TOOL" in
        curl) curl -fsSL --head --max-time 10 "$url" > /dev/null 2>&1 ;;
        wget) wget -q --spider --timeout=10 "$url" 2> /dev/null ;;
    esac
}

# GDB utilities

# Find a suitable GDB for RISC-V
find_riscv_gdb()
{
    local cross="${CROSS_COMPILE:-}"
    local gdb_candidates=(
        "${cross}gdb"
        "gdb-multiarch"
        "riscv64-unknown-elf-gdb"
        "riscv32-unknown-elf-gdb"
        "riscv-none-elf-gdb"
    )

    for gdb in "${gdb_candidates[@]}"; do
        if [[ -n "$gdb" ]] && command -v "$gdb" &> /dev/null; then
            echo "$gdb"
            return 0
        fi
    done

    print_error "No suitable RISC-V GDB found"
    print_info "Tried: ${gdb_candidates[*]}"
    return 1
}

# Wait for emulator to be ready (listening on port)
wait_for_port()
{
    local port="$1"
    local timeout="${2:-10}"
    local elapsed=0

    while ! nc -z 127.0.0.1 "$port" 2> /dev/null; do
        sleep 0.1
        elapsed=$((elapsed + 1))
        if [[ $elapsed -ge $((timeout * 10)) ]]; then
            return 1
        fi
    done
    return 0
}

# Toolchain utilities

# Check if toolchain supports multilib (both RV32 and RV64)
check_multilib_support()
{
    local cc="${CROSS_COMPILE:-}gcc"

    if ! command -v "$cc" &> /dev/null; then
        return 1
    fi

    # Check if gcc supports both rv32 and rv64
    local multilib
    multilib=$("$cc" --print-multi-lib 2> /dev/null) || return 1

    if echo "$multilib" | grep -q "rv32" && echo "$multilib" | grep -q "rv64"; then
        return 0
    fi

    return 1
}

# Detect architecture capability from CROSS_COMPILE
# Sets DETECTED_RV32, DETECTED_RV64, and DETECTED_MULTILIB variables
# May auto-detect and export CROSS_COMPILE if not set
detect_arch_from_toolchain()
{
    local cross="${CROSS_COMPILE:-}"

    DETECTED_RV32=false
    DETECTED_RV64=false
    DETECTED_MULTILIB=false
    DETECTED_AUTO=false

    # If no CROSS_COMPILE, try to find a default toolchain
    if [[ -z "$cross" ]]; then
        if command -v riscv64-unknown-elf-gcc &> /dev/null; then
            cross="riscv64-unknown-elf-"
        elif command -v riscv64-linux-gnu-gcc &> /dev/null; then
            cross="riscv64-linux-gnu-"
        elif command -v riscv32-unknown-elf-gcc &> /dev/null; then
            cross="riscv32-unknown-elf-"
        else
            # No toolchain found - warn and assume both to let build fail clearly
            print_warning "No RISC-V toolchain found in PATH"
            DETECTED_RV32=true
            DETECTED_RV64=true
            return
        fi
        # Update CROSS_COMPILE for downstream use
        export CROSS_COMPILE="$cross"
        DETECTED_AUTO=true
    fi

    case "$cross" in
        riscv32-*)
            # RV32-only toolchain, no need to check multilib
            DETECTED_RV32=true
            DETECTED_RV64=false
            ;;
        riscv64-* | riscv-none-elf-* | riscv-none-*)
            # Check if toolchain supports multilib (both RV32 and RV64)
            if check_multilib_support; then
                DETECTED_RV32=true
                DETECTED_RV64=true
                DETECTED_MULTILIB=true
            else
                # Not multilib - determine primary architecture from prefix
                if [[ "$cross" == riscv64-* ]]; then
                    DETECTED_RV32=false
                    DETECTED_RV64=true
                else
                    # riscv-none-elf without multilib defaults to RV32
                    DETECTED_RV32=true
                    DETECTED_RV64=false
                fi
            fi
            ;;
        *)
            # Unknown prefix - warn and assume both
            print_warning "Unknown toolchain prefix: $cross"
            DETECTED_RV32=true
            DETECTED_RV64=true
            ;;
    esac
}

# Get toolchain prefix for architecture
get_toolchain_prefix()
{
    local arch="$1"
    case "$arch" in
        rv32 | RV32) echo "riscv32-unknown-elf-" ;;
        rv64 | RV64) echo "riscv64-unknown-elf-" ;;
        *) echo "riscv64-unknown-elf-" ;;
    esac
}

# Verify toolchain is functional
verify_toolchain()
{
    local prefix="${CROSS_COMPILE:-riscv64-unknown-elf-}"
    local cc="${prefix}gcc"

    if ! command -v "$cc" &> /dev/null; then
        print_error "Toolchain not found: $cc"
        return 1
    fi

    print_info "Using toolchain: $cc"
    $cc --version | head -1
    return 0
}
