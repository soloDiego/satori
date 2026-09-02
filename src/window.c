// Windows: tracking, focus bookkeeping, and the per-sequence work that makes
// them visible. Every request in here is sequence-scoped -- see the function
// comments in satori.h for which sequence each belongs to.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "satori.h"

// Recency list. Tolerates a window that is not linked in, so window_focus can
// call it on anything; the creation-order unlink below cannot, and does not
// need to -- only window_create adds and only closed removes.
static void window_mru_unlink(struct satori *satori, struct window *win) {
    struct window **pp = &satori->mru;
    while (*pp && *pp != win) {
        pp = &(*pp)->mru_next;
    }
    if (*pp) *pp = win->mru_next;
    win->mru_next = NULL;
}

static void window_mru_promote(struct satori *satori, struct window *win) {
    if (satori->mru == win) return;
    window_mru_unlink(satori, win);
    win->mru_next = satori->mru;
    satori->mru = win;
}

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
    window_mru_unlink(satori, win);

    // Focus falls through to the window you used most recently, not the newest
    // one, which keeps the recency list and the focus in agreement. win is
    // already unlinked, so mru is a survivor or NULL -- never win itself, and
    // window_focus therefore cannot early-return here.
    if (satori->focused == win) {
        window_focus(satori, satori->mru);
    }
    if (satori->raised == win) satori->raised = NULL;

    river_node_v1_destroy(win->node);
    river_window_v1_destroy(win->handle);
    free(win->app_id);
    free(win->title);
    free(win);
    satori_log("window: closed\n");
}
static void win_dimensions(void *data, struct river_window_v1 *handle, int32_t width, int32_t height) {
    (void) handle;

    struct window *win = data;
    win->width = width;
    win->height = height;
    satori_log("window: %dx%d\n", width, height);
}
static void win_app_id(void *data, struct river_window_v1 *handle, const char *app_id) {
    (void) handle;

    // The server owns app_id only for the duration of this call, so copy it.
    struct window *win = data;
    free(win->app_id);
    win->app_id = app_id ? strdup(app_id) : NULL;

    // Logged because it is the only way to find out what to write in a
    // `passthrough` line. An app_id is not the window title and not the command
    // name, and nothing else in the session will tell you it.
    satori_log("window: app_id %s\n", win->app_id ? win->app_id : "(none)");
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
// The client's half of the float toggle. A window asking to be unmaximized wants
// to keep its own size, which is what floating is here, so both events record
// exactly what the binding records and windows_propose applies either one.
// Both are followed by a manage_start, so no manage_dirty is needed.
static void win_maximize_requested(void *data, struct river_window_v1 *handle) {
    (void) handle;

    struct window *win = data;
    if (!win->floating) return;     // already maximized; nothing to re-propose
    win->floating = false;
    win->proposed = false;
}
static void win_unmaximize_requested(void *data, struct river_window_v1 *handle) {
    (void) handle;

    struct window *win = data;
    if (win->floating) return;
    win->floating = true;
    win->proposed = false;
}
// Both fullscreen events are followed by a manage_start, so recording intent is
// enough -- no manage_dirty needed to get a sequence.
static void win_fullscreen_requested(void *data, struct river_window_v1 *handle, struct river_output_v1 *output) {
    (void) handle;

    struct window *win = data;
    win->fullscreen = true;
    win->fullscreen_dirty = true;
    // A null output means the client had no preference; fall back at apply time.
    win->fs_output = output ? output_from_handle(win->satori, output) : NULL;
}
static void win_exit_fullscreen_requested(void *data, struct river_window_v1 *handle) {
    (void) handle;

    struct window *win = data;
    win->fullscreen = false;
    win->fullscreen_dirty = true;
    win->fs_output = NULL;
}
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
        satori_log("window_create: calloc failed\n");
        return;
    }
    win->handle = handle;
    win->satori = satori;
    win->node = river_window_v1_get_node(handle);   // protocol error to ask twice

    win->next = satori->windows;
    satori->windows = win;
    win->mru_next = satori->mru;
    satori->mru = win;

    river_window_v1_add_listener(handle, &window_listener, win);
    window_focus(satori, win);      // new windows open focused
    satori_log("wm: window\n");
}

