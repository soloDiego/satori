# The manage/render state machine

Scope: Satori's side of `river-window-management-v1`. The protocol XML in
`protocol/` is the authority; this is the part that is easy to get wrong.

## Why it exists

River never applies window manager state as it arrives. It batches: it sends
everything it has (new windows, new app_ids, key presses), then a `manage_start`,
and waits. Satori answers with requests and a `manage_finish`. Only then does
river push the new state to clients and wait for their acks. As sizes resolve it
sends `dimensions` events and a `render_start`, and Satori answers with node
positions and a `render_finish`.

The point is atomicity: a window is never half-configured on screen, and Satori
never races an input event it has not seen yet.

Consequence for the code: **an event handler records intent, a sequence applies
it.** `binding_pressed` (`src/input.c:293`) sets `close_pending` or moves
`satori->focused`; `wm_manage_start` (`src/wm.c:27`) turns that into
`river_window_v1_close` and `river_seat_v1_focus_window`.

## Which state belongs to which sequence

| State | Sequence | Satori |
| --- | --- | --- |
| dimensions, maximize/fullscreen, focus, close, capabilities, binding enable/disable | manage only | `windows_apply_fullscreen`, `windows_propose`, `windows_apply_closes`, `seats_apply_focus`, `bindings_apply_enabled` |
| node position, stacking, borders, clip boxes | manage or render | `windows_render` |
| layer shell default output | manage only | `layer_apply_default_output` |

`exit_session` belongs to neither: it is not window management state, so
`action_exit_session` calls it straight from the binding handler. It is the only
action that is not deferred.

## Invariants

- Every `manage_start` is answered with exactly one `manage_finish`, every
  `render_start` with one `render_finish`. Miss one and the compositor waits
  forever — that is the hang, and it takes the session with it on a real TTY.
- Window management state is touched only inside a manage sequence. Outside it,
  river answers with a `sequence_order` protocol error and disconnects.
- A new window is invisible until it has been proposed dimensions (or made
  fullscreen) *and* the render sequence that follows the resulting `dimensions`
  event has finished.
- Every proposal is answered with a `dimensions` event, which starts another
  sequence. Proposing unconditionally in `manage_start` never settles — hence
  the `proposed` flag and `enabled` on bindings. Any new per-window request
  needs the same treatment.
- Leaving fullscreen leaves the window's dimensions and position undefined until
  both are set again, and the protocol asks for that in the same manage
  sequence. `windows_apply_fullscreen` runs before `windows_propose` and clears
  `proposed` so the re-proposal lands in that sequence; position is set inline,
  which a manage sequence may do because it is rendering state.
- A fullscreen window's size belongs to the compositor: `propose_dimensions` and
  `set_position` do not affect it, and `windows_propose` skips it.
- `get_node` is once per window, ever. Twice is a protocol error.
- `wl_output` and `wl_seat` globals can disappear from `wl_registry` before the
  matching `river_output_v1.removed` / `river_seat_v1.removed` arrives: globals
  are not synced to sequences. Track the two independently.
- Shutdown while active: `stop`, wait for `finished`, then `destroy`. Never just
  disconnect. Shutdown after `unavailable`: `destroy` only — `stop` from a
  window manager that was never active is an error.
- `unavailable` also has to *end the loop*. It is the first and only event that
  object gets, so nothing will ever wake us again; the event loop checks it
  right after `wl_display_prepare_read` and cancels the pending read on the way
  out (`src/main.c:88`). Without that check satori sits in `poll` forever.

## Reloading the config

A reload destroys every `river_xkb_binding_v1` and frees the table they point
into. Both the trigger and the teardown order are constrained.

`get_xkb_binding` and `river_xkb_binding_v1.destroy` carry no sequence
constraint — only `enable`, `disable` and `set_layout_override` do. So a reload
*could* run anywhere. It runs in the manage sequence anyway, for two reasons:

- The keypress that asks for it is inside `binding_pressed`, reading
  `bind->keybind`. Rebuilding there frees the keybind the running callback is
  reading and destroys the proxy libwayland is dispatching on. `action_reload`
  records intent like every other action; `config_reload` runs later.
- The fresh bindings need `enable`, which *is* window management state.
  `config_reload` runs first in `manage_start`, so `bindings_apply_enabled`
  enables them a few lines later in that same sequence.

The protocol endorses this directly: the `pressed` event's description says the
compositor waits for the manage sequence to complete "to allow the window
manager client to, for example, modify key bindings ... without racing against
future input events."

Teardown order is the part that bites. Every `struct binding` borrows a
`&config->binds[i]`, and `binds` is a growable array — a `realloc` moves it. Two
rules follow:

- A config is built to completion before any binding borrows into it, and is
  never grown again afterwards. A reload builds a whole new one.
