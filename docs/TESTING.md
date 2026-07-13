# Testing Satori

Satori only runs as the WM of a live river session; outside one it fails to
connect.

Two layers:

- `make test` -- automated, headless. Run it every change.
- Manual nested river -- for anything visual. The automated test proves events
  flowed, not that pixels are right.

## Automated: `make test`

```sh
make test
```

Builds `satori` + `satori-asan`, runs each through
`scripts/test-nested.sh`. Takes ~10s.

Runs river under the **headless** wlroots backend (`WLR_BACKENDS=headless`):
virtual 1280x720 output, no window on screen, outer session untouched. Satori
binds as *its* WM, so a hang breaks nothing.

Each run: bind -> spawn a real `foot` into the nested compositor -> kill it
(exercises `win_closed`) -> SIGINT satori -> check the `stop` / `finished`
handshake. Asserts:

- `bound river_window_manager_v1 v4` -- active WM
- `wm: output`
- `wm: window` -- client tracked
- `window: WxH` -- the `dimensions` event; proof `propose_dimensions` landed
- survives the client closing -- no crash in the unlink/free path
- `wm: finished` + process exits -- clean shutdown
- no ASan/LSan findings (asan run) -- no leaked proxies

Failure prints the log path. Run one binary directly:

```sh
./scripts/test-nested.sh ./satori-asan
```

Catches: protocol errors (`sequence_order`, double `get_node`), compositor
hangs (missing `manage_finish` / `render_finish`), leaks, use-after-free.
None of these are reachable by unit tests -- they need a real compositor.

Does NOT catch: wrong position, wrong size, inverted stacking. Events flowing
!= pixels correct.

### Gotcha

Never match satori with `pkill -f`. River's argv contains satori's path (it is
the `-c` init command), so `-f` signals the compositor too. Use `pkill -x satori`.

## Manual: visible nested river

For visual checks only (does it actually draw? maximized? on top?).

```sh
river -c "$PWD/satori 2>/tmp/satori.log"
```

Same as the automated path but on the real backend -> opens a window. `$PWD`
expands in the launching shell; from elsewhere use satori's full path.

Watch: `tail -f /tmp/satori.log`

Spawn a client into it (no keybinds yet -> from outside). Find the nested
socket -- the new one; outer session is `wayland-0`:

```sh
ls $XDG_RUNTIME_DIR/wayland-*
WAYLAND_DISPLAY=wayland-N foot
```

Expect: client appears, maximized, filling the nested output.

Close it with Ctrl-C in its launching terminal. Not `pkill foot` (kills every
foot, incl. your outer session).

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

None yet, and not a gap: `main.c` is Wayland glue, so unit tests would test
libwayland. First compositor-free logic: config parser, `app_id` prefix match,
MRU list. Add a real C test framework + wire into `make test` then -- alongside
the smoke test, not instead of it.
