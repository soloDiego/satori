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
it.** `binding_pressed` (`src/input.c:83`) sets `close_pending` or moves
`satori->focused`; `wm_manage_start` (`src/wm.c:27`) turns that into
`river_window_v1_close` and `river_seat_v1_focus_window`.

## Which state belongs to which sequence

| State | Sequence | Satori |
| --- | --- | --- |
| dimensions, maximize/fullscreen, focus, close, capabilities, binding enable/disable | manage only | `windows_propose`, `windows_apply_closes`, `seats_apply_focus`, `bindings_enable_pending` |
| node position, stacking, borders, clip boxes | manage or render | `windows_render` (`src/window.c:161`) |

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
  the `proposed` flag (`src/satori.h:53`) and `enabled` on bindings. Any new
  per-window request needs the same treatment.
- `get_node` is once per window, ever. Twice is a protocol error.
- `wl_output` and `wl_seat` globals can disappear from `wl_registry` before the
  matching `river_output_v1.removed` / `river_seat_v1.removed` arrives: globals
  are not synced to sequences. Track the two independently.
- Shutdown while active: `stop`, wait for `finished`, then `destroy`. Never just
  disconnect. Shutdown after `unavailable`: `destroy` only — `stop` from a
  window manager that was never active is an error.

## Stacking

`windows` is prepended, so it runs newest to oldest, and `windows_render` pushes
each node to the bottom in turn (`src/window.c:169`). The last one pushed is the
oldest, so the newest ends on top. Walking the same list with `place_top` inverts
it and buries new windows.

## What is not modelled yet

- `proposed` is never cleared. An output resize or a second output leaves
  existing windows at the old size.
- Everything pins to `satori->outputs`, the first output. Single monitor by
  construction.
- A window arriving before its output's `dimensions` event proposes 0x0, which
  means "client picks", and never corrects itself.
