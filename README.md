# Satori

Satori is a floating window manager for the
[river](https://codeberg.org/river/river) Wayland compositor. River does the
compositing; Satori is the client that speaks `river-window-management-v1` and
supplies the policy — what is focused, how big it is, what the keyboard does.
Windows open maximized and focused. No workspaces, no tiling.

Needs river 0.4.x or later, and its `river_xkb_bindings_v1` global for keybinds.

## Run it

```sh
make
river -c "$PWD/satori 2>/tmp/satori.log"
```

Run from inside an existing Wayland session, river nests itself in a window and
Satori manages that window's contents — the development setup, where a hang
costs one window instead of the machine.

One window manager per river instance. If the slot is already taken, Satori logs
`wm: unavailable` and exits. `river -c` is a startup command, not a lifeline:
the compositor outlives Satori, so quit it with Ctrl-C in the launching
terminal.

To run it as a real session, `make install` (to `~/.local/bin`, override with
`PREFIX=` or `BINDIR=`) and put `satori &` in `~/.config/river/init`. See
[docs/RUNNING.md](docs/RUNNING.md) first — Satori owns every binding in the
session, and what it does not bind cannot be done.

## Use it

Super+Return opens a terminal, Super+Space a launcher, Super+Q closes the
focused window, Super+J/K cycle focus. Bindings are compiled in for now —
[docs/KEYBINDS.md](docs/KEYBINDS.md) covers the table and how to add one.

## Tests

```sh
make test
```

Unit tests, then a headless nested river with injected key events. ~50s, run it
on every change. Pixels are not covered — see
[docs/TESTING.md](docs/TESTING.md).

## Next

- [docs/RUNNING.md](docs/RUNNING.md) — running it as your session, and recovery
- [docs/TESTING.md](docs/TESTING.md) — how to test a change
- [docs/KEYBINDS.md](docs/KEYBINDS.md) — default bindings
- [docs/SEQUENCES.md](docs/SEQUENCES.md) — the manage/render state machine

## License

GPL-3.0-or-later. The protocol XML in `protocol/` is MIT, from
[river](https://codeberg.org/river/river); the test-only XML in `tests/` is MIT,
from wlr-protocols.
