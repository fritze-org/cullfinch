#!/usr/bin/env bash
# Run a command inside an isolated, headless native Wayland session.
#
# This owns the whole session: a private runtime directory, its own compositor,
# its own D-Bus, and its own application-data and cache roots. Nothing it
# starts outlives it, and nothing on the developer's desktop is touched.
#
#   tests/support/with-wayland.sh ctest --preset dev-fast --label-regex gui
#
# CULLFINCH_COMPOSITOR selects which compositor owns the session:
#
#   weston   the deterministic reference compositor for the required checks
#   kwin     KDE Plasma, where decorations and real focus routing are exercised
#   mutter   GNOME, likewise
#
# Weston stays the default: a required pull-request check must not depend on a
# full desktop stack. What differs between the three is session setup, not the
# tests, so the extended compositor jobs come through here rather than growing
# a second, weaker copy of this file inside a workflow.
#
# Readiness is a real client connection, never the mere existence of a socket:
# a socket appears before the compositor is able to serve anyone. Mutter proves
# the point — it publishes its socket and *then* aborts when it has no session
# bus, so a socket check alone reports a dead compositor as four failing tests.
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

: "${CULLFINCH_COMPOSITOR:=weston}"
: "${CULLFINCH_OUTPUT_WIDTH:=1920}"
: "${CULLFINCH_OUTPUT_HEIGHT:=1080}"
: "${CULLFINCH_WAYLAND_TIMEOUT:=60}"

case "${CULLFINCH_COMPOSITOR}" in
    weston | kwin | mutter) ;;
    *)
        echo "unknown CULLFINCH_COMPOSITOR '${CULLFINCH_COMPOSITOR}':" \
            "expected weston, kwin or mutter" >&2
        exit 2
        ;;
esac

session_root="$(mktemp -d "${TMPDIR:-/tmp}/cullfinch-wayland-XXXXXX")"
chmod 0700 "${session_root}"

runtime_dir="${session_root}/runtime"
mkdir -p "${runtime_dir}" "${session_root}/data" "${session_root}/cache" "${session_root}/artifacts"
chmod 0700 "${runtime_dir}"

socket_name="cullfinch-${CULLFINCH_COMPOSITOR}-$$"
compositor_log="${session_root}/artifacts/${CULLFINCH_COMPOSITOR}.log"
compositor_pid=""
dbus_pid=""

artifact_dir="${CULLFINCH_ARTIFACT_DIR:-}"

cleanup() {
    local status=$?
    # Preserve evidence before tearing the session down.
    if [[ -n "${artifact_dir}" ]]; then
        mkdir -p "${artifact_dir}"
        cp -a "${session_root}/artifacts/." "${artifact_dir}/" 2>/dev/null || true
    elif [[ ${status} -ne 0 && -s "${compositor_log}" ]]; then
        echo "--- ${CULLFINCH_COMPOSITOR} log ---" >&2
        tail -n 40 "${compositor_log}" >&2 || true
    fi

    for pid in "${compositor_pid}" "${dbus_pid}"; do
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
# is not the developer's session bus. The desktop compositors do not merely
# prefer one: Mutter asserts on a missing session bus and aborts outright, so
# for them a bus that could not be started is a hard failure rather than a
# session with desktop integration switched off.
bus_required=0
if [[ "${CULLFINCH_COMPOSITOR}" != "weston" ]]; then
    bus_required=1
fi

# Drop a bus inherited from the caller before trying to start one. Otherwise a
# private bus that could not be started leaves the developer's own address in
# place: the requirement below is satisfied by it, and KWin or Mutter attach to
# the very session this script promises not to touch.
unset DBUS_SESSION_BUS_ADDRESS

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

if [[ ${bus_required} -eq 1 && -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    echo "${CULLFINCH_COMPOSITOR} requires a session bus, and no private" \
        "dbus-daemon could be started" >&2
    exit 1
fi

# Every compositor here is asked for a headless backend at an explicit output
# size, because the scaling tests define 100% inspection in buffer pixels and
# cannot infer the output they were given.
case "${CULLFINCH_COMPOSITOR}" in
    weston)
        # A software renderer, so the baseline job needs no GPU.
        weston \
            --backend=headless \
            --renderer=pixman \
            --width="${CULLFINCH_OUTPUT_WIDTH}" \
            --height="${CULLFINCH_OUTPUT_HEIGHT}" \
            --socket="${socket_name}" \
            --idle-time=0 \
            --no-config \
            >"${compositor_log}" 2>&1 &
        ;;
    kwin)
        # --virtual is KWin's headless backend. There is no --headless option;
        # asking for one prints "Unknown option 'headless'" and exits.
        kwin_wayland \
            --virtual \
            --width "${CULLFINCH_OUTPUT_WIDTH}" \
            --height "${CULLFINCH_OUTPUT_HEIGHT}" \
            --socket "${socket_name}" \
            >"${compositor_log}" 2>&1 &
        ;;
    mutter)
        # Headless Mutter has no output at all unless one is asked for, and
        # Xwayland is switched off for the same reason DISPLAY is unset below.
        mutter \
            --headless \
            --no-x11 \
            --virtual-monitor "${CULLFINCH_OUTPUT_WIDTH}x${CULLFINCH_OUTPUT_HEIGHT}" \
            --wayland-display "${socket_name}" \
            >"${compositor_log}" 2>&1 &
        ;;
