#!/usr/bin/env bash
# Install the macOS prerequisites that are outside vcpkg's scope.
#
# This script must never install a second copy of Qt: vcpkg owns every
# application library.
set -euo pipefail

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required. See https://brew.sh" >&2
    exit 1
fi

# Autotools. Several vcpkg ports in the dependency graph run autoreconf --
# gperf, which fontconfig needs, and libxcrypt, which D-Bus needs. Homebrew's
# libtool ships the ltdl development files those ports look for.
brew install ninja pkg-config autoconf autoconf-archive automake libtool

if ! command -v uv >/dev/null 2>&1; then
    brew install uv
fi

# The pinned LLVM used for clang-tidy analysis. The Apple toolchain does not
# ship clang-tidy, and mixing LLVM versions changes which diagnostics appear.
brew install llvm@23 || brew install llvm

xcode-select --print-path >/dev/null 2>&1 || {
    echo "Command Line Tools are required: run 'xcode-select --install'" >&2
    exit 1
}

echo "macOS prerequisites installed. Qt itself comes from vcpkg."
