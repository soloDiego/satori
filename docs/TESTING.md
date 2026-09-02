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

`XDG_CONFIG_HOME` is pointed at a scratch directory for the nested run, so the
suite always tests the built-in table rather than whatever config the developer
happens to have written, and the reload assertions can rewrite the file freely.

Each run: bind -> spawn a real `foot` into the nested compositor -> inject key
chords -> kill the client (exercises `win_closed`) -> SIGINT satori -> check the
`stop` / `finished` handshake. Asserts:

- `bound river_window_manager_v1 v4` -- active WM
- `bound river_xkb_bindings_v1 vN` -- keybind global present
- `bound river_layer_shell_v1 vN` -- without it river closes every layer surface
- a second instance launched onto the same display logs `wm: unavailable`, exits
  **on its own**, and logs no error. The other branch of binding; see
  "Test: unavailable path" below for why exiting is the interesting half
- `wm: output`, `wm: seat`, `layer: default output`
- `wm: window` -- client tracked
- `window: WxH` -- the `dimensions` event; proof `propose_dimensions` landed
- `seat: focus window` -- focus applied in a manage sequence
- `binding: pressed ...` for super+return / super+q / super+j -- real key events
  reach their actions
- a second `wm: window` after super+return -- the spawn action ran
- `window: closed` after super+q -- the close intent reached a manage sequence
- one more `window: raised` after super+j -- cycling raises, not just re-focuses.
  Needs a second window spawned first: with one window open the wrap lands on the
  already-focused window and `window_focus` correctly no-ops. Asserts the raise
  was *decided*, not that the compositor restacked -- stacking is still only
  checkable by eye
- `window: fullscreen on` / `off` -- a client's fullscreen request, honored. The
  client must start as a normal window and toggle: one started with
  `--fullscreen` is never proposed, so the re-proposal below happens either way
- one more `window: propose` after leaving fullscreen -- the re-proposal the
  protocol requires. Asserted on the proposal, not the `dimensions` event that
  answers it: on a screen-sized window, maximized and fullscreen dimensions are
  identical
- `window: fullscreen on` then `off` again after two super+f presses -- the same
  round trip driven by us rather than by the client. Both halves are asserted
  because `windows_apply_fullscreen` clears the dirty flag after applying: a
  toggle that sets it only once goes fullscreen and stays there, with no bind
  left to escape it
- `window: propose 853x480 floating` after super+shift+space, then one more
  `... maximized` after a second press -- the float toggle round trip. The size
  is asserted, not just the word: two thirds of the 1280x720 headless output
  proves the geometry, not only the flag. Mods are asserted too (`0x41`) --
  Mod+Space is the same keysym and would match otherwise
- `action: focus app 'f'` after super+alt+f, plus one more `seat: focus window`
  each for a jump into an app, a repeat press, and a jump back out. Needs a
  second client with a distinct `app_id` (`foot --app-id=vtest`) -- everything
  else in the run is foot, so with one app 'f' would be the only letter that ever
  matched and any lookup that ignored the letter would pass. Mods are asserted
  (`0x48`); `0x40` alone is Mod+F, which is fullscreen
- `action: no window for 'z'` and an unchanged `seat: focus window` count -- at
  any moment most of the 26 letters match nothing, so the miss has to leave focus
  alone rather than clear it
- `binding: pressed keysym 0x1008ffb2 mods 0x0` twice -- an unmodified XF86
  keysym dispatches at all. Dispatch only, and deliberately so; see below
- the config reload chain, in order: super+shift+t does **nothing** before any
  reload (the suite starts with no config file), then a config is written,
  super+shift+r logs `config: reloaded N bindings`, and super+shift+t now fires
  and spawns a window. A chord that was unbound at startup firing afterwards is
  the only assertion that proves all four steps happened -- parse, destroy every
  proxy borrowing into the old table, recreate, and enable in the same manage
  sequence
- one more `config: reloaded` after `SIGHUP`, and a binding added over that
  reload firing. SIGHUP exercises a path the keybind does not: a signal brings no
  manage sequence with it, so satori has to ask for one with `manage_dirty`
