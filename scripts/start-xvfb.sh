#!/usr/bin/env bash
# Start Xvfb and a lightweight window manager, then wait until both are
# actually usable.
#
# Fullscreen tests need a cooperating window manager, and waiting on a real
# condition rather than sleeping is what keeps the GUI jobs deterministic.
set -euo pipefail

display="${1:-:99}"
resolution="${2:-1920x1080x24}"

Xvfb "${display}" -screen 0 "${resolution}" -nolisten tcp &
xvfb_pid=$!
echo "xvfb_pid=${xvfb_pid}"

export DISPLAY="${display}"

for _ in $(seq 1 100); do
    if xdpyinfo -display "${display}" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

if ! xdpyinfo -display "${display}" >/dev/null 2>&1; then
    echo "Xvfb did not become ready on ${display}" >&2
    exit 1
fi

openbox --sm-disable &
wm_pid=$!
echo "wm_pid=${wm_pid}"

# Wait for the window manager to claim the screen, so fullscreen transitions
# are honoured rather than silently ignored.
for _ in $(seq 1 100); do
    if xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1; then
        echo "display ${display} is ready with a window manager"
        exit 0
    fi
    sleep 0.2
done

echo "no window manager claimed ${display}" >&2
exit 1
