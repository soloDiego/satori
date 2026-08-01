// Windows: tracking, focus bookkeeping, and the per-sequence work that makes
// them visible. Every request in here is sequence-scoped -- see the function
// comments in satori.h for which sequence each belongs to.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "satori.h"

static void win_closed(void *data, struct river_window_v1 *handle) {
    (void) handle;

    struct window *win = data;
    struct satori *satori = win->satori;

    // Unlink before freeing: find the pointer slot that holds win.
    struct window **pp = &satori->windows;
    while (*pp != win) {
        pp = &(*pp)->next;
    }
    *pp = win->next;

    // The list head is the newest surviving window.
    if (satori->focused == win) {
        window_focus(satori, satori->windows);
    }

    river_node_v1_destroy(win->node);
    river_window_v1_destroy(win->handle);
    free(win->app_id);
    free(win->title);
    free(win);
}
static void win_dimensions(void *data, struct river_window_v1 *handle, int32_t width, int32_t height) {
    (void) handle;

    struct window *win = data;
    win->width = width;
    win->height = height;
    fprintf(stderr, "window: %dx%d\n", width, height);
}
static void win_app_id(void *data, struct river_window_v1 *handle, const char *app_id) {
    (void) handle;

    // The server owns app_id only for the duration of this call, so copy it.
    struct window *win = data;
    free(win->app_id);
    win->app_id = app_id ? strdup(app_id) : NULL;
}
static void win_title(void *data, struct river_window_v1 *handle, const char *title) {
    (void) handle;

    struct window *win = data;
    free(win->title);
    win->title = title ? strdup(title) : NULL;
}
static void win_dimensions_hint(void *data, struct river_window_v1 *handle, int32_t min_width,
        int32_t min_height, int32_t max_width, int32_t max_height) {
    (void)data; (void)handle; (void)min_width; (void)min_height; (void)max_width; (void)max_height;
}
static void win_parent(void *data, struct river_window_v1 *handle, struct river_window_v1 *parent) {
    (void)data; (void)handle; (void)parent;
}
static void win_decoration_hint(void *data, struct river_window_v1 *handle, uint32_t hint) {
    (void)data; (void)handle; (void)hint;
}
static void win_pointer_move_requested(void *data, struct river_window_v1 *handle, struct river_seat_v1 *seat) {
    (void)data; (void)handle; (void)seat;
}
static void win_pointer_resize_requested(void *data, struct river_window_v1 *handle, struct river_seat_v1 *seat,
        uint32_t edges) {
    (void)data; (void)handle; (void)seat; (void)edges;
}
static void win_show_window_menu_requested(void *data, struct river_window_v1 *handle, int32_t x, int32_t y) {
    (void)data; (void)handle; (void)x; (void)y;
}
static void win_maximize_requested(void *data, struct river_window_v1 *handle) { (void)data; (void)handle; }
static void win_unmaximize_requested(void *data, struct river_window_v1 *handle) { (void)data; (void)handle; }
static void win_fullscreen_requested(void *data, struct river_window_v1 *handle, struct river_output_v1 *output) {
    (void)data; (void)handle; (void)output;
}
static void win_exit_fullscreen_requested(void *data, struct river_window_v1 *handle) { (void)data; (void)handle; }
static void win_minimize_requested(void *data, struct river_window_v1 *handle) { (void)data; (void)handle; }
static void win_unreliable_pid(void *data, struct river_window_v1 *handle, int32_t unreliable_pid) {
    (void)data; (void)handle; (void)unreliable_pid;
}
static void win_presentation_hint(void *data, struct river_window_v1 *handle, uint32_t hint) {
    (void)data; (void)handle; (void)hint;
}
static void win_identifier(void *data, struct river_window_v1 *handle, const char *id) {
    (void)data; (void)handle; (void)id;
}
static const struct river_window_v1_listener window_listener = {
    .closed = win_closed,
    .dimensions_hint = win_dimensions_hint,
    .dimensions = win_dimensions,
    .app_id = win_app_id,
    .title = win_title,
    .parent = win_parent,
    .decoration_hint = win_decoration_hint,
    .pointer_move_requested = win_pointer_move_requested,
    .pointer_resize_requested = win_pointer_resize_requested,
    .show_window_menu_requested = win_show_window_menu_requested,
    .maximize_requested = win_maximize_requested,
    .unmaximize_requested = win_unmaximize_requested,
    .fullscreen_requested = win_fullscreen_requested,
    .exit_fullscreen_requested = win_exit_fullscreen_requested,
    .minimize_requested = win_minimize_requested,
    .unreliable_pid = win_unreliable_pid,
    .presentation_hint = win_presentation_hint,
    .identifier = win_identifier,
};

void window_create(struct satori *satori, struct river_window_v1 *handle) {
    struct window *win = calloc(1, sizeof *win);
    if (!win) {
        fprintf(stderr, "window_create: calloc failed\n");
        return;
    }
    win->handle = handle;
    win->satori = satori;
    win->node = river_window_v1_get_node(handle);   // protocol error to ask twice

    win->next = satori->windows;
    satori->windows = win;

    river_window_v1_add_listener(handle, &window_listener, win);
    window_focus(satori, win);      // new windows open focused
    fprintf(stderr, "wm: window\n");
}

void window_focus(struct satori *satori, struct window *win) {
    if (satori->focused == win) return;
    satori->focused = win;
    satori->focus_dirty = true;
}

void windows_propose(struct satori *satori) {
    struct output *out = satori->outputs;
    if (!out) return;

    for (struct window *win = satori->windows; win; win = win->next) {
        // The server answers every proposal with a dimensions event, which
        // starts another sequence: re-proposing unconditionally never settles.
        if (win->proposed) continue;
        river_window_v1_propose_dimensions(win->handle, out->width, out->height);
        river_window_v1_inform_maximized(win->handle);
        win->proposed = true;
    }
}

void windows_apply_closes(struct satori *satori) {
    for (struct window *win = satori->windows; win; win = win->next) {
        if (!win->close_pending) continue;
        river_window_v1_close(win->handle);
        win->close_pending = false;     // the client answers with a closed event, or ignores us
    }
}

void windows_render(struct satori *satori) {
    struct output *out = satori->outputs;

    // The list runs newest to oldest and each window is pushed to the bottom in
    // turn, which leaves the newest on top. place_top would invert that.
    for (struct window *win = satori->windows; win; win = win->next) {
        if (!win->node) continue;
        river_node_v1_set_position(win->node, out ? out->x : 0, out ? out->y : 0);
        river_node_v1_place_bottom(win->node);
    }
}

void windows_destroy_all(struct satori *satori) {
    struct window *win = satori->windows;
    while (win) {
        struct window *next = win->next;
        river_node_v1_destroy(win->node);
        river_window_v1_destroy(win->handle);
        free(win->app_id);
        free(win->title);
        free(win);
        win = next;
    }
    satori->windows = NULL;
    satori->focused = NULL;
}
