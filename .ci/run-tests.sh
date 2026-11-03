#!/usr/bin/env bash
#
# GDB Stub Test Runner
#
# Runs all GDB stub tests for specified architectures.
# Auto-detects architecture capability from CROSS_COMPILE if set.
#
# Usage:
#   .ci/run-tests.sh                    # Auto-detect from CROSS_COMPILE or test both
#   .ci/run-tests.sh --arch=rv64        # Test RV64 only
#   .ci/run-tests.sh --arch=rv32        # Test RV32 only
#
# Environment Variables:
#   CROSS_COMPILE   Toolchain prefix (e.g., riscv64-unknown-elf-)
#                   - riscv32-* prefix: test RV32 only
#                   - riscv64-* prefix: test RV64 only
#                   - riscv-none-elf-* (multilib): test both
#   RISCV_GDB       Path to GDB executable
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/common.sh"

# Configuration
TEST_RV32=false
TEST_RV64=false
ARCH_EXPLICIT=false
OVERALL_PASSED=0
OVERALL_FAILED=0
RESULTS=()

usage()
{
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --arch=ARCH    Test specific architecture: rv32, rv64, or both"
    echo "  --help         Show this help message"
    echo ""
    echo "Environment Variables:"
    echo "  CROSS_COMPILE  Toolchain prefix (auto-detects arch capability)"
    echo "  RISCV_GDB      Path to GDB executable"
    echo ""
    echo "Architecture Detection:"
    echo "  riscv32-*      -> RV32 only"
    echo "  riscv64-*      -> RV64 only"
    echo "  riscv-none-*   -> Both (multilib toolchain)"
    echo "  (not set)      -> Both"
}

# Apply architecture detection from common.sh and set local test flags
apply_arch_detection()
{
    # Use common function to detect capabilities (may auto-detect CROSS_COMPILE)
    detect_arch_from_toolchain

    local cross="${CROSS_COMPILE:-}"

    # Map to local test flags
    TEST_RV32="$DETECTED_RV32"
    TEST_RV64="$DETECTED_RV64"

    # Print detection result based on actual detection state
    if [[ "$DETECTED_AUTO" == "true" ]]; then
        print_info "Auto-detected toolchain: $cross"
    elif [[ -n "$cross" ]]; then
        print_info "Using toolchain: $cross"
    fi

    # Report architecture capabilities with accurate source
    if [[ "$DETECTED_MULTILIB" == "true" ]]; then
        print_info "Multilib toolchain, testing both RV32 and RV64"
    elif [[ "$TEST_RV32" == "true" ]] && [[ "$TEST_RV64" == "true" ]]; then
        print_info "Testing both architectures (no multilib check)"
    elif [[ "$TEST_RV32" == "true" ]]; then
        print_info "RV32 architecture"
    elif [[ "$TEST_RV64" == "true" ]]; then
        print_info "RV64 architecture"
    fi
}

parse_args()
{
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --arch=*)
                ARCH_EXPLICIT=true
                local arch="${1#*=}"
                case "$arch" in
                    rv32 | RV32)
                        TEST_RV32=true
                        TEST_RV64=false
                        ;;
                    rv64 | RV64)
                        TEST_RV32=false
                        TEST_RV64=true
                        ;;
                    both | all)
                        TEST_RV32=true
                        TEST_RV64=true
                        ;;
                    *)
                        print_error "Invalid architecture: $arch"
                        exit 1
                        ;;
                esac
                ;;
            --help | -h)
                usage
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
        shift
    done

    # Auto-detect if no explicit --arch given
    if [[ "$ARCH_EXPLICIT" == "false" ]]; then
        apply_arch_detection
    fi
}

run_arch_tests()
{
    local arch="$1" test name
    print_header "Testing $arch Architecture"

    for test in "$SCRIPT_DIR"/gdbstub_*_test.sh; do
        name=$(basename "$test" _test.sh)
        name=${name#gdbstub_}
        print_step "Running $name test..."
        if ARCH="$arch" "$test"; then
            RESULTS+=("| $arch | $name | PASSED |")
            OVERALL_PASSED=$((OVERALL_PASSED + 1))
        else
            RESULTS+=("| $arch | $name | FAILED |")
            OVERALL_FAILED=$((OVERALL_FAILED + 1))
        fi
    done
}

main()
{
    parse_args "$@"

    print_header "GDB Stub Test Suite"

    [[ "$TEST_RV64" == "true" ]] && run_arch_tests "rv64"
    [[ "$TEST_RV32" == "true" ]] && run_arch_tests "rv32"

    print_header "Overall Test Summary"
    printf '  %s\n' "${RESULTS[@]}"
    echo -e "  ${BOLD}Total Passed: ${GREEN}$OVERALL_PASSED${NC}"
    echo -e "  ${BOLD}Total Failed: ${RED}$OVERALL_FAILED${NC}"

    # GitHub Actions shows this table on the run's summary page
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        printf '%s\n' "| Arch | Test | Status |" "|------|------|--------|" \
            "${RESULTS[@]}" >> "$GITHUB_STEP_SUMMARY"
    fi

    if [[ $OVERALL_FAILED -gt 0 ]]; then
        echo -e "\n  ${RED}${BOLD}SOME TESTS FAILED${NC}"
        exit 1
    else
        echo -e "\n  ${GREEN}${BOLD}ALL TESTS PASSED${NC}"
        exit 0
    fi
}

main "$@"
