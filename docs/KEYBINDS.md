# Keybinds

The defaults. A config file merges over this table — see
[CONFIG.md](CONFIG.md) for the format, the action names, and reloading.

River 0.4 has no built-in bindings and ships no `riverctl`. This table plus the
config file is every binding in the session: a key not listed in either does
nothing.

Mod = super (`RIVER_SEAT_V1_MODIFIERS_MOD4`), Alt = `MOD1`.

| Binding | Action | Arg | Effect |
| --- | --- | --- | --- |
| Mod+Return | `spawn` | `foot` | runs the arg through `/bin/sh -c`, detached |
| Mod+Space | `spawn` | `fuzzel` | as above |
| Mod+Q | `close` | — | close request to the focused window; the client may delay or ignore it |
| Mod+F | `fullscreen` | — | fullscreens the focused window, or leaves fullscreen |
| Mod+J | `focus-next` | — | next window in list order, wraps |
| Mod+K | `focus-prev` | — | previous window in list order, wraps |
| Mod+Shift+Space | `float` | — | floating window keeps its own size and position, or back to maximized |
| Mod+Shift+R | `reload` | — | re-reads the config file; keeps the running bindings if it does not parse |
| Mod+Shift+E | `exit` | — | ends the session, no confirmation; every client is disconnected |
| Mod+Alt+A … Mod+Alt+Z | `focus-app` | the letter, in `arg.u` | focuses a window whose `app_id` starts with that letter |

The built-ins are written as action *names* (`defaults`, `src/input.c:205`) and
installed through the same `config_set` path a config file uses, so a default
that would be rejected in a config file is rejected here too.

The 26 letter bindings are generated, not typed out (`config_add_app_keys`,
`src/config.c:209`); `mod4|mod1` carries nothing else, so the whole
`Mod+<letter>` space stays free for ordinary bindings.

Which window `Mod+Alt+<letter>` picks:

| Focused window's app_id | Picks |
| --- | --- |
| does not start with the letter | the matching window focused most recently (`satori->mru`) |
| starts with the letter | the next match in creation order, wrapping (`satori->windows`) |
| no window matches | nothing; logs `action: no window for '<letter>'` |

Two lists because the two cases want different answers: recency to arrive, a
stable ring to walk. See [SEQUENCES.md](SEQUENCES.md). Matching is on the first
character only, case-insensitive; a window that has not sent an `app_id` yet
matches no letter.

List order is newest first. Nothing is focused when no window is open; Mod+J/K
then focus the newest. Focusing also raises: without that the window you cycle
to stays buried under the newest one, and since everything is maximized it is
invisible.

With one window open, Mod+J/K are no-ops — the wrap lands on the window that is
already focused, and `window_focus` returns early when focus is unchanged.

Mod+F records the same intent a client's own `fullscreen_requested` does, so both
land in `windows_apply_fullscreen` and a window fullscreened by the client (foot's
bind, a video player) leaves fullscreen on Mod+F. Fullscreen covers the whole
output including any exclusive zone, so a bar is hidden while it is up.

Mod+Shift+Space pairs the same way with a client's `unmaximize_requested`: both
set `floating` and clear `proposed`, and `windows_propose` applies either. A
floating window is sized two thirds of the usable area, centered, the first time
it floats — nothing moves or resizes it yet, so that is the only geometry it
gets. Toggling while fullscreen only records the state; it takes effect on
leaving fullscreen.

Mod+Space and Mod+Shift+Space are the same keysym. River matches the *unshifted*
keysym, so the modifiers are what tell them apart — `0x40` vs `0x41`.

`action_exit_session` needs `river_window_manager_v1` v4; on an older
compositor it logs and does nothing (`src/input.c:53`).

Mod+Alt+&lt;letter&gt; is a no-op when the only match is the window already focused —
the ring comes back to it and `window_focus` returns early. Same shape as Mod+J/K
with one window open.

## Media keys

Unmodified, on the ThinkPad's printed keycaps. No Fn needed with the default
Fn-lock; these are the F1–F6 row's primary function.

