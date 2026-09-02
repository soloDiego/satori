// Layer shell: panels, bars, launchers, wallpapers, notification daemons.
//
// River closes layer surfaces immediately unless the window manager binds
// river_layer_shell_v1 -- binding it is what declares support, so without this
// file a launcher starts, gets its surface closed, and hangs around invisible.
//
// Two per-object channels come with it: the output object reports the area left
// over after panels reserve exclusive zones, and the seat object reports when a
// layer surface takes keyboard focus out of our hands.

#include <stdio.h>

#include "satori.h"

static void layer_output_non_exclusive_area(void *data, struct river_layer_shell_output_v1 *handle,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    (void) handle;

    struct output *out = data;

    // Re-proposing on every event would never settle: each proposal is answered
    // with a dimensions event, which starts another sequence.
    if (out->has_area && out->area_x == x && out->area_y == y &&
            out->area_width == width && out->area_height == height) {
        return;
    }
    out->has_area = true;
    out->area_x = x;
    out->area_y = y;
    out->area_width = width;
    out->area_height = height;

    // Maximized windows are sized to this area, so they all need re-proposing.
    // The event is followed by a manage_start, so no manage_dirty is needed.
    windows_invalidate_layout(out->satori);
    satori_log("layer: area %dx%d at %d,%d\n", width, height, x, y);
}
static const struct river_layer_shell_output_v1_listener layer_output_listener = {
    .non_exclusive_area = layer_output_non_exclusive_area,
};

// All three events are followed by a manage_start, so recording intent is enough.
static void layer_seat_focus_exclusive(void *data, struct river_layer_shell_seat_v1 *handle) {
    (void) handle;

    struct seat *seat = data;
    seat->layer_focus = true;
    satori_log("layer: focus exclusive\n");
}
static void layer_seat_focus_non_exclusive(void *data, struct river_layer_shell_seat_v1 *handle) {
    (void) handle;

    struct seat *seat = data;
    seat->layer_focus = true;
    satori_log("layer: focus non-exclusive\n");
}
static void layer_seat_focus_none(void *data, struct river_layer_shell_seat_v1 *handle) {
    (void) handle;

    struct seat *seat = data;
    seat->layer_focus = false;
    seat->satori->focus_dirty = true;   // the layer surface let go; take focus back
    satori_log("layer: focus none\n");
}
static const struct river_layer_shell_seat_v1_listener layer_seat_listener = {
    .focus_exclusive     = layer_seat_focus_exclusive,
    .focus_non_exclusive = layer_seat_focus_non_exclusive,
    .focus_none          = layer_seat_focus_none,
};

void layer_output_create(struct satori *satori, struct output *out) {
    if (!satori->layer_shell) return;

    // Protocol error to ask twice for the same output, so this is only ever
    // called from output_create.
    out->layer = river_layer_shell_v1_get_output(satori->layer_shell, out->handle);
    river_layer_shell_output_v1_add_listener(out->layer, &layer_output_listener, out);

    // A layer surface that names no output of its own lands on the default one,
    // and there is no default until we pick it.
    satori->default_output_dirty = true;
}

void layer_output_destroy(struct output *out) {
    if (!out->layer) return;
    river_layer_shell_output_v1_destroy(out->layer);
    out->layer = NULL;
}

void layer_seat_create(struct satori *satori, struct seat *seat) {
    if (!satori->layer_shell) return;

    seat->layer = river_layer_shell_v1_get_seat(satori->layer_shell, seat->handle);
    river_layer_shell_seat_v1_add_listener(seat->layer, &layer_seat_listener, seat);
}

void layer_seat_destroy(struct seat *seat) {
    if (!seat->layer) return;
    river_layer_shell_seat_v1_destroy(seat->layer);
    seat->layer = NULL;
}

void layer_apply_default_output(struct satori *satori) {
    if (!satori->default_output_dirty) return;

    struct output *out = satori->outputs;
    if (!out || !out->layer) return;    // nothing to point at yet; stay dirty

    river_layer_shell_output_v1_set_default(out->layer);
    satori->default_output_dirty = false;
    satori_log("layer: default output\n");
}
