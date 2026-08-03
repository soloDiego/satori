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
NAME="$(basename "$BIN")"
# nested_pids / nested_running / nested_kill: never signal satori by name alone,
# it would hit the live session's window manager. See the file for why.
# shellcheck source=lib-nested.sh
. "$(dirname "$0")/lib-nested.sh"

LOG="$(mktemp -t satori-test.XXXXXX.log)"
RIVER_LOG="$(mktemp -t river-test.XXXXXX.log)"
RIVER_PID=""
CLIENT_PID=""
FS_PID=""
LAYER_PID=""
FAILED=0

cleanup() {
    [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
    [ -n "$FS_PID" ] && kill "$FS_PID" 2>/dev/null
    [ -n "$LAYER_PID" ] && kill "$LAYER_PID" 2>/dev/null
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
# Without this bind river closes every layer surface, so bars and launchers
# never map -- and the client gives no error, it just never appears.
check "binds the layer shell global"   'bound river_layer_shell_v1 v[0-9]+'
check "sees an output"                 'wm: output'
check "sees a seat"                    'wm: seat'
# Layer surfaces that name no output need a default, or they have nowhere to go.
check "picks a default layer output"   'layer: default output'

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

    # Cycling has to raise, not just re-focus. Every window is maximized, so a
    # focused window that is not on top is invisible and keystrokes land in a
    # window you cannot see -- which is exactly how this looked broken.
    #
    # super+q above left one window, and focus_next on a single window is
    # correctly a no-op, so cycling needs a second one back.
    press super+return
    check_count "spawns a window to cycle to" 'wm: window' 3
    wait_for 'window: raised' 5
    sleep 0.5   # let the new window's own raise land before counting
    raises_before="$(grep -cE 'window: raised' "$LOG")"
    press super+j
    check "super+j triggers its binding"  'binding: pressed keysym 0x6a'
    check_count "cycling raises the newly focused window" \
        'window: raised' "$((raises_before + 1))"

    # Fullscreen comes from the client, not from us, so it needs a client that
    # asks. foot has no default fullscreen bind; -o gives it one, and the chord
    # has no super so it reaches foot rather than satori. It must start as a
    # normal window: --fullscreen would start it fullscreen, so it would never
    # be proposed, and the re-proposal below would happen either way.
    WAYLAND_DISPLAY="$NESTED" foot -o key-bindings.fullscreen=Control+Shift+f \
        sh -c 'sleep 60' >/dev/null 2>&1 &
    FS_PID=$!
    check_count "sees the fullscreen test window" 'wm: window' 4

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
    check_count "sees the fullscreen test window close" 'window: closed' 2

    # The other half: fullscreen driven by us, not by the client. Counting from
    # here, because the client round trip above already logged one of each.
    fs_on_before="$(grep -cE 'window: fullscreen on' "$LOG")"
    fs_off_before="$(grep -cE 'window: fullscreen off' "$LOG")"
    props_before="$(grep -cE 'window: propose' "$LOG")"

    press super+f
    check       "super+f triggers its binding"   'binding: pressed keysym 0x66'
    check_count "super+f fullscreens the focused window" \
        'window: fullscreen on' "$((fs_on_before + 1))"

    # Toggling has to come back off. windows_apply_fullscreen clears the dirty
    # flag after applying, so a toggle that sets it only once goes fullscreen and
    # stays there -- with no bind left to escape it.
    press super+f
    check_count "super+f toggles back out of fullscreen" \
        'window: fullscreen off' "$((fs_off_before + 1))"
    check_count "re-proposes after the toggle" 'window: propose' "$((props_before + 1))"
else
    echo "  ..    skipping key bindings (no $KEYPRESS; run make)"
fi

# A real layer surface end to end. fuzzel takes exclusive keyboard focus, which
# is the case that matters: satori has to stop driving focus while it is up and
# take it back afterwards. Its lock file is per-$WAYLAND_DISPLAY, so this cannot
# collide with a fuzzel in the outer session.
if command -v fuzzel >/dev/null; then
    focus_before="$(grep -cE 'seat: focus window' "$LOG")"

    WAYLAND_DISPLAY="$NESTED" fuzzel >/dev/null 2>&1 &
    LAYER_PID=$!

    check "a layer surface takes focus" 'layer: focus (exclusive|non-exclusive)'

    kill "$LAYER_PID" 2>/dev/null
    LAYER_PID=""

    check "notices the layer surface let go" 'layer: focus none'
    # The deferred focus has to actually land, not just stop being deferred.
    check_count "returns focus to a window"  'seat: focus window' "$((focus_before + 1))"
else
    echo "  ..    skipping layer shell (fuzzel not installed)"
fi

# Closing the client must exercise win_closed without taking satori down.
kill "$CLIENT_PID" 2>/dev/null
CLIENT_PID=""
sleep 1
if nested_running; then
    echo "  ok    survives the client closing"
else
    echo "  FAIL  satori died when the client closed"
    FAILED=1
fi

# Clean shutdown: SIGINT -> stop -> finished -> exit.
nested_kill INT
check "shuts down cleanly on SIGINT"   'wm: finished'
sleep 1
if nested_running; then
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
