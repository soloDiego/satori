#!/usr/bin/env bash
# Automated smoke test: run satori as the WM of a headless nested river,
# spawn a client, close it, shut down cleanly. Asserts on satori's log.
#
# Headless => no window on screen, no interference with the outer session.
# Usage: scripts/test-nested.sh [./satori | ./satori-asan]

set -uo pipefail

BIN="${1:-./satori}"
[ -x "$BIN" ] || { echo "no such binary: $BIN (run make)" >&2; exit 1; }
BIN="$(realpath "$BIN")"
# Match satori by exact process name: river's own argv contains $BIN (it is the
# -c init command), so a `pkill -f "$BIN"` would signal the compositor too.
NAME="$(basename "$BIN")"

LOG="$(mktemp -t satori-test.XXXXXX.log)"
RIVER_LOG="$(mktemp -t river-test.XXXXXX.log)"
RIVER_PID=""
CLIENT_PID=""
FAILED=0

cleanup() {
    [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
    [ -n "$RIVER_PID" ] && kill "$RIVER_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

# Wait until $1 (regex) shows up in the satori log, or $2 seconds pass.
wait_for() {
    local pattern="$1" secs="${2:-5}" i
    for ((i = 0; i < secs * 10; i++)); do
        grep -qE "$pattern" "$LOG" && return 0
        sleep 0.1
    done
    return 1
}

check() {
    local desc="$1" pattern="$2"
    if wait_for "$pattern" 5; then
        echo "  ok    $desc"
    else
        echo "  FAIL  $desc (no /$pattern/ in log)"
        FAILED=1
    fi
}

echo "== satori nested-river smoke test ($BIN)"

sockets_before="$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | sort)"

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    river -c "$BIN 2>$LOG" >"$RIVER_LOG" 2>&1 &
RIVER_PID=$!

# The nested compositor's socket is whichever one is new.
NESTED=""
for _ in {1..50}; do
    NESTED="$(comm -13 <(echo "$sockets_before") \
        <(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | sort) | head -1)"
    [ -n "$NESTED" ] && break
    sleep 0.1
done
[ -n "$NESTED" ] || { echo "  FAIL  nested river never came up; see $RIVER_LOG"; exit 1; }
echo "  ..    nested river on \$WAYLAND_DISPLAY=$NESTED (pid $RIVER_PID)"

check "binds as active window manager" 'bound river_window_manager_v1 v4'
check "sees an output"                 'wm: output'

# Spawn a client into the nested compositor.
WAYLAND_DISPLAY="$NESTED" foot sh -c 'sleep 60' >/dev/null 2>&1 &
CLIENT_PID=$!

check "sees the new window"            'wm: window'
check "window gets dimensions"         'window: [0-9]+x[0-9]+'   # Stage C: proof it renders

# Closing the client must exercise win_closed without taking satori down.
kill "$CLIENT_PID" 2>/dev/null
CLIENT_PID=""
sleep 1
if pgrep -x "$NAME" >/dev/null; then
    echo "  ok    survives the client closing"
else
    echo "  FAIL  satori died when the client closed"
    FAILED=1
fi

# Clean shutdown: SIGINT -> stop -> finished -> exit.
pkill -INT -x "$NAME"
check "shuts down cleanly on SIGINT"   'wm: finished'
sleep 1
if pgrep -x "$NAME" >/dev/null; then
    echo "  FAIL  satori still running after SIGINT"
    FAILED=1
else
    echo "  ok    process exited"
fi

# ASan/LSan write to stderr, which is this log. Any hit = a real finding.
if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error' "$LOG"; then
    echo "  FAIL  sanitizer findings:"
    grep -E 'ERROR:|SUMMARY:' "$LOG" | sed 's/^/          /'
    FAILED=1
elif [[ "$BIN" == *asan* ]]; then
    echo "  ok    no leaks (asan clean)"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "PASS  (log: $LOG)"
else
    echo "FAIL  (log: $LOG, river: $RIVER_LOG)"
fi
exit "$FAILED"
