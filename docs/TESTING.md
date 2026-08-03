# Testing Satori

Satori only runs as the WM of a live river session; outside one it fails to
connect.

Three layers:

- `make test` -- automated. Unit tests, then a headless nested river. Run it
  every change.
- Manual nested river -- for anything visual. The automated test proves events
  flowed, not that pixels are right.

## Automated: `make test`

```sh
make test
```

Builds `satori` + `satori-asan`, runs each through `scripts/test-nested.sh`,
then `scripts/test-exit.sh` once. Takes ~50s.

Runs river under the **headless** wlroots backend (`WLR_BACKENDS=headless`):
virtual 1280x720 output, no window on screen, outer session untouched. Satori
binds as *its* WM, so a hang breaks nothing.

Each run: bind -> spawn a real `foot` into the nested compositor -> inject key
chords -> kill the client (exercises `win_closed`) -> SIGINT satori -> check the
`stop` / `finished` handshake. Asserts:

- `bound river_window_manager_v1 v4` -- active WM
- `bound river_xkb_bindings_v1 vN` -- keybind global present
- `wm: output`, `wm: seat`
- `wm: window` -- client tracked
- `window: WxH` -- the `dimensions` event; proof `propose_dimensions` landed
- `seat: focus window` -- focus applied in a manage sequence
- `binding: pressed ...` for super+return / super+q / super+j -- real key events
  reach their actions
- a second `wm: window` after super+return -- the spawn action ran
- `window: closed` after super+q -- the close intent reached a manage sequence
- `window: fullscreen on` / `off` -- a client's fullscreen request, honored. The
  client must start as a normal window and toggle: one started with
  `--fullscreen` is never proposed, so the re-proposal below happens either way
- one more `window: propose` after leaving fullscreen -- the re-proposal the
  protocol requires. Asserted on the proposal, not the `dimensions` event that
  answers it: on a screen-sized window, maximized and fullscreen dimensions are
  identical
- survives the client closing -- no crash in the unlink/free path
- `wm: finished` + process exits -- clean shutdown
- no ASan/LSan findings (asan run) -- no leaked proxies

## `scripts/test-exit.sh`

Its own nested river, because `exit_session` destroys the compositor and cannot
share a session with the assertions above. Asserts `binding: pressed keysym 0x65
mods 0x41`, `action: exit session`, that the compositor is gone, and that satori
falls out of its event loop on the closed display rather than spinning.

Separate because the exit binding is the only way out of a river session and
fails silently when wrong -- a binding on the shifted keysym (`XKB_KEY_E`) is
created without error and never fires.

Failure prints the log path. Run one binary directly:

```sh
./scripts/test-nested.sh ./satori-asan
```

Catches: protocol errors (`sequence_order`, double `get_node`), compositor
hangs (missing `manage_finish` / `render_finish`), leaks, use-after-free.
None of these are reachable by unit tests -- they need a real compositor.

Does NOT catch: wrong position, wrong size, inverted stacking. Events flowing
!= pixels correct.

## Key injection

The headless backend has no keyboard (`WLR_LIBINPUT_NO_DEVICES=1`), so bindings
are triggered through `zwp_virtual_keyboard_v1` instead.

```sh
WAYLAND_DISPLAY=wayland-N ./build/keypress super+return
```

`tests/keypress.c`: binds the seat and the virtual keyboard manager, uploads an
xkb keymap built with xkbcommon (a keymap is required before any key request),
then presses the chord left to right and releases it in reverse. Names in
`key_names[]`; a bare number is an evdev keycode. Roundtrip plus 40ms between
events -- satori answers a binding with a whole manage sequence, and the chord
must not outrun it.

The protocol XML is vendored at `tests/virtual-keyboard-unstable-v1.xml`
(wlr-protocols, MIT). Test-only: satori itself never speaks it.

This covers the wire from a real key event to an action running. What the action
then does is covered by the unit tests.