- Every proxy is destroyed *before* the table it borrows from is freed. Getting
  this backwards is a use-after-free in `binding_pressed`, not a wrong answer —
  deleting the `bindings_destroy_all` from `config_reload` crashes satori on the
  next keypress under ASan.

The new table is parsed first and swapped in only if it parsed. Satori owns
100% of input, so a reload that cleared the bindings and then failed to rebuild
them is a session with no terminal, no launcher, and no way to fix the file that
broke it. On failure the running bindings are left exactly as they were.

SIGHUP needs one extra step a keybind does not: a signal arrives with no manage
sequence behind it, so the loop calls `manage_dirty` to ask for one
(`src/main.c:155`). Without it the reload sits pending until something else
happens to move a window. This is the protocol's own worked example for
`manage_dirty` — its description names an internal state change the compositor
cannot see.

## Stacking

`windows` is prepended, so it runs newest to oldest, and `windows_render` pushes
each node to the bottom in turn (`src/window.c:395`). The last one pushed is the
oldest, so the newest ends on top. Walking the same list with `place_top` inverts
it and buries new windows.

Then the focused window is raised with `place_top`. **Unconditionally** — the
walk above re-asserts newest-on-top on every render and would bury it again on
the next one.

Without the raise, stacking follows creation order alone and focus does not
affect it. That is not a cosmetic bug: every window is maximized, so cycling
moves the keyboard focus to a window that is completely hidden behind the newest
one, and keystrokes land in a window you cannot see. It reads as "Mod+J does
nothing" — the focus request was going out correctly the whole time.

`satori->raised` exists only to log the raise on change; it is never
dereferenced. `win_closed` still clears it, so a later window allocated at the
same address cannot compare equal to a dead one.

## Outputs going away

`river_output_v1.removed` is followed by a `manage_start`, so unlinking the
output and clearing per-window state is enough — no `manage_dirty` needed.
`windows_forget_output` runs before the unlink, because windows hold pointers
into the struct being freed. It also clears `proposed` on every window: they
were sized against an output that is about to stop existing.

The compositor exits fullscreen by itself when the output a window is fullscreen
on is removed, so Satori only corrects its own record.

## Fullscreen has two entry points

`win_fullscreen_requested` (the client asked) and `action_toggle_fullscreen`
(Mod+F) both only set `fullscreen` + `fullscreen_dirty` on the window;
`windows_apply_fullscreen` is the single place either one is applied. Both the
client's request events and a binding's `pressed` are followed by a
`manage_start`, so neither needs `manage_dirty`.

`fullscreen_dirty` is cleared once applied, so the toggle has to set it on every
press. Setting it only on the way in strands the window fullscreen.

## Floating, and why it needs no dirty flag of its own

Same two entry points: `win_unmaximize_requested` / `win_maximize_requested`
(the client asked) and `action_toggle_floating` (Mod+Shift+Space). All three set
`floating` and clear `proposed`; `windows_propose` is the single place either is
applied. Both maximize events are followed by a `manage_start`.

Unlike fullscreen there is no `floating_dirty`. **`proposed` is the dirty bit** —
the toggle is applied only because clearing it makes `windows_propose` run again,
which re-sizes the window and sends `inform_maximized` or `inform_unmaximized`.
A toggle that leaves `proposed` set flips the flag and changes nothing on screen.

Geometry:

- Sized once, on the first float: two thirds of the usable area, centered
  (`window_init_float_geometry`). `float_width <= 0` is the "unset" marker.
- `window_position` is split out of `window_place` so the choice between float
  coordinates and the usable-area origin is unit-testable. It has to be —
  headless tests prove the protocol, not pixels, so a floating window parked in
  the corner passes every integration assertion.
- `windows_forget_output` zeroes the float size along with `proposed`: the
  coordinates belong to the output that is going away, and keeping them parks
  the window off screen on whatever output is left.
- A window toggled while fullscreen only records the state. `windows_propose`
  skips fullscreen windows, so it is sized on the way out.

## Passthrough, and why disabling *is* forwarding

Satori has no raw key path. River matches bindings itself and sends `pressed`
only for chords that already matched; the XML says the rest go straight to the
focused surface, and that bound chords are **not** delivered to it. So there is
no lookup to skip and no key event to forward.

`bindings_apply_enabled` disables every non-exempt binding while the focused
window's `app_id` is listed in `passthrough`. The compositor then stops matching
those chords, and they reach the client like any other key. That is the whole
mechanism.

`enable` and `disable` are window management state, so this runs in the manage
sequence. `action_toggle_passthrough` records nothing but a per-window flag; the
sequence that follows the keypress applies it, same as every other action.

