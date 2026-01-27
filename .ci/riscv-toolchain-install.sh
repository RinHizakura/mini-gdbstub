#!/usr/bin/env bash
#
# RISC-V Toolchain Installation Script
#
# Downloads and installs RISC-V GNU toolchain with caching support.
# Toolchains are cached in TOOLCHAIN_CACHE_DIR to speed up CI builds.
#
# Usage:
#   .ci/riscv-toolchain-install.sh [OPTIONS]
#
# Options:
#   --arch=ARCH     Target architecture: rv32, rv64, or both (default: both)
#   --dest=DIR      Installation directory (default: /opt/riscv)
#   --cache=DIR     Cache directory (default: ~/.cache/riscv-toolchain)
#   --force         Force re-download even if cached
#   --help          Show this help message
#
# Environment Variables:
#   TOOLCHAIN_VERSION   Release tag (default: latest from GitHub)
#   TOOLCHAIN_DEST      Installation directory
#   TOOLCHAIN_CACHE_DIR Cache directory
#   GH_TOKEN            GitHub token for API requests (optional)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/common.sh"

# Configuration defaults
TOOLCHAIN_VERSION="${TOOLCHAIN_VERSION:-}"
TOOLCHAIN_DEST="${TOOLCHAIN_DEST:-/opt/riscv}"
TOOLCHAIN_CACHE_DIR="${TOOLCHAIN_CACHE_DIR:-$HOME/.cache/riscv-toolchain}"
INSTALL_RV32=false
INSTALL_RV64=false
FORCE_DOWNLOAD=false

# GitHub release info
GITHUB_REPO="riscv-collab/riscv-gnu-toolchain"
GITHUB_API="https://api.github.com/repos/$GITHUB_REPO/releases/latest"
GITHUB_DOWNLOAD="https://github.com/$GITHUB_REPO/releases/download"

usage()
{
    sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'
    exit 0
}

parse_args()
{
    local arch="both"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --arch=*)
                arch="${1#*=}"
                ;;
            --dest=*)
                TOOLCHAIN_DEST="${1#*=}"
                ;;
            --cache=*)
                TOOLCHAIN_CACHE_DIR="${1#*=}"
                ;;
            --force)
                FORCE_DOWNLOAD=true
                ;;
            --help | -h)
                usage
                ;;
            *)
                print_error "Unknown option: $1"
                usage
                ;;
        esac
        shift
    done

    case "$arch" in
        rv32 | RV32)
            INSTALL_RV32=true
            ;;
        rv64 | RV64)
            INSTALL_RV64=true
            ;;
        both | all)
            INSTALL_RV32=true
            INSTALL_RV64=true
            ;;
        *)
            print_error "Invalid architecture: $arch"
            exit 1
            ;;
    esac
}

# Detect Ubuntu version for toolchain selection
detect_ubuntu_version()
{
    if [[ -f /etc/os-release ]]; then
        . /etc/os-release
        if [[ "$ID" == "ubuntu" ]]; then
            case "$VERSION_ID" in
                24.04*) echo "ubuntu-24.04" ;;
                22.04*) echo "ubuntu-22.04" ;;
                *) echo "ubuntu-24.04" ;; # Fallback to latest
            esac
            return
        fi
    fi
    echo "ubuntu-24.04" # Default fallback
}

# Fetch latest release version from GitHub
fetch_latest_version()
{
    local api_url="$GITHUB_API"
    local -a curl_opts=(-fsSL --retry 3 --retry-delay 2 --max-time 30)

    if [[ -n "${GH_TOKEN:-}" ]]; then
        curl_opts+=(-H "Authorization: token $GH_TOKEN")
    fi

    print_step "Fetching latest toolchain version..."

    local response
    response=$(curl "${curl_opts[@]}" "$api_url" 2> /dev/null) || true

    if [[ -z "$response" ]]; then
        print_warning "Failed to fetch from GitHub API, using fallback version"
        echo "2025.01.20" # Fallback version
        return
    fi

    echo "$response" | grep -o '"tag_name": *"[^"]*"' | head -1 | cut -d'"' -f4
}

# Get download URL for toolchain
get_toolchain_url()
{
    local arch="$1"
    local version="$2"
    local ubuntu_ver="$3"

    local filename="${arch}-elf-${ubuntu_ver}-gcc-nightly-${version}-nightly.tar.xz"
    echo "${GITHUB_DOWNLOAD}/${version}/${filename}"
}

# Check if toolchain is cached and valid
is_cached()
{
    local cache_file="$1"
    local version_file="${cache_file}.version"

    if [[ ! -f "$cache_file" ]]; then
        return 1
    fi

    # Check version marker
    if [[ -f "$version_file" ]]; then
        local cached_version
        cached_version=$(cat "$version_file")
        if [[ "$cached_version" == "$TOOLCHAIN_VERSION" ]]; then
            return 0
        fi
    fi

    return 1
}