### Gotcha: never signal satori by name

Satori is the developer's real window manager, so the instance under test and
the live session have the same process name. **`pkill -x satori` matches both**
— `make test` used to SIGINT the desktop it was running on, which looks like a
clean shutdown in the log and nothing like a test bug. `pkill -f` is worse:
river's argv contains satori's path when it is the `-c` command, so `-f` signals
the compositor too.

`scripts/lib-nested.sh` is the only correct way to find it. River exports
`WAYLAND_DISPLAY` to its `-c` child, so the nested instance is the one whose
`/proc/PID/environ` names the nested socket — which still holds after river
exits and the process is reparented, the case `test-exit.sh` needs.

```sh
. scripts/lib-nested.sh   # needs $NAME and $NESTED in scope
nested_pids               # PIDs belonging to this nested compositor
nested_running            # true while it is alive
nested_kill INT           # signal only it
```

With `$NESTED` unset it deliberately matches nothing: leaking a test process is
cheaper than killing the session.

## Manual: visible nested river

For visual checks only (does it actually draw? maximized? on top?).

```sh
river -c "$PWD/satori 2>/tmp/satori.log"
```

Same as the automated path but on the real backend -> opens a window. `$PWD`
expands in the launching shell; from elsewhere use satori's full path.

Watch: `tail -f /tmp/satori.log`

Spawn a client with **Super+Return** (click the nested window first, so it has
keyboard focus). Expect: `foot` appears, maximized, filling the nested output,
and focused -- type and characters land in it.

From outside works too. Find the nested socket -- the new one; outer session is
`wayland-0`:

```sh
ls $XDG_RUNTIME_DIR/wayland-*
WAYLAND_DISPLAY=wayland-N foot
```

Close a client with Ctrl-C in its launching terminal, or Super+Q. Not
`pkill foot` (kills every foot, incl. your outer session).

### Keybinds

See [KEYBINDS.md](KEYBINDS.md). With two clients open:

- Super+Return -- another `foot`, on top, focused
- Super+J / Super+K -- focus cycles; the log shows `binding: pressed ...` then
  `seat: focus window`
- Super+Q -- focused client closes, focus lands on the next one

If nothing happens, check whether the outer session's WM grabbed the key first
-- it never reaches the nested river. Test that binding on a TTY, or rebind it
temporarily in `keybinds[]`.

### Close the nested river

`-c` runs once at startup; the nested river outlives Satori (blank window).

- Ctrl-C in the launching terminal, or
- `pgrep -f "river -c"`, then `kill <pid>`

Never `pkill river` (kills your main session too).

## Test: unavailable path

```sh
./satori
```

Connects to the outer river; WM slot already taken -> `unavailable`.
Expect: `wm: unavailable`, clean exit, no `stop` sent.

## Recovery

Locked-up compositor: SSH in (openssh enabled), kill the nested river / Satori.

## Unit tests

`tests/test_actions.c`, run first by `make test`, also runnable alone:

```sh
make build/test-actions && ./build/test-actions
```

Covers the compositor-free half of the action layer -- focus cycling and its
wraps, empty and single-window lists, the `focus_dirty` no-op guard, close
intent. Fake `struct window`s on the stack, handles left NULL and never touched.

It `#include`s `src/input.c` to reach the static actions, so the Makefile rule
lists `src/input.c` as a prerequisite. Drop that and the test binary silently
stops rebuilding when you edit an action -- it passes forever, testing nothing.

The rest of `src/` is Wayland glue; unit tests there would test libwayland. Next
compositor-free logic worth covering: config parser, `app_id` prefix match, MRU
list. Add a real framework when the plain `CHECK` macro stops being enough.

Check a test can fail before trusting it. Break the thing it covers, run it,
revert:

```sh
sed -i 's/if (satori->focused == win) return;/if (0) return;/' src/window.c
make -s build/test-actions && ./build/test-actions   # expect FAIL
git checkout src/window.c
```
