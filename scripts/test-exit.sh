#!/usr/bin/env bash
# Automated check for the exit binding, in its own nested river because it
# destroys the compositor -- it cannot share test-nested.sh's session.
#
# This binding is the only way out of a river session (river 0.4 has no
# built-in bindings and no riverctl), so a silent failure strands the user.
# Usage: scripts/test-exit.sh [./satori | ./satori-asan]

set -uo pipefail

BIN="${1:-./satori}"
[ -x "$BIN" ] || { echo "no such binary: $BIN (run make)" >&2; exit 1; }
BIN="$(realpath "$BIN")"
NAME="$(basename "$BIN")"
# shellcheck source=lib-nested.sh
. "$(dirname "$0")/lib-nested.sh"

KEYPRESS="$(realpath "$(dirname "$0")/../build/keypress")"
[ -x "$KEYPRESS" ] || { echo "no $KEYPRESS (run make)" >&2; exit 1; }

LOG="$(mktemp -t satori-exit.XXXXXX.log)"
RIVER_PID=""
CLIENT_PID=""
NESTED=""
FAILED=0

cleanup() {
    [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
    [ -n "$RIVER_PID" ] && kill "$RIVER_PID" 2>/dev/null
    nested_kill TERM    # scoped to $NESTED; by name it would hit the live session
    wait 2>/dev/null
}
trap cleanup EXIT

wait_for() {
    local pattern="$1" secs="${2:-5}" i
    for ((i = 0; i < secs * 10; i++)); do
        grep -qE "$pattern" "$LOG" && return 0
        sleep 0.1
    done
    return 1
}

check() {
    if wait_for "$2" 5; then
        echo "  ok    $1"
    else
        echo "  FAIL  $1 (no /$2/ in log)"
        FAILED=1
    fi
}

echo "== satori exit-binding test ($BIN)"

sockets_before="$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | sort)"

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    river -c "$BIN 2>$LOG" >/dev/null 2>&1 &
RIVER_PID=$!

NESTED=""
for _ in {1..50}; do
    NESTED="$(comm -13 <(echo "$sockets_before") \
        <(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | sort) | head -1)"
    [ -n "$NESTED" ] && break
    sleep 0.1
done
[ -n "$NESTED" ] || { echo "  FAIL  nested river never came up"; exit 1; }
echo "  ..    nested river on \$WAYLAND_DISPLAY=$NESTED (pid $RIVER_PID)"

# Bindings are only enabled during a manage sequence, and a window is the
# simplest proof one has run.
WAYLAND_DISPLAY="$NESTED" foot sh -c 'sleep 60' >/dev/null 2>&1 &
CLIENT_PID=$!
check "reaches a manage sequence" 'seat: focus window'
sleep 1

WAYLAND_DISPLAY="$NESTED" "$KEYPRESS" super+shift+e >/dev/null 2>&1

# 0x65 is XKB_KEY_e, 0x41 is mod4|shift. River matches the *unshifted* keysym
# plus the shift bit; XKB_KEY_E never matches. Asserting the exact numbers
# catches a table edit that silently stops matching.
check "super+shift+e triggers its binding" 'binding: pressed keysym 0x65 mods 0x41'
check "runs the exit action"               'action: exit session'

# The real assertion: the compositor is gone.
for _ in {1..50}; do kill -0 "$RIVER_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$RIVER_PID" 2>/dev/null; then
    echo "  FAIL  compositor still running after exit_session"
    FAILED=1
else
    echo "  ok    compositor exited"
    RIVER_PID=""
fi

# Satori must fall out of its event loop on the closed display, not spin.
# River is gone by now, so the nested instance has been reparented to init --
# which is why nested_pids matches on the environment, not on ancestry.
for _ in {1..50}; do nested_running || break; sleep 0.1; done
if nested_running; then
    echo "  FAIL  satori still running $(ps -o pcpu= -p "$(nested_pids | head -1)" | tail -1 | tr -d ' ')% cpu"
    FAILED=1
else
    echo "  ok    satori exited with the session"
fi

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
    echo "FAIL  (log: $LOG)"
fi
exit "$FAILED"
