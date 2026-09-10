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

# Autotools, the complete set vcpkg's own vcpkg-make helper asks for. Several
# ports in the dependency graph run autoreconf: gperf, which fontconfig needs,
# and libxcrypt, which D-Bus needs and whose configure.ac uses
# LT_CONFIG_LTDL_DIR and so also wants the ltdl development files.
$SUDO apt-get install --no-install-recommends -y \
    autoconf \
    autoconf-archive \
    automake \
    libltdl-dev \
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

# Native Wayland is the primary Linux backend, and Qt needs D-Bus for
# portal-backed folder selection, desktop integration and the accessibility
# bridge.
$SUDO apt-get install --no-install-recommends -y \
    libdbus-1-dev \
    libwayland-dev \
    wayland-protocols

# Headless GUI testing. Weston is the deterministic reference compositor for
# the required native Wayland suite; Xvfb and a window manager serve the
# secondary X11 compatibility suite.
$SUDO apt-get install --no-install-recommends -y \
    dbus-daemon \
    openbox \
    wayland-utils \
    weston \
    x11-utils \
    xvfb

echo "Linux prerequisites installed. Qt itself comes from vcpkg."