- `config: reload failed, keeping the running bindings` after a SIGHUP with a
  broken config, **and the previously loaded binding still firing**. The second
  half is the assertion that matters: satori owns 100% of input, so a reload that
  cleared the table and then failed would be a session with no way out
- `window: app_id ptest` -- the `app_id` is logged as it arrives. Load-bearing
  rather than cosmetic: it is the only way to find out what to write in a
  `passthrough` line, since an `app_id` is neither the window title nor the
  command name
- the passthrough chain, against a `foot --app-id=ptest` and a config listing
  `ptest`: `passthrough: on` when it takes focus, then super+shift+t (bound by
  the reload above) **not** firing, then super+shift+p firing anyway and logging
  `action: passthrough suspended`, then super+shift+t firing again, then a second
  super+shift+p restoring it, then `passthrough: off` when the window closes.
  The negative assertion in the middle is the feature: passthrough is
  implemented by *disabling* the bindings, so a chord that stops reaching satori
  is the only thing observable from outside. The escape half is the one that
  keeps the session recoverable -- with the exemption removed, 8 assertions go
  red and the app owns the keyboard permanently
- `layer: focus exclusive` then `focus none` plus one more `seat: focus window`
  -- a real layer surface (`fuzzel`, skipped if not installed) maps, takes the
  keyboard, and Satori takes it back. Only exercises the *exclusive* path;
  non-exclusive focus has no coverage here, see below
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

Float *placement* is the sharpest case of that. A floating window is proposed at
the right size and stacked correctly whether or not `window_position` honors
`float_x/float_y` -- park every floating window in the top left corner and the
whole smoke test still passes. `window_position` is split out of `window_place`
purely so `test_position_follows_float_geometry` can guard it; that unit test is
the only thing standing between us and that bug.

Media keys are the one binding class whose effect is deliberately **not** run.
The commands act on the machine, not on satori: the real PipeWire sink and the
real backlight belong to whoever typed `make test`, and a suite that dimmed your
screen 5% per run would be worse than the bug it guards. Only **mic mute** is
injected, twice -- exactly reversible, no clamping edge cases, no trace left. It
proves the thing that could fail silently: that a keysym bound at modifier `0`
fires at all, which is the same failure mode as `XKB_KEY_E` vs `XKB_KEY_e`.
Mutation-verified -- rebinding mic mute to `MOD` takes both assertions red.

Everything else about those bindings lives in `test_media_keys_are_bound_unmodified`:
the exact keysym, that it is bound unmodified, and the exact command string,
including the `-l 1.0` volume cap and the brightness floor. Three mutants die
there (mic mute given a modifier, floor clamp deleted, volume cap dropped) and
none of them is reachable from the smoke test.

Also does not catch the layer-shell focus deferral. `fuzzel` takes focus
exclusively, and river ignores Satori's focus requests outright in that state,
so deleting the `seat->layer_focus` guard still passes here -- verified by
mutation. The case the guard exists for is non-exclusive (on-demand) focus, and
exercising it would need a purpose-built layer client. The unit test is the only
guard; see below.

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

Covered by `make test` now -- a second instance is launched onto the nested
display and asserted on. Manual version:

```sh
./satori
```

Connects to the outer river; WM slot already taken -> `unavailable`.
Expect: `wm: unavailable`, exits **on its own** within a second, no `stop` sent.

Exiting is the assertion. `unavailable` is the first and only event that object
gets, so nothing will ever wake the loop again; until 2026-08-02 satori idled in
`poll` forever here and left a stray process behind on every occurrence.

## Recovery

Locked-up compositor: SSH in (openssh enabled), kill the nested river / Satori.

## Unit tests

`tests/test_actions.c`, run first by `make test`, also runnable alone:

```sh
make build/test-actions && ./build/test-actions
```

Covers the compositor-free half of the action layer -- focus cycling and its
wraps, empty and single-window lists, the `focus_dirty` no-op guard, close
intent, the fullscreen and float toggles, float geometry and placement, output
removal, usable-area fallback, layer-focus deferral, MRU promotion and the
`app_id` lookup. Fake `struct window`s on the stack, handles left NULL and never
touched.

