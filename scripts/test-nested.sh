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
FS_PID=""
FAILED=0

cleanup() {
    [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
    [ -n "$FS_PID" ] && kill "$FS_PID" 2>/dev/null
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

# Same, but the pattern must appear at least $3 times -- for events that repeat
# (a second window, a second focus change).
check_count() {
    local desc="$1" pattern="$2" want="$3" i got
    for ((i = 0; i < 50; i++)); do
        got="$(grep -cE "$pattern" "$LOG")"
        [ "$got" -ge "$want" ] && { echo "  ok    $desc"; return; }
        sleep 0.1
    done
    echo "  FAIL  $desc (/$pattern/ x$got, want $want)"
    FAILED=1
}

# Inject a chord into the nested compositor. The headless backend has no
# keyboard, so this is the only way to trigger a binding.
KEYPRESS="$(dirname "$0")/../build/keypress"
press() {
    WAYLAND_DISPLAY="$NESTED" "$KEYPRESS" "$1" >/dev/null 2>&1
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
check "binds the xkb bindings global"  'bound river_xkb_bindings_v1 v[0-9]+'
check "sees an output"                 'wm: output'
check "sees a seat"                    'wm: seat'

# Spawn a client into the nested compositor.
WAYLAND_DISPLAY="$NESTED" foot sh -c 'sleep 60' >/dev/null 2>&1 &
CLIENT_PID=$!

check "sees the new window"            'wm: window'
check "window gets dimensions"         'window: [0-9]+x[0-9]+'   # proof propose_dimensions landed
check "focuses the new window"         'seat: focus window'

# Key bindings, end to end: a real key event through the compositor into an
# action. Everything above this proves bindings exist, not that they fire.
if [ -x "$KEYPRESS" ]; then
    press super+return
    check       "super+return triggers its binding" 'binding: pressed keysym 0xff0d'
    check_count "super+return spawns a window"      'wm: window' 2

    press super+q
    check "super+q triggers its binding"  'binding: pressed keysym 0x71'
    check "super+q closes the focused window" 'window: closed'

    press super+j
    check "super+j triggers its binding"  'binding: pressed keysym 0x6a'

    # Fullscreen comes from the client, not from us, so it needs a client that
    # asks. foot has no default fullscreen bind; -o gives it one, and the chord
    # has no super so it reaches foot rather than satori. It must start as a
    # normal window: --fullscreen would start it fullscreen, so it would never
    # be proposed, and the re-proposal below would happen either way.
    WAYLAND_DISPLAY="$NESTED" foot -o key-bindings.fullscreen=Control+Shift+f \
        sh -c 'sleep 60' >/dev/null 2>&1 &
    FS_PID=$!
    check_count "sees the fullscreen test window" 'wm: window' 3

    press ctrl+shift+f
    check "honors a fullscreen request"   'window: fullscreen on'

    # Leaving fullscreen must re-propose, or the window keeps the undefined
    # dimensions the protocol leaves it with. Assert on the proposal itself:
    # the dimensions event answering it is indistinguishable from the one
    # entering fullscreen already produced. Counting from here is safe -- the
    # window was proposed before it went fullscreen, above.
    props_before="$(grep -cE 'window: propose' "$LOG")"
    press ctrl+shift+f
    check "honors leaving fullscreen"     'window: fullscreen off'
    check_count "re-proposes after fullscreen" 'window: propose' "$((props_before + 1))"

    kill "$FS_PID" 2>/dev/null
    FS_PID=""
else
    echo "  ..    skipping key bindings (no $KEYPRESS; run make)"
fi

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
