# Keybinds

Compiled in. Table: `keybinds[]`, `src/input.c:118`. No config file yet.

River 0.4 has no built-in bindings and ships no `riverctl`. This table is every
binding in the session: a key not listed here does nothing.

Mod = super (`RIVER_SEAT_V1_MODIFIERS_MOD4`).

| Binding | Action | Arg | Effect |
| --- | --- | --- | --- |
| Mod+Return | `action_spawn` | `foot` | runs the arg through `/bin/sh -c`, detached |
| Mod+Space | `action_spawn` | `fuzzel` | as above |
| Mod+Q | `action_close_focused` | — | close request to the focused window; the client may delay or ignore it |
| Mod+F | `action_toggle_fullscreen` | — | fullscreens the focused window, or leaves fullscreen |
| Mod+J | `action_focus_next` | — | next window in list order, wraps |
| Mod+K | `action_focus_prev` | — | previous window in list order, wraps |
| Mod+Shift+E | `action_exit_session` | — | ends the session, no confirmation; every client is disconnected |

`mod4|mod1` is reserved for the planned `Mod+Alt+<letter>` app_id lookup and is
deliberately absent from this table.

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

`action_exit_session` needs `river_window_manager_v1` v4; on an older
compositor it logs and does nothing (`src/input.c:51`).

## Action arguments

`satori_action` takes a `union satori_arg` (`src/satori.h:67`), so one action
can serve many bindings — `action_spawn` is the reason it exists. Actions that
ignore it still take it. Members: `cmd` (`const char *`), `u` (`uint32_t`).

## Modifier values

`river_seat_v1.modifiers`, a bitfield. Combine with `|`.

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

River matches the unshifted keysym. `XKB_KEY_E` with the shift bit binds
successfully and then **never fires** — no error, no log line, the key simply
does nothing. `scripts/test-exit.sh` asserts the exact pair Mod+Shift+E produces
(`keysym 0x65 mods 0x41`) because that failure is silent.

## Constraints

- One binding per keysym+modifier pair per seat. Duplicates: which one fires is
  compositor policy.
- Bindings are created per seat, at `wm: seat`, and enabled in the next manage
  sequence (`src/input.c:190`). A key pressed before that reaches the focused
  client instead.
- Actions run outside a manage sequence and must not touch window management
  state. See [SEQUENCES.md](SEQUENCES.md).