| Binding | Keysym | Runs |
| --- | --- | --- |
| Mute | `XF86AudioMute` | `wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle` |
| Volume down | `XF86AudioLowerVolume` | `wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-` |
| Volume up | `XF86AudioRaiseVolume` | `wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+ -l 1.0` |
| Mic mute | `XF86AudioMicMute` | `wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle` |
| Brightness down | `XF86MonBrightnessDown` | sysfs write, −5% |
| Brightness up | `XF86MonBrightnessUp` | sysfs write, +5% |

**These are the only bindings with no modifier**, and the only ones that may be.
An unmodified letter would be unusable everywhere in the session; XF86 keysyms
produce no text, so grabbing them swallows nothing. `test_media_keys_are_bound_unmodified`
enforces both halves — media keys bind at `0`, everything else must carry a
modifier.

One step is 5% per press. **Holding a key does not ramp**: the xkb-bindings
protocol does not auto-repeat, and `stop_repeat` exists precisely because
repeating is the window manager's job. Not implemented — it needs a timerfd in
the poll loop.

`-l 1.0` caps volume at 100%. Without it `wpctl` boosts past unity, which clips
rather than gets louder.

Brightness writes `/sys/class/backlight/intel_backlight/brightness` directly
(`BRIGHTNESS_CMD`, `src/input.c:148`) rather than shelling out to
`brightnessctl` or `light` — neither is installed, and the file is group-writable
to `video`. The floor of 1% of max is load-bearing: a backlight at 0 is a black
screen, and the key that raises it again is one you can no longer see. The
device path is hardcoded to this machine's `intel_backlight`.

Media keys are `action_spawn` like any other command binding, so they carry no
sequence constraint and satori does not track volume or brightness state — the
key runs a command and the tool owns the state.

## Action arguments

`satori_action` takes a `union satori_arg`, so one action can serve many
bindings — `spawn` is the reason it exists. Actions that ignore it still take
it. Members: `cmd` (`const char *`), `u` (`uint32_t`).

Which member is live is recorded alongside it as an `enum satori_arg_kind`
(`src/satori.h:130`). That is not redundant: the table is heap-owned and rebuilt
on every reload, so a keybind has to be freeable without consulting its action,
and `arg.u` aliases the same storage as `arg.cmd`. Freeing unconditionally would
treat a `focus-app` letter as a pointer.

## Modifier values

`river_seat_v1.modifiers`, a bitfield. Config-file spellings are in
[CONFIG.md](CONFIG.md).

| Name | Value |
| --- | --- |
| none | 0 |
| shift | 1 |
| ctrl | 4 |
| mod1 (alt) | 8 |
| mod3 | 32 |
| mod4 (super) | 64 |
| mod5 | 128 |

Capslock (2) and numlock (16) are excluded upstream: locked modifiers are not
usable in bindings.

## Keysyms

`XKB_KEY_*`, `/usr/include/xkbcommon/xkbcommon-keysyms.h`. Letter keysyms are
always the lowercase form (`XKB_KEY_q`, not `XKB_KEY_Q`); adding shift means
setting the shift bit, not changing the keysym.

XF86 keysyms occupy `0x1008ff00`–`0x1008ffff`. That range is what
`is_media_key` tests to allow the unmodified exception.

River matches the unshifted keysym. `XKB_KEY_E` with the shift bit binds
successfully and then **never fires** — no error, no log line, the key simply
does nothing. `scripts/test-exit.sh` asserts the exact pair Mod+Shift+E produces
(`keysym 0x65 mods 0x41`) because that failure is silent.

## Constraints

- One binding per keysym+modifier pair per seat. `config_set` enforces it by
  replacing in place; two on one chord would make which fires compositor policy.
- Bindings are created per seat, at `wm: seat` (`seat_bindings_create`,
  `src/input.c:301`), and enabled in the next manage sequence. A key pressed
  before that reaches the focused client instead.
- 41 bindings per seat by default: 15 built-in, 26 letters. A config file
  changes the count.
- Actions run outside a manage sequence and must not touch window management
  state. See [SEQUENCES.md](SEQUENCES.md).
