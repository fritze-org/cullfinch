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

# Qt's XCB platform plugin and its font/input stack.
$SUDO apt-get install --no-install-recommends -y \
    libdbus-1-dev \
    libegl1-mesa-dev \
    libfontconfig1-dev \
    libfreetype-dev \
    libgl1-mesa-dev \
    libglu1-mesa-dev \
    libinput-dev \
    libx11-dev \
    libx11-xcb-dev \
    libxcb-cursor-dev \
    libxcb-glx0-dev \
    libxcb-icccm4-dev \
    libxcb-image0-dev \
    libxcb-keysyms1-dev \
    libxcb-randr0-dev \
    libxcb-render-util0-dev \
    libxcb-render0-dev \
    libxcb-shape0-dev \
    libxcb-shm0-dev \
    libxcb-sync-dev \
    libxcb-util-dev \
    libxcb-xfixes0-dev \
    libxcb-xinerama0-dev \
    libxcb-xkb-dev \
    libxcb1-dev \
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
