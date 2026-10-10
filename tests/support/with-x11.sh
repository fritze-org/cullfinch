#!/usr/bin/env bash
# Run a command inside an isolated X11 session, for the secondary compatibility
# suite. Native Wayland is the primary Linux backend; this exists so the XCB
# path keeps working, and is never a substitute for the Wayland job.
#
#   tests/support/with-x11.sh ctest --preset dev-fast --label-regex gui
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

: "${CULLFINCH_X11_RESOLUTION:=1920x1080x24}"
: "${CULLFINCH_X11_TIMEOUT:=30}"

session_root="$(mktemp -d "${TMPDIR:-/tmp}/cullfinch-x11-XXXXXX")"
chmod 0700 "${session_root}"
mkdir -p "${session_root}/data" "${session_root}/cache"

# A free display number, so concurrent runs never collide.
display_number=99
while [[ -e "/tmp/.X11-unix/X${display_number}" ]]; do
    display_number=$((display_number + 1))
done
display=":${display_number}"

xvfb_pid=""
wm_pid=""

# Ending the session's processes is subtler than killing two pids; the shared
# helper beside this file explains why and provides end_session(). An Xvfb and
# openbox pair from an interrupted run was found still alive a week later,
# which is what this closes here.
# shellcheck source=tests/support/session-processes.sh
source "$(dirname "${BASH_SOURCE[0]}")/session-processes.sh"

cleanup() {
    local status=$?
    # The window manager first, so it is not left talking to a display that
    # has already gone.
    end_session "${wm_pid}"
    end_session "${xvfb_pid}"
    rm -rf "${session_root}"
    return ${status}
}
trap cleanup EXIT INT TERM

# `set -m` puts each of these in a process group of its own, so anything they
# fork is ended with them rather than outliving the session. Job control is
# switched straight back off: the test command runs in the foreground and has
# no business being handed a terminal.
set -m
Xvfb "${display}" -screen 0 "${CULLFINCH_X11_RESOLUTION}" -nolisten tcp &
xvfb_pid=$!
set +m

export DISPLAY="${display}"
unset WAYLAND_DISPLAY

deadline=$((SECONDS + CULLFINCH_X11_TIMEOUT))
while ((SECONDS < deadline)); do
    if ! kill -0 "${xvfb_pid}" 2>/dev/null; then
        echo "Xvfb exited before ${display} became usable" >&2
        exit 1
    fi
    xdpyinfo -display "${display}" >/dev/null 2>&1 && break
    sleep 0.2
done

if ! xdpyinfo -display "${display}" >/dev/null 2>&1; then
    echo "Xvfb did not become ready on ${display}" >&2
    exit 1
fi

# Fullscreen transitions need a cooperating window manager.
set -m
openbox --sm-disable &
wm_pid=$!
set +m

deadline=$((SECONDS + CULLFINCH_X11_TIMEOUT))
while ((SECONDS < deadline)); do
    xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1 && break
    sleep 0.2
done

if ! xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1; then
    echo "no window manager claimed ${display}" >&2
    exit 1
fi

export QT_QPA_PLATFORM=xcb
export CULLFINCH_EXPECTED_PLATFORM=xcb
export CULLFINCH_TEST_MODE=1
export CULLFINCH_TEST_DATA_DIR="${session_root}/data"
export CULLFINCH_TEST_CACHE_DIR="${session_root}/cache"

"$@"
