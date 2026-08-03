# Shared by the test scripts. Source it; do not run it.
#
# Finding the satori under test is safety-critical, which is why it lives in one
# place instead of being copied into each script.
#
# Satori is the developer's real window manager, so a test instance and the live
# session have the same process name. `pkill -x satori` matches both, and killing
# the live one takes down the desktop the tests are running on -- that really
# happened. `pkill -f` is worse still: river's argv contains satori's path when
# it is the -c command, so -f signals the compositor too.
#
# River exports WAYLAND_DISPLAY to the child it spawns with -c, so the nested
# instance is the one whose environment names the nested socket. That survives
# river exiting and the process being reparented to init, which is what
# test-exit.sh needs -- an ancestor walk would not.
#
# Needs $NAME (process name) and $NESTED (the nested wayland socket) in scope.
nested_pids() {
    local pid env_display
    # No nested socket yet (an early exit, before river came up) means we cannot
    # tell the instances apart. Match nothing: leaking a test process is a far
    # cheaper mistake than killing the live session.
    [ -n "${NESTED:-}" ] || return 0
    for pid in $(pgrep -x "$NAME" 2>/dev/null); do
        env_display="$(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null \
            | grep -m1 '^WAYLAND_DISPLAY=')"
        [ "$env_display" = "WAYLAND_DISPLAY=$NESTED" ] && echo "$pid"
    done
}

# True while the nested satori is alive.
nested_running() {
    [ -n "$(nested_pids)" ]
}

# Signal only the nested satori. $1 is the signal, default TERM.
nested_kill() {
    local sig="${1:-TERM}" pids
    pids="$(nested_pids)"
    [ -n "$pids" ] && kill "-$sig" $pids 2>/dev/null
    return 0
}