void window_focus(struct satori *satori, struct window *win) {
    if (satori->focused == win) return;
    satori->focused = win;
    satori->focus_dirty = true;
    // Recency is maintained here rather than in the actions, so every way of
    // focusing a window feeds the lookup -- bindings, new windows, and focus
    // falling through when the focused window closes.
    if (win) window_mru_promote(satori, win);
}

// Case-insensitive first-letter match. A window with no app_id yet matches
// nothing rather than matching everything.
static bool window_app_starts_with(const struct window *win, char letter) {
    if (!win->app_id || !win->app_id[0]) return false;
    return tolower((unsigned char) win->app_id[0]) == tolower((unsigned char) letter);
}

// The window Mod+Alt+<letter> should focus, or NULL when nothing matches.
//
// Two questions, so two lists. Coming from another app the answer is recency --
// the window of that app you used last, which is the whole point of the feature.
// Already in that app, recency would bounce between the same two windows
// forever, so the answer is the next match in creation order: a stable ring,
// the same shape as Mod+J/K.
struct window *window_find_by_app(const struct satori *satori, char letter) {
    struct window *focused = satori->focused;

    if (!focused || !window_app_starts_with(focused, letter)) {
        for (struct window *win = satori->mru; win; win = win->mru_next) {
            if (window_app_starts_with(win, letter)) return win;
        }
        return NULL;
    }

    // One lap of the ring, starting after the focused window.
    for (struct window *win = focused->next; win; win = win->next) {
        if (window_app_starts_with(win, letter)) return win;
    }
    for (struct window *win = satori->windows; win != focused; win = win->next) {
        if (window_app_starts_with(win, letter)) return win;
    }
    return focused;     // the only window of its app
}

// Where a window's node goes: its own coordinates when floating, otherwise the
// top left of the output's usable area. Kept separate from the request below so
// there is something to unit test -- a placement bug is otherwise invisible to
// every automated test we have, since headless proves protocol, not pixels.
void window_position(const struct window *win, const struct output *out,
        int32_t *x, int32_t *y) {
    if (win->floating) {
        *x = win->float_x;
        *y = win->float_y;
        return;
    }

    int32_t width, height;
    output_usable_area(out, x, y, &width, &height);
    (void) width; (void) height;    // placement needs the origin only
}

// Node position is rendering state, which a manage sequence may also touch --
// that is what lets the fullscreen exit path call this.
static void window_place(struct window *win, const struct output *out) {
    int32_t x, y;
    window_position(win, out, &x, &y);
    river_node_v1_set_position(win->node, x, y);
}

// The geometry a window gets the first time it floats: two thirds of the usable
// area, centered. Nothing moves or resizes a floating window yet, so this is the
// only geometry it ever gets.
void window_init_float_geometry(struct window *win, const struct output *out) {
    int32_t x, y, width, height;
    output_usable_area(out, &x, &y, &width, &height);

    win->float_width  = width  * 2 / 3;
    win->float_height = height * 2 / 3;
    win->float_x = x + (width  - win->float_width)  / 2;
    win->float_y = y + (height - win->float_height) / 2;
}

// The area windows are sized against changed; every one needs re-proposing.
void windows_invalidate_layout(struct satori *satori) {
    for (struct window *win = satori->windows; win; win = win->next) {
        win->proposed = false;
    }
}

// Drop every reference to an output that is going away. The manage sequence
// that follows the removed event re-proposes what is left.
void windows_forget_output(struct satori *satori, struct output *out) {
    for (struct window *win = satori->windows; win; win = win->next) {
        if (win->fs_output == out) {
            // The compositor already exited fullscreen for us when the output
            // went; we only have to stop believing otherwise.
            win->fs_output = NULL;
            win->fullscreen = false;
            win->fullscreen_dirty = true;
        }
        win->proposed = false;      // sized against an output that no longer exists

        // Float geometry is in the coordinates of the output that is going
        // away; dropping it re-centers the window on whatever output is left
        // rather than parking it off screen.
        win->float_width = 0;
        win->float_height = 0;
    }
}

