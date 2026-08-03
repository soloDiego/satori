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
APP_PID=""
LAYER_PID=""
SECOND_PID=""
SECOND_LOG="$(mktemp -t satori-second.XXXXXX.log)"
FAILED=0

cleanup() {
    [ -n "$SECOND_PID" ] && kill "$SECOND_PID" 2>/dev/null
    [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
    [ -n "$FS_PID" ] && kill "$FS_PID" 2>/dev/null
    [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null
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

# The other branch of binding. Only one WM can be active, so a second satori on
# the same display is told `unavailable` and has to leave on its own -- it gets
# no further events and nothing will ever wake it. It also must NOT send `stop`,
# which is a protocol error when we were never the active WM.
WAYLAND_DISPLAY="$NESTED" "$BIN" 2>"$SECOND_LOG" &
SECOND_PID=$!

for _ in {1..50}; do
    grep -qE 'wm: unavailable' "$SECOND_LOG" && break
    sleep 0.1
done
if grep -qE 'wm: unavailable' "$SECOND_LOG"; then
    echo "  ok    a second instance is told the slot is taken"
else
    echo "  FAIL  a second instance is told the slot is taken (see $SECOND_LOG)"
    FAILED=1
fi

# No signal is sent: falling out of the loop is the whole assertion. Before this
# was fixed it idled in poll forever and left a stray process on every login.
for _ in {1..50}; do
    kill -0 "$SECOND_PID" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$SECOND_PID" 2>/dev/null; then
    echo "  FAIL  a second instance exits on its own (still running after 5s)"
    FAILED=1
    kill "$SECOND_PID" 2>/dev/null
else
    echo "  ok    a second instance exits on its own"
fi
SECOND_PID=""

# Catches both a `stop` on the inactive path (river answers with a protocol
# error) and, under satori-asan, anything leaked on the way out.
if grep -qE 'error|ERROR' "$SECOND_LOG"; then
    echo "  FAIL  a second instance exits cleanly (errors in $SECOND_LOG)"
    FAILED=1
else
    echo "  ok    a second instance exits cleanly"
fi

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

    # Float/maximize. Unlike fullscreen there is no separate dirty flag: the
    # toggle is applied only because it clears `proposed`, so a proposal at the
    # floating size is the whole proof it worked.
    #
    # Mod+Space (fuzzel) is the same keysym, so the mods have to be asserted
    # too -- 0x41 is shift|mod4, 0x40 alone would match the launcher binding.
    max_before="$(grep -cE 'window: propose [0-9]+x[0-9]+ maximized' "$LOG")"

    press super+shift+space
    check "super+shift+space triggers its binding" 'binding: pressed keysym 0x20 mods 0x41'
    # Two thirds of the 1280x720 headless output, centered. Asserting the size
    # rather than just the word proves the geometry, not only the flag.
    check "floats the focused window" 'window: propose 853x480 floating'

    press super+shift+space
    check_count "toggles back to maximized" \
        'window: propose [0-9]+x[0-9]+ maximized' "$((max_before + 1))"

    # The app_id lookup. Everything else in this test is foot, so a second
    # client with a distinct app_id is what gives the letters something to tell
    # apart -- 'f' must not be the only letter that ever matches.
    #
    # Two foot windows are alive here: the original client and the one super+q
    # left behind, which is what makes the within-app ring observable.
    WAYLAND_DISPLAY="$NESTED" foot --app-id=vtest sh -c 'sleep 60' >/dev/null 2>&1 &
    APP_PID=$!
    check_count "sees the app-lookup test window" 'wm: window' 5

    # It opened focused, so 'f' is a jump out of vtest into the terminals, and
    # the second press is the ring inside them. 0x48 is mod4|mod1 -- 0x40 alone
    # would be Mod+F, which is fullscreen.
    focus_before="$(grep -cE 'seat: focus window' "$LOG")"

    press super+alt+f
    check "super+alt+f triggers its binding" 'binding: pressed keysym 0x66 mods 0x48'
    check "reaches a window of that app"     "action: focus app 'f'"
    check_count "the jump actually moves focus" 'seat: focus window' "$((focus_before + 1))"

    press super+alt+f
    check_count "a repeat press cycles within the app" \
        'seat: focus window' "$((focus_before + 2))"

    press super+alt+v
    check "super+alt+v reaches the other app" "action: focus app 'v'"
    check_count "jumping back across apps moves focus" \
        'seat: focus window' "$((focus_before + 3))"

    # Twenty-five of the twenty-six letters match nothing at any given moment,
    # so the miss has to be a quiet no-op rather than a focus change.
    focus_before="$(grep -cE 'seat: focus window' "$LOG")"
    press super+alt+z
    check "an unmatched letter finds nothing" "action: no window for 'z'"
    sleep 0.5
    if [ "$(grep -cE 'seat: focus window' "$LOG")" -eq "$focus_before" ]; then
        echo "  ok    an unmatched letter leaves focus alone"
    else
        echo "  FAIL  an unmatched letter moved focus"
        FAILED=1
    fi

    kill "$APP_PID" 2>/dev/null
    APP_PID=""
    check_count "sees the app-lookup test window close" 'window: closed' 3
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
