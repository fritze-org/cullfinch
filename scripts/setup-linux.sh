#!/usr/bin/env bash
# Install the Linux prerequisites that are outside vcpkg's scope: compilers,
# Git, and the desktop development libraries Qt builds against.
#
# This script must never install a second copy of Qt: vcpkg owns every
# application library.
set -euo pipefail

SUDO=""
if [[ $EUID -ne 0 ]]; then
    SUDO="sudo"
fi

$SUDO apt-get update

$SUDO apt-get install --no-install-recommends -y \
    build-essential \
    ca-certificates \
    curl \
    git \
    ninja-build \
    pkg-config \
    python3 \
    tar \
    unzip \
    zip

# Autotools. Several vcpkg ports in the dependency graph -- gperf, which
# fontconfig needs -- run autoreconf and fail without these.
$SUDO apt-get install --no-install-recommends -y \
    autoconf \
    autoconf-archive \
    automake \
    libtool \
    m4

# Qt's XCB platform plugin and its font stack.
#
# The libxcb set is installed by pattern, exactly as the vcpkg qtbase port
# documents. A hand-written enumeration got this wrong once already: the
# xcb-sm and system-xcb-xinput features both failed their conditions because
# libsm-dev and libxcb-xinput-dev were not on the list.
$SUDO apt-get install --no-install-recommends -y \
    '^libxcb.*-dev' \
    libegl1-mesa-dev \
    libfontconfig1-dev \
    libfreetype-dev \
    libgl1-mesa-dev \
    libglu1-mesa-dev \
    libice-dev \
    libsm-dev \
    libx11-dev \
    libx11-xcb-dev \
    libxext-dev \
    libxfixes-dev \
    libxi-dev \
    libxkbcommon-dev \
    libxkbcommon-x11-dev \
    libxrender-dev

# Wayland support, so the baseline package works on both display servers.
$SUDO apt-get install --no-install-recommends -y \
    libwayland-dev \
    wayland-protocols

# Headless GUI testing under the real XCB plugin.
$SUDO apt-get install --no-install-recommends -y \
    openbox \
    x11-utils \
    xvfb

echo "Linux prerequisites installed. Qt itself comes from vcpkg."