**Recomputed every manage sequence, with no dirty flag.** The inputs are focus,
the focused window's `app_id`, the escape toggle and the config — and `app_id`
arrives on its own event *after* the `window` event that created the struct. A
flag set at focus time would miss it and leave a passthrough app fully bound
until something else moved a window. The walk is a bool compare per binding and
only sends a request when one actually changes.

The exemption is structural, not an ordering check: exempt bindings are never
disabled, so no sequencing mistake can shadow the escape hatch. It lives on the
`action_spec`, not the chord, so re-binding `passthrough` carries it — a
chord-level flag would let a config move the binding and silently lose the only
way back out. Two actions are exempt (`passthrough`, `exit`) so there are two
independent ways out of a passthrough app.

Stuck modifiers on a focus change are not Satori's to fix, and it has no way to:
there is no request in either protocol to inject key events into a client. Nor
is one needed — river never delivers press *or* release of a bound chord to the
focused surface, and `wl_keyboard.leave` already obliges clients to treat every
key as released.

The one live version of that hazard is internal: disabling a binding between its
`pressed` and `released` means the `released` may never arrive. Nothing strands
today, because `binding_released` is a no-op. It becomes real with key repeat —
whatever tracks a repeating action has to be cancelled where the binding is
disabled.

## Recency, and why there are two window lists

`satori->windows` is creation order. `satori->mru` is the same windows in
most-recently-focused order, linked through `mru_next`. Every window is in both,
always.

One list would have been enough for the lookup: promoting the focused window to
the head of `windows` gives recency, newest-on-top and raise-on-focus from a
single invariant. It was rejected because it makes Mod+J/K reorder the thing it
is walking — cycling stops being a ring and becomes alt-tab bouncing between the
last two windows — and it ties stacking order to focus order.

So the two questions get the two lists (`window_find_by_app`,
`src/window.c:219`). Arriving from another app, the answer is recency: the
window of that app you used last. Already in that app, recency would bounce
between the same two windows forever, so the answer is the next match in
creation order, wrapping — the same ring Mod+J/K walks.

Invariants:

- Recency is maintained in `window_focus`, not in the actions, so every route to
  focusing a window feeds the lookup: bindings, a new window opening focused, and
  focus falling through when the focused window closes.
- `window_focus` returns early when focus is unchanged, so the promotion is
  skipped then too. That is correct — the window is already the head.
- After any focus change the focused window *is* the `mru` head. Nothing depends
  on it, and `window_find_by_app` does not assume it.
- `win_closed` unlinks from both lists. Missing the `mru` unlink is a
  use-after-free the next time a letter is pressed, not a wrong answer.
- Focus falls through to the `mru` head, not the newest window, so the recency
  list and the focus stay in agreement.
- `window_mru_unlink` tolerates a window that is not linked in;
  the `windows` unlink (`src/window.c:39`) deliberately does not, because only
  `window_create` adds and only `closed` removes.

None of this is protocol state. It is read inside a binding handler, outside any
sequence, and only ever produces a `window_focus` — which is intent, applied by
`seats_apply_focus` in the manage sequence that follows.

## Layer shell

Binding `river_layer_shell_v1` is what tells river Satori supports layer
surfaces. Without the bind river closes every one, silently — the launcher
starts and never appears. The bind is the feature; the rest is arbitration.

- `non_exclusive_area` gives the output minus reserved zones.
  `windows_propose` sizes to it, so a bar is not covered. The handler compares
  before storing: re-proposing on an unchanged area would never settle, same
  trap as `proposed`.
- All four layer events (`non_exclusive_area`, the three focus ones) are
  followed by a `manage_start`. Record intent, no `manage_dirty`.
- Focus is arbitrated, not owned. On `focus_exclusive` our requests are ignored
  outright; on `focus_non_exclusive` a focus request in that same sequence
  cancels the layer surface's focus. `seats_apply_focus` skips those seats and
  leaves `focus_dirty` set, so `focus_none` gets focus back where it was.
  Clearing the flag there loses focus for good.
- `get_output` / `get_seat` are once per output/seat, ever — like `get_node`.
  Both objects go inert on the corresponding `removed` event and must be
  destroyed before the `river_layer_shell_v1` they came from.
- A layer surface that names no output needs a default, or it has nowhere to go.
  `set_default` is manage-sequence state; `default_output_dirty` carries it.

## What is not modelled yet

- Everything pins to `satori->outputs`, the first output. Single monitor by
  construction: a second output is tracked but never used, and window position
  comes from the head of the list.
- An output *resize* still leaves windows at the old size — only removal clears
  `proposed`.
- A window arriving before its output's `dimensions` event proposes 0x0, which
  means "client picks", and never corrects itself.
- No seat listener, so a mid-session seat unplug leaves a dangling `struct seat`.
- Floating: `inform_maximized` is unconditional for non-fullscreen windows.
