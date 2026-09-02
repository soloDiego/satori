// The window manager object: object creation events, and the two sequences the
// compositor drives us through.
//
// manage sequence: window management state (dimensions, focus, close, bindings).
// render sequence: node positions and stacking only.
// Touching the wrong state in the wrong sequence is a protocol error, and
// missing a manage_finish / render_finish hangs the compositor.

#include <stdio.h>

#include "satori.h"

static void wm_unavailable(void *data, struct river_window_manager_v1 *handle) {
    (void) handle;

    struct satori *satori = data;
    satori->got_unavailable = true;     // another WM holds the slot; do not send stop
    satori_log("wm: unavailable\n");
}
static void wm_finished(void *data, struct river_window_manager_v1 *handle) {
    (void) handle;

    struct satori *satori = data;
    satori->finished_received = true;
    satori_log("wm: finished\n");
}
static void wm_manage_start(void *data, struct river_window_manager_v1 *handle) {
    struct satori *satori = data;

    // Before bindings_apply_enabled: a reload replaces every binding proxy, and
    // the new ones are enabled below, in this same sequence.
    config_reload(satori);

    // Before propose: leaving fullscreen clears the proposed flag so the window
    // is re-sized in this same sequence, which is what the protocol asks for.
    layer_apply_default_output(satori);
    windows_apply_fullscreen(satori);
    windows_propose(satori);
    windows_apply_closes(satori);
    seats_apply_focus(satori);
    bindings_apply_enabled(satori);

    river_window_manager_v1_manage_finish(handle);
}
static void wm_render_start(void *data, struct river_window_manager_v1 *handle) {
    struct satori *satori = data;

    windows_render(satori);

    river_window_manager_v1_render_finish(handle);
}
static void wm_session_locked(void *data, struct river_window_manager_v1 *handle) {
    (void)data; (void)handle;
    satori_log("wm: session locked\n");
}
static void wm_session_unlocked(void *data, struct river_window_manager_v1 *handle) {
    (void)data; (void)handle;
    satori_log("wm: session unlocked\n");
}
static void wm_window(void *data, struct river_window_manager_v1 *handle, struct river_window_v1 *id) {
    (void) handle;
    window_create(data, id);
}
static void wm_output(void *data, struct river_window_manager_v1 *handle, struct river_output_v1 *id) {
    (void) handle;
    output_create(data, id);
}
static void wm_seat(void *data, struct river_window_manager_v1 *handle, struct river_seat_v1 *id) {
    (void) handle;
    seat_create(data, id);
}
const struct river_window_manager_v1_listener wm_listener = {
    .unavailable      = wm_unavailable,
    .finished         = wm_finished,
    .manage_start     = wm_manage_start,
    .render_start     = wm_render_start,
    .session_locked   = wm_session_locked,
    .session_unlocked = wm_session_unlocked,
    .window           = wm_window,
    .output           = wm_output,
    .seat             = wm_seat,
};
