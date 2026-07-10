# Testing Satori

Satori only runs as the WM of a live river session; outside one it fails to
connect.

## Prereqs

- In a river session (`WAYLAND_DISPLAY` set)
- Built: `make`

## Test 1: active WM (nested river)

Nested river = its own window, its own empty WM slot. Satori binds as its WM.
A hang stays contained to that window.

Run from repo root:

```sh
river -c "$PWD/satori 2>/tmp/satori.log"
```

`$PWD` expands in the launching shell; from elsewhere, use satori's full path.

Watch the log:

```sh
tail -f /tmp/satori.log
```

Expect:

- first line: `bound river_window_manager_v1 v4`
- then: `wm: output`, `wm: seat`, `wm: window`, repeating `wm: manage start` /
  `wm: render start`
- window blank (no policy yet)

### Clean shutdown

```sh
pkill -INT satori
```

Expect: log ends `wm: finished`, `pgrep satori` empty.
Hang: save `/tmp/satori.log`.

### Close the nested river

`-c` runs once at startup; the nested river outlives Satori (blank window).

- Ctrl-C in the launching terminal, or
- `pgrep -f "river -c"`, then `kill <pid>`

Never `pkill river` (kills your main session too).

## Test 2: unavailable path (direct run)

```sh
./satori
```

Connects to the outer river; WM slot already taken -> `unavailable`.
Expect: `wm: unavailable`, clean exit, no `stop` sent.

## Recovery

Locked-up compositor: SSH in (openssh enabled), kill the nested river / Satori.

## Automated tests

None yet. `main.c` is mostly Wayland glue, so unit tests would test libwayland,
not Satori. First compositor-free logic: config parser, `app_id` prefix match,
MRU list. Add a `make test` target + small C test framework then.