Also the whole of `src/config.c`, which is pure and needs no compositor at all:
chord parsing and its rejections, the keysym lowering, merge-over-defaults,
`bind ... none`, `app-keys`, param joining, and every way a file can be
rejected. Config files are written to a temp path and loaded for real, so
`libscfg` is exercised rather than mocked.

`test_config_set_replaces_owned_commands` walks one chord through every
`arg_kind` and back out. It looks like it asserts nothing interesting, and under
the plain binary it nearly doesn't -- its real value is under ASan, where an
override that failed to free the command it replaced shows up as a leak.

The binding-table assertions run against a real `config_load(NULL, true)` rather
than a static array, so the built-ins are checked as they are actually assembled
-- including that every one of them names an action that exists.

`test_example_config_matches_the_defaults` parses `example/config` and compares
it binding for binding with the built-ins, so the shipped example cannot drift
from what it claims to restate. It loads the file with `with_defaults=false`,
and that argument is the entire test: merged, a line *missing* from the example
is filled in by the built-in underneath it and the comparison passes. Verified
by deleting a `bind` line -- red only with the bare parse.

Deleting the `app-keys Mod+Alt` line is an equivalent mutant and stays green.
That is correct: `Mod+Alt` is the default when the directive is absent, so the
line documents the knob rather than changing anything.

The lookup is where the unit tests carry the most weight. The smoke test can see
that focus moved; it cannot see *which list was walked*, and both the recency
answer and the creation-order answer look like a focus change in the log. The
two tests that pin it down are `test_focus_app_jumps_to_the_most_recent_match`
(the older window is the more recently used one, so creation order gives the
wrong answer) and `test_focus_app_cycles_within_an_app_in_a_stable_ring` (three
windows of one app; a recency walk visits only two of them, forever).

Passthrough splits the same way. `bindings_apply_enabled` sends requests on live
proxies and cannot run in a unit test, so the two decisions it makes are their
own functions: `satori_passthrough_active` (is the focused window's `app_id`
listed, and has the escape suspended it) and `binding_stays_enabled` (is this
binding exempt). Both are unit tested, including that the exempt set is exactly
`passthrough` and `exit` and that re-binding the escape action carries the
exemption -- the smoke test can see a chord stop firing, but not *why*.

The layer-focus deferral test runs last on purpose: with the guard removed it
walks into a request on a NULL proxy and segfaults. That is still red, but it
takes the rest of the run with it.

It `#include`s `src/input.c` to reach the static actions, so the Makefile rule
lists `src/input.c` as a prerequisite. Drop that and the test binary silently
stops rebuilding when you edit an action -- it passes forever, testing nothing.
`build/config.o` is linked rather than included; it calls back into the included
`input.c` for `action_from_name`.

The rest of `src/` is Wayland glue; unit tests there would test libwayland. Add a
real framework when the plain `CHECK` macro stops being enough.

Check a test can fail before trusting it. Break the thing it covers, run it,
revert:

```sh
sed -i 's/if (satori->focused == win) return;/if (0) return;/' src/window.c
make -s build/test-actions && ./build/test-actions   # expect FAIL
git checkout src/window.c
```

**`make` does not rebuild `build/test-actions`** -- only the `test` target does.
Build it explicitly, and run the unmutated control first. A mutation run against
a stale binary reports every mutant surviving, which has already produced one
full wrong conclusion. `build/keypress` has the same trap: `make clean` removes
it and `make satori` does not bring it back, so ad-hoc chord injection silently
does nothing.

Mutants the reload path has been checked against, all four fatal:

| Mutation | Result |
| --- | --- |
| `chord_parse` skips `xkb_keysym_to_lower` | 5 unit CHECKs + 4 smoke assertions |
| SIGHUP without `manage_dirty` | 3 smoke assertions; the reload never fires |
| `config_load` returns its table instead of NULL on error | 11 unit CHECKs + 2 smoke |
| `config_reload` frees the old table before destroying the proxies | ASan heap-use-after-free in `binding_pressed`; satori dies, 6 assertions |

The last one is the reason the teardown order is worth a doc line in
[SEQUENCES.md](SEQUENCES.md): it is a lifetime bug rather than a wrong answer,
and only ASan or an outright crash surfaces it.
