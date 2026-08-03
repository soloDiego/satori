# Running Satori as your session

Scope: a real river session on a TTY, not the nested development setup. For
nested, see [TESTING.md](TESTING.md).

## Install

```sh
make install                    # -> ~/.local/bin/satori
make install PREFIX=/usr/local  # -> /usr/local/bin/satori
make install BINDIR=/opt/bin
```

`make uninstall` takes the same variables.

## Session setup

River runs `~/.config/river/init` at startup, or `sh -c <command>` with `-c`.

```sh
#!/bin/sh
satori 2>/tmp/satori.log &
```

Expect: executable bit set on `init`. River does not run it otherwise, and
starts with no window manager — windows never become visible.

Log to a file. Satori writes every event to stderr and there is no terminal to
read it from on a TTY.

## What Satori owns

River 0.4 has no built-in key bindings and ships no `riverctl`. Every binding in
the session comes from `keybinds[]` (`src/input.c:104`). Not in the table = not
possible. There is no fallback and no command socket.

Consequences:

| Want | Status |
| --- | --- |
| exit the session | Mod+Shift+E only |
| launch anything | Mod+Return (foot), Mod+Space (fuzzel) |
| volume, brightness, media keys | not bound; add a table row |
| screenshots | not bound; add a table row |
| move/resize with the mouse | not implemented |
| workspaces, tags, tiling | not implemented, by design |
| multiple monitors | tracked, not used; everything pins to one output |

## Layer-shell clients

Bars, notification daemons, launchers, and wallpaper setters work. They are not
window-managed — Satori assigns them no position or size — but river only lets
them map because Satori binds `river_layer_shell_v1`. A window manager that
does not bind it gets every layer surface closed on sight, with no error to the
client: the process starts, stays running, and never appears.

What Satori does with them:

| Thing | Behavior |
| --- | --- |
| exclusive zones | subtracted; maximized windows get what is left, so a bar is not covered |
| keyboard focus | handed over while a layer surface wants it, taken back after |
| default output | the first one; where a surface that names no output lands |
| position, size, stacking | river's, not ours |

Lock screens use `ext-session-lock-v1`, not layer shell, and are river's
business either way.

## Dependencies for the default bindings

| Binding | Package |
| --- | --- |
| Mod+Return | `foot` |
| Mod+Space | `fuzzel` |

A missing binary fails silently: `spawn` runs the command through `/bin/sh -c`
and does not report the exit status.

## Recovery

Satori exiting does not end the session. River keeps running with an empty
window manager slot: existing windows stay on screen but stop being managed —
no bindings, no focus changes, new windows never appear.

| Situation | Fix |
| --- | --- |
| Satori crashed or was killed | rerun `satori` over SSH or from a TTY; it takes the free slot |
| Satori hung mid-sequence | `pkill -x satori`, then rerun. A missed `manage_finish` blocks the compositor, so the screen is frozen but the machine is not |
| need a shell, no working binding | Ctrl+Alt+F2 for a TTY, or SSH in |

Never `pkill -f satori`: river's argv contains satori's path when it is the
`-c` command, so `-f` signals the compositor too. `pkill -x`.

`pkill -x satori` cuts the other way too — it matches *every* satori, including
one running under a nested test river. That is the right thing here, where the
live session is the target, and the wrong thing in a test script: see
`scripts/lib-nested.sh` and [TESTING.md](TESTING.md).

Set up SSH before the first TTY session. It is the only recovery channel that
does not depend on the compositor responding.

## Known gaps

Untested on a real TTY session — only ever run nested. Also:

- Output hotplug is handled (`output_removed`) but never exercised on hardware.
- Docking to a second monitor: the newest output wins, and windows follow it.
- A mid-session seat unplug dangles; there is no seat listener yet.
- No floating mode, so `Mod+F` does nothing and windows are always maximized.
