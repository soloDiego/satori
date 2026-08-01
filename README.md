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
river -c "$PWD/satori"
```

Started from inside an existing Wayland session, river nests itself in a window
— that is the development setup, and a hang costs one window instead of the
machine. See [docs/TESTING.md](docs/TESTING.md).

One window manager per river instance. If the slot is already taken, Satori logs
`wm: unavailable` and exits.

## Tests

```sh
make test
```

Headless nested river, ~10s, run it on every change. Key presses and pixels are
not covered — see [docs/TESTING.md](docs/TESTING.md).

## Next

- [docs/TESTING.md](docs/TESTING.md) — how to test a change
- [docs/KEYBINDS.md](docs/KEYBINDS.md) — default bindings
- [docs/SEQUENCES.md](docs/SEQUENCES.md) — the manage/render state machine

## License

GPL-3.0-or-later. The protocol XML in `protocol/` is MIT, from
[river](https://codeberg.org/river/river).
