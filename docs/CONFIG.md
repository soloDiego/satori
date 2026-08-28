# Config file

`$XDG_CONFIG_HOME/satori/config`, else `~/.config/satori/config`
(`config_default_path`, `src/config.c:395`). Optional — without it Satori runs
the built-in table.

[scfg](https://git.sr.ht/~emersion/scfg) format, parsed by `libscfg`. `#`
starts a comment. Parameters are whitespace-separated; `"` or `'` quote one
containing spaces. Neither quote style processes escapes, so `$`, `(`, `>` and
`&&` inside a quoted command reach the shell untouched.

```
# ~/.config/satori/config
bind Mod+Return spawn alacritty
bind Mod+Space none
app-keys Mod+Alt
```

`example/config` in the repo is a commented starting point:

```sh
mkdir -p ~/.config/satori
cp example/config ~/.config/satori/config
```

It restates the built-ins exactly, so copying it changes nothing until edited —
and because a config merges over the built-ins, *deleting* a line from it does
not unbind that chord. Use `none` for that.
`test_example_config_matches_the_defaults` keeps the file from drifting.

## Directives

| Directive | Params | Effect |
| --- | --- | --- |
| `bind` | chord, action, action args | binds the chord, replacing any binding already on it |
| `bind` | chord, `none` | unbinds the chord, including a built-in |
| `app-keys` | modifiers | modifiers for the 26 generated `focus-app` letter bindings |
| `app-keys` | `none` | omits the generated block entirely |

`app-keys` defaults to `Mod+Alt`. Repeating a directive is allowed; the last one
wins.

## Actions

`action_specs`, `src/input.c:148`. An action not listed here cannot be bound.

| Action | Args | Effect |
| --- | --- | --- |
| `spawn` | command, one or more words | runs it through `/bin/sh -c`, detached |
| `close` | none | close request to the focused window |
| `fullscreen` | none | fullscreens the focused window, or leaves fullscreen |
| `float` | none | floats the focused window, or back to maximized |
| `focus-next` | none | next window in list order, wraps |
| `focus-prev` | none | previous window in list order, wraps |
| `focus-app` | one letter | focuses a window whose `app_id` starts with that letter |
| `reload` | none | re-reads this file |
| `exit` | none | ends the session, no confirmation |

`spawn` joins its remaining parameters with single spaces, so quoting is only
needed to protect runs of whitespace:

```
bind Mod+V spawn wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+
bind Mod+B spawn "sh -c 'echo  two spaces'"
```

## Chords

`<modifier>+...+<key>`. Every token but the last is a modifier; the last is an
xkbcommon keysym name. Zero modifiers is legal.

| Modifier | Accepted names | Value |
| --- | --- | --- |
| shift | `Shift` | 1 |
| ctrl | `Ctrl`, `Control` | 4 |
| mod1 | `Alt`, `Mod1` | 8 |
| mod3 | `Mod3` | 32 |
| mod4 | `Mod`, `Super`, `Logo`, `Mod4` | 64 |
| mod5 | `Mod5` | 128 |

Names are case-insensitive (`modifier_names`, `src/config.c:30`). Capslock (2)
and numlock (16) are excluded upstream: locked modifiers are not usable in
bindings.

Keysym names are matched exactly first, then case-insensitively, so `Space`,
`space`, `Return` and `return` all resolve. Names are the `XKB_KEY_*` suffixes
in `/usr/include/xkbcommon/xkbcommon-keysyms.h`.

**Keysyms are lowered before storage** (`chord_parse`, `src/config.c:87`). River
matches the *unshifted* keysym, so `Mod+Shift+E` is stored as `e` with the shift
bit. Writing `E` yourself would otherwise bind without error and never fire.

## Precedence

The table is assembled in this order, each layer overriding the one above on the
same keysym+modifier pair:

1. Built-in bindings (`defaults`, `src/input.c:205`)
2. The generated `focus-app` letter block, unless `app-keys none`
3. `bind` lines, in file order

So a config file **merges over** the built-ins rather than replacing them: an
unmentioned chord stays bound, and `bind <chord> none` is the only way to take
one away. An explicit `bind` on a letter chord also wins over the generated
block, because the block goes in first.

One binding per keysym+modifier pair — `config_set` (`src/config.c:148`)
replaces in place rather than appending. Which of two bindings on one chord
would fire is compositor policy.

## Reloading

| Trigger | Notes |
| --- | --- |
| `Mod+Shift+R` | works with no terminal open |
| `SIGHUP` | `pkill -HUP satori`; for editor hooks and scripts |

Both set `reload_pending`; the reload itself runs in the next manage sequence
(`config_reload`, `src/input.c:348`). SIGHUP additionally requests that sequence
with `manage_dirty` (`src/main.c:150`) — a signal, unlike a keypress, does not
bring one with it.

A reload replaces every `river_xkb_binding_v1` proxy on every seat. Changed
bindings take effect immediately; nothing else about the session is disturbed.

## Errors

Any error in the file rejects the **whole file**. All errors are reported
together, each with a line number, on stderr.

| Situation | Result |
| --- | --- |
| no file | built-in table, not an error |
| file unreadable or malformed, at startup | built-in table, logged |
| any bad directive, at startup | built-in table, logged |
| any bad directive, on reload | the running bindings are kept |

The running table is never cleared before its replacement is known good. Satori
owns every binding in the session, so a reload that emptied the table and then
failed would leave no terminal, no launcher, and no way to fix the file that
caused it.

Startup logs the count and the path it read:

```
config: 41 bindings from /home/diego/.config/satori/config
```

## See also

- [KEYBINDS.md](KEYBINDS.md) — the built-in table and what each binding does
- [SEQUENCES.md](SEQUENCES.md) — why a reload waits for a manage sequence