void windows_apply_fullscreen(struct satori *satori) {
    for (struct window *win = satori->windows; win; win = win->next) {
        if (!win->fullscreen_dirty) continue;
        win->fullscreen_dirty = false;

        if (win->fullscreen) {
            struct output *out = win->fs_output ? win->fs_output : satori->outputs;
            if (!out) {
                win->fullscreen = false;    // nowhere to be fullscreen on
                continue;
            }
            river_window_v1_fullscreen(win->handle, out->handle);
            river_window_v1_inform_fullscreen(win->handle);
        } else {
            river_window_v1_exit_fullscreen(win->handle);
            river_window_v1_inform_not_fullscreen(win->handle);

            // Leaving fullscreen leaves dimensions and position undefined until
            // both are set again. The protocol asks for that in this same
            // sequence: clearing the flag lets windows_propose run below, and
            // node position is rendering state, which a manage sequence may
            // also touch.
            win->proposed = false;
            struct output *out = satori->outputs;
            if (win->node && out) window_place(win, out);
        }
        satori_log("window: fullscreen %s\n", win->fullscreen ? "on" : "off");
    }
}

void windows_propose(struct satori *satori) {
    struct output *out = satori->outputs;
    if (!out) return;

    // Maximized means the output minus whatever panels reserved, not the whole
    // output -- otherwise a bar sits on top of every window.
    int32_t x, y, width, height;
    output_usable_area(out, &x, &y, &width, &height);
    (void) x; (void) y;

    for (struct window *win = satori->windows; win; win = win->next) {
        // The server answers every proposal with a dimensions event, which
        // starts another sequence: re-proposing unconditionally never settles.
        if (win->proposed) continue;
        // The compositor owns a fullscreen window's size, and it is neither
        // maximized nor floating. windows_apply_fullscreen clears the flag on
        // the way out, so a window toggled while fullscreen is sized then.
        if (win->fullscreen) continue;

        if (win->floating) {
            if (win->float_width <= 0 || win->float_height <= 0) {
                window_init_float_geometry(win, out);
            }
            river_window_v1_propose_dimensions(win->handle, win->float_width, win->float_height);
            river_window_v1_inform_unmaximized(win->handle);
        } else {
            river_window_v1_propose_dimensions(win->handle, width, height);
            river_window_v1_inform_maximized(win->handle);
        }
        win->proposed = true;
        // A proposal is otherwise invisible: on a screen-sized window the
        // dimensions event that answers it looks the same as the fullscreen
        // one, so a re-proposal cannot be told apart without this.
        satori_log("window: propose %dx%d %s\n",
                win->floating ? win->float_width  : width,
                win->floating ? win->float_height : height,
                win->floating ? "floating" : "maximized");
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
        if (out) {
            window_place(win, out);
        } else {
            river_node_v1_set_position(win->node, 0, 0);
        }
        river_node_v1_place_bottom(win->node);
    }

    // Raise the focused window. Without this, stacking follows creation order
    // only: cycling moves the keyboard focus to a window that stays buried, and
    // since every window is maximized it is invisible -- keystrokes land in a
    // window you cannot see. Unconditional, because the walk above re-asserts
    // newest-on-top every render and would bury it again.
    if (satori->focused && satori->focused->node) {
        river_node_v1_place_top(satori->focused->node);
        // Logged on change only: this runs every render, and the smoke test
        // needs a signal that the raise happened at all.
        if (satori->raised != satori->focused) {
            satori->raised = satori->focused;
            satori_log("window: raised\n");
        }
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
    satori->mru = NULL;
    satori->focused = NULL;
    satori->raised = NULL;
}