# Download toolchain with caching
download_toolchain()
{
    local arch="$1"
    local version="$2"
    local ubuntu_ver="$3"

    local url
    url=$(get_toolchain_url "$arch" "$version" "$ubuntu_ver")
    local filename="${arch}-elf-${ubuntu_ver}-gcc-nightly-${version}-nightly.tar.xz"
    local cache_file="${TOOLCHAIN_CACHE_DIR}/${filename}"
    local version_file="${cache_file}.version"

    mkdir -p "$TOOLCHAIN_CACHE_DIR"

    if [[ "$FORCE_DOWNLOAD" == "false" ]] && is_cached "$cache_file"; then
        print_info "Using cached toolchain: $filename"
        echo "$cache_file"
        return 0
    fi

    print_step "Downloading $arch toolchain ($version)..."
    print_info "URL: $url"

    if ! download_with_progress "$url" "$cache_file"; then
        print_error "Failed to download toolchain"
        rm -f "$cache_file"
        return 1
    fi

    # Write version marker
    echo "$version" > "$version_file"

    print_success "Downloaded: $filename"
    echo "$cache_file"
}

# Extract and install toolchain
install_toolchain()
{
    local archive="$1"
    local dest="$2"
    local arch="$3"

    local install_dir="${dest}/${arch}"

    print_step "Installing $arch toolchain to $install_dir..."

    mkdir -p "$install_dir"

    # Extract based on file extension
    case "$archive" in
        *.tar.xz)
            tar -xJf "$archive" -C "$install_dir" --strip-components=1
            ;;
        *.tar.gz)
            tar -xzf "$archive" -C "$install_dir" --strip-components=1
            ;;
        *)
            print_error "Unknown archive format: $archive"
            return 1
            ;;
    esac

    # Verify installation
    local gcc="${install_dir}/bin/${arch}-unknown-elf-gcc"
    if [[ ! -x "$gcc" ]]; then
        print_error "Installation verification failed: $gcc not found"
        return 1
    fi

    print_success "Installed: $($gcc --version | head -1)"
}

# Create convenience symlinks
create_symlinks()
{
    local dest="$1"

    print_step "Creating symlinks..."

    # Create bin directory with symlinks to all toolchain binaries
    local bin_dir="${dest}/bin"
    mkdir -p "$bin_dir"

    for arch_dir in "${dest}"/riscv{32,64}; do
        if [[ -d "${arch_dir}/bin" ]]; then
            for tool in "${arch_dir}/bin"/*; do
                if [[ -x "$tool" ]]; then
                    local name
                    name=$(basename "$tool")
                    ln -sf "$tool" "${bin_dir}/${name}" 2> /dev/null || true
                fi
            done
        fi
    done

    print_success "Symlinks created in $bin_dir"
}

# Print PATH export instructions
print_setup_instructions()
{
    local dest="$1"

    echo ""
    print_header "Setup Instructions"
    echo "Add the following to your shell profile (~/.bashrc, ~/.zshrc, etc.):"
    echo ""
    echo "  export PATH=\"${dest}/bin:\$PATH\""
    echo ""
    echo "Or for specific architectures:"
    if [[ "$INSTALL_RV32" == "true" ]]; then
        echo "  export CROSS_COMPILE=riscv32-unknown-elf-"
        echo "  export PATH=\"${dest}/riscv32/bin:\$PATH\""
    fi
    if [[ "$INSTALL_RV64" == "true" ]]; then
        echo "  export CROSS_COMPILE=riscv64-unknown-elf-"
        echo "  export PATH=\"${dest}/riscv64/bin:\$PATH\""
    fi
    echo ""
}

main()
{
    parse_args "$@"

    print_header "RISC-V Toolchain Installation"

    check_platform
    print_info "Platform: $PLATFORM ($MACH)"

    # Only Linux with riscv-collab toolchains
    if [[ "$PLATFORM" != "Linux" ]]; then
        print_error "This script only supports Linux"
        print_info "For macOS, use: brew install riscv-gnu-toolchain"
        print_info "Or xPack toolchains: https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack"
        exit 1
    fi

    local ubuntu_ver
    ubuntu_ver=$(detect_ubuntu_version)
    print_info "Ubuntu version: $ubuntu_ver"

    # Get toolchain version
    if [[ -z "$TOOLCHAIN_VERSION" ]]; then
        TOOLCHAIN_VERSION=$(fetch_latest_version)
    fi
    print_info "Toolchain version: $TOOLCHAIN_VERSION"

    # Install RV32 toolchain
    if [[ "$INSTALL_RV32" == "true" ]]; then
        local rv32_archive
        rv32_archive=$(download_toolchain "riscv32" "$TOOLCHAIN_VERSION" "$ubuntu_ver")
        install_toolchain "$rv32_archive" "$TOOLCHAIN_DEST" "riscv32"
    fi

    # Install RV64 toolchain
    if [[ "$INSTALL_RV64" == "true" ]]; then
        local rv64_archive
        rv64_archive=$(download_toolchain "riscv64" "$TOOLCHAIN_VERSION" "$ubuntu_ver")
        install_toolchain "$rv64_archive" "$TOOLCHAIN_DEST" "riscv64"
    fi

    create_symlinks "$TOOLCHAIN_DEST"
    print_setup_instructions "$TOOLCHAIN_DEST"

    print_success "Installation complete!"
}

main "$@"
