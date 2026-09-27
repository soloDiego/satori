# Status feed

Window list for bars. `src/status.c`.

```sh
satori status
```

Connects, prints one JSON line per change until satori exits. First line = current state.

```json
{"focused":0,"windows":[{"app_id":"foot","title":"~"},{"app_id":"zen","title":"GitHub"}]}
```

- `windows`: creation order, newest first — the `focus-next` direction.
- `focused`: index into `windows`; `-1` = nothing focused.
- `app_id` / `title`: `""` until the client sends one.
- Sent on: window open/close, focus change, app_id/title change. At most one line per loop wakeup.

## Socket

- `$XDG_RUNTIME_DIR/satori-<WAYLAND_DISPLAY>.sock`. Per display, so a nested test session never takes the live one's.
- Read-only. Up to 8 clients (`SATORI_STATUS_CLIENTS`).
- Slow or gone client: dropped, not buffered. Next line supersedes the last.
- Live socket (something answers) = left alone; a second satori runs without one. Dead socket = removed and rebound.
- Removed on clean exit.

## Waybar

`custom` module, `exec` a script that pipes `satori status` through `jq` into waybar's JSON (`text`/`tooltip`/`class`). Set `restart-interval`: the script exits when satori does. Example: `local/bin/bar-windows` in the author's dotfiles.

Expect: `status: /run/user/1000/satori-wayland-1.sock` in the log at startup, `status: client` per connect.
