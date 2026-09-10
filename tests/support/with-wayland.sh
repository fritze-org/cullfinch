#!/usr/bin/env bash
# Run a command inside an isolated, headless native Wayland session.
#
# This owns the whole session: a private runtime directory, its own compositor,
# its own D-Bus, and its own application-data and cache roots. Nothing it
# starts outlives it, and nothing on the developer's desktop is touched.
#
#   tests/support/with-wayland.sh ctest --preset dev-fast --label-regex gui
#
# Readiness is a real client connection, never the mere existence of a socket:
# a socket appears before the compositor is able to serve anyone.
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

: "${CULLFINCH_WESTON_WIDTH:=1920}"
: "${CULLFINCH_WESTON_HEIGHT:=1080}"
: "${CULLFINCH_WAYLAND_TIMEOUT:=60}"

session_root="$(mktemp -d "${TMPDIR:-/tmp}/cullfinch-wayland-XXXXXX")"
chmod 0700 "${session_root}"

runtime_dir="${session_root}/runtime"
mkdir -p "${runtime_dir}" "${session_root}/data" "${session_root}/cache" "${session_root}/artifacts"
chmod 0700 "${runtime_dir}"

socket_name="cullfinch-wl-$$"
weston_log="${session_root}/artifacts/weston.log"
weston_pid=""
dbus_pid=""

artifact_dir="${CULLFINCH_ARTIFACT_DIR:-}"

cleanup() {
    local status=$?
    # Preserve evidence before tearing the session down.
    if [[ -n "${artifact_dir}" ]]; then
        mkdir -p "${artifact_dir}"
        cp -a "${session_root}/artifacts/." "${artifact_dir}/" 2>/dev/null || true
    elif [[ ${status} -ne 0 && -s "${weston_log}" ]]; then
        echo "--- weston log ---" >&2
        tail -n 40 "${weston_log}" >&2 || true
    fi

    for pid in "${weston_pid}" "${dbus_pid}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done

    rm -rf "${session_root}"
    return ${status}
}
trap cleanup EXIT INT TERM

export XDG_RUNTIME_DIR="${runtime_dir}"

# A private bus, so portal and desktop integration have somewhere to talk that
# is not the developer's session bus.
if command -v dbus-daemon >/dev/null 2>&1; then
    dbus_address_file="${session_root}/dbus-address"
    dbus-daemon --session --nofork --print-address=3 --print-pid=4 \
        3>"${dbus_address_file}" 4>"${session_root}/dbus-pid" &
    dbus_pid=$!
    for _ in $(seq 1 50); do
        [[ -s "${dbus_address_file}" ]] && break
        sleep 0.1
    done
    if [[ -s "${dbus_address_file}" ]]; then
        DBUS_SESSION_BUS_ADDRESS="$(cat "${dbus_address_file}")"
        export DBUS_SESSION_BUS_ADDRESS
    fi
fi

# The headless backend with a software renderer, so the baseline job needs no
# GPU, at an explicit output size the scaling tests can rely on.
weston \
    --backend=headless \
    --renderer=pixman \
    --width="${CULLFINCH_WESTON_WIDTH}" \
    --height="${CULLFINCH_WESTON_HEIGHT}" \
    --socket="${socket_name}" \
    --idle-time=0 \
    --no-config \
    >"${weston_log}" 2>&1 &
weston_pid=$!

export WAYLAND_DISPLAY="${socket_name}"
# X11 must be genuinely unavailable: a job that quietly succeeds through
# XWayland has not tested the native path at all.
unset DISPLAY

# Wait for a real client to complete a connection, while checking that the
# compositor is still alive so a crash fails fast instead of at the deadline.
connected=0
deadline=$((SECONDS + CULLFINCH_WAYLAND_TIMEOUT))
while ((SECONDS < deadline)); do
    if ! kill -0 "${weston_pid}" 2>/dev/null; then
        echo "weston exited before accepting a client" >&2
        tail -n 40 "${weston_log}" >&2 || true
        exit 1
    fi
    if command -v wayland-info >/dev/null 2>&1; then
        if wayland-info >/dev/null 2>&1; then
            connected=1
            break
        fi
    elif [[ -S "${runtime_dir}/${socket_name}" ]]; then
        # No wayland-info available: connect to the socket directly. Weaker
        # than a protocol roundtrip, and reported as such.
        if python3 - "${runtime_dir}/${socket_name}" <<'PY'; then
import socket
import sys

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
    probe.settimeout(2)
    probe.connect(sys.argv[1])
PY
            echo "note: wayland-info is not installed; readiness verified by socket connect only" >&2
            connected=1
            break
        fi
    fi
    sleep 0.2
done

if [[ ${connected} -ne 1 ]]; then
    echo "no Wayland client could connect to '${socket_name}' within ${CULLFINCH_WAYLAND_TIMEOUT}s" >&2
    tail -n 40 "${weston_log}" >&2 || true
    exit 1
fi

# Select the native backend explicitly and make the tests assert they got it,
# so an XCB, offscreen or minimal fallback fails the suite rather than passing
# quietly under a different backend.
export QT_QPA_PLATFORM=wayland
export CULLFINCH_EXPECTED_PLATFORM=wayland
export CULLFINCH_TEST_MODE=1
export CULLFINCH_TEST_DATA_DIR="${session_root}/data"
export CULLFINCH_TEST_CACHE_DIR="${session_root}/cache"

"$@"
