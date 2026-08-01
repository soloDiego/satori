# Keybinds

Compiled in. Table: `keybinds[]`, `src/input.c:76`. No config file yet.

Mod = super (`RIVER_SEAT_V1_MODIFIERS_MOD4`).

| Binding | Action | Effect |
| --- | --- | --- |
| Mod+Return | `action_spawn_terminal` | runs `SATORI_TERMINAL` (`foot`) through `/bin/sh -c`, detached |
| Mod+Q | `action_close_focused` | close request to the focused window; the client may delay or ignore it |
| Mod+J | `action_focus_next` | next window in list order, wraps |
| Mod+K | `action_focus_prev` | previous window in list order, wraps |

List order is newest first. Nothing is focused when no window is open; Mod+J/K
then focus the newest.

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
lowercase (`XKB_KEY_q`, not `XKB_KEY_Q`); adding shift means setting the shift
bit, not changing the keysym.

## Constraints

- One binding per keysym+modifier pair per seat. Duplicates: which one fires is
  compositor policy.
- Bindings are created per seat, at `wm: seat`, and enabled in the next manage
  sequence (`src/input.c:157`). A key pressed before that reaches the focused
  client instead.
- Actions run outside a manage sequence and must not touch window management
  state. See [SEQUENCES.md](SEQUENCES.md).