esac
compositor_pid=$!

export WAYLAND_DISPLAY="${socket_name}"
# X11 must be genuinely unavailable: a job that quietly succeeds through
# XWayland has not tested the native path at all.
unset DISPLAY

# Wait for a real client to complete a connection, while checking that the
# compositor is still alive so a crash fails fast instead of at the deadline.
connected=0
deadline=$((SECONDS + CULLFINCH_WAYLAND_TIMEOUT))
while ((SECONDS < deadline)); do
    if ! kill -0 "${compositor_pid}" 2>/dev/null; then
        echo "${CULLFINCH_COMPOSITOR} exited before accepting a client" >&2
        tail -n 40 "${compositor_log}" >&2 || true
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
    tail -n 40 "${compositor_log}" >&2 || true
    exit 1
fi

# The sanitizer runtime options are inherited from the caller on purpose, and
# must stay that way: the sanitized CI job sets them once, and a session helper
# that rebuilt the environment from scratch would quietly disarm every test it
# launches. What is not inherited is the directory a log_path points into --
# the runtime writes "<log_path>.<pid>" but creates no directories, so a report
# from a test started here would be dropped on the floor. Create it, and say
# which options are in force so the artifacts explain themselves.
for sanitizer_options in "${ASAN_OPTIONS:-}" "${UBSAN_OPTIONS:-}" "${LSAN_OPTIONS:-}"; do
    [[ -n "${sanitizer_options}" ]] || continue
    log_path="${sanitizer_options##*log_path=}"
    [[ "${log_path}" != "${sanitizer_options}" ]] || continue
    log_path="${log_path%%:*}"
    if [[ -n "${log_path}" ]]; then
        mkdir -p "$(dirname "${log_path}")"
    fi
done
if [[ -n "${ASAN_OPTIONS:-}${UBSAN_OPTIONS:-}" ]]; then
    {
        echo "ASAN_OPTIONS=${ASAN_OPTIONS:-}"
        echo "UBSAN_OPTIONS=${UBSAN_OPTIONS:-}"
        echo "LSAN_OPTIONS=${LSAN_OPTIONS:-}"
    } >"${session_root}/artifacts/sanitizer-options.txt"
fi

# Select the native backend explicitly and make the tests assert they got it,
# so an XCB, offscreen or minimal fallback fails the suite rather than passing
# quietly under a different backend.
export CULLFINCH_COMPOSITOR
export QT_QPA_PLATFORM=wayland
export CULLFINCH_EXPECTED_PLATFORM=wayland
export CULLFINCH_TEST_MODE=1
export CULLFINCH_TEST_DATA_DIR="${session_root}/data"
export CULLFINCH_TEST_CACHE_DIR="${session_root}/cache"

"$@"
