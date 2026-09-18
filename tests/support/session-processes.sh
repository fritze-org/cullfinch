# shellcheck shell=bash
#
# Ending a test session's processes, for the session helpers beside this file.
# Sourced, never executed.
#
# Killing by pid is not enough, and that is the whole reason this exists.
#
# dbus-daemon activates services on demand, and on a desktop machine the
# private bus in with-wayland.sh activates the portal backend and a wallet
# daemon as soon as a Qt application connects. They are dbus-daemon's children,
# not the helper's, so signalling the daemon leaves them running. Compositors
# and window managers fork helpers of their own for the same reason.
#
# What survives is not merely untidy. It holds the session's stdout, so a piped
# invocation --
#
#     tests/support/with-wayland.sh ctest ... | tail
#
# -- then waits forever for an EOF that never comes, and every run strands
# another set of daemons on a bus nothing will ever reap. Both were observed on
# a KDE desktop: a leaked xdg-desktop-portal-kde and ksecretd per GUI run, and
# an Xvfb/openbox pair still alive a week after the run that started them.
#
# So each long-lived process is started under `set -m`, which makes it a
# process group leader, and end_session() signals the group.

# This shell's own process group, which is never signalled: it holds the helper
# and, on a developer's machine, the shell or ctest run that invoked it.
# Recorded at source time, before anything has been started.
cullfinch_own_group="$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')"

# end_session <pid>
#
# End the process and everything it brought with it. Safe to call with an empty
# or already-dead pid.
end_session() {
    local pid="$1" group
    [[ -n "${pid}" ]] || return 0
    group="$(ps -o pgid= -p "${pid}" 2>/dev/null | tr -d ' ')"

    # A process not leading a group of its own would mean signalling this
    # shell's group, and with it the caller. Fall back to the pid alone.
    if [[ -z "${group}" || -z "${cullfinch_own_group}" ||
        "${group}" == "${cullfinch_own_group}" ]]; then
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
        return 0
    fi

    kill -TERM -- "-${group}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true

    # An activated service is under no obligation to honour SIGTERM promptly.
    # Wait for the group to empty, then insist: a straggler here is exactly the
    # leak this function exists to prevent.
    local _
    for _ in $(seq 1 30); do
        kill -0 -- "-${group}" 2>/dev/null || return 0
        sleep 0.1
    done
    kill -KILL -- "-${group}" 2>/dev/null || true
}
