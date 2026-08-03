#ifndef SATORI_H
#define SATORI_H

#include <stdbool.h>
#include <stdint.h>

#include "river-layer-shell-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

// Protocol versions we know how to speak. The actual bind clamps to whatever
// the compositor advertises.
#define SATORI_WM_VERSION           4
#define SATORI_XKB_BINDINGS_VERSION 3
#define SATORI_LAYER_SHELL_VERSION  1

struct output;
struct window;
struct seat;
struct binding;

struct satori {
    struct river_window_manager_v1  *wm;
    struct river_xkb_bindings_v1    *xkb;
    struct river_layer_shell_v1     *layer_shell;
    uint32_t wm_version;        // bound version; some requests are version gated
    bool got_unavailable;
    bool finished_received;

    struct output   *outputs;
    struct window   *windows;   // newest first
    struct seat     *seats;
    struct binding  *bindings;

    struct window   *focused;       // NULL = nothing focused
    bool            focus_dirty;    // focus must be re-applied in the next manage sequence
    // Last window windows_render raised, for logging only -- the raise itself is
    // unconditional. Never dereferenced, but cleared when the window closes so
    // it cannot compare equal to a later window at the same address.
    struct window   *raised;
    bool            default_output_dirty;   // the layer shell default output needs re-setting
};

struct output {
    struct river_output_v1              *handle;
    struct river_layer_shell_output_v1  *layer;     // NULL if the layer shell is unavailable
    struct satori                       *satori;
    int32_t x, y;
    int32_t width, height;

    // What is left of the output after layer surfaces reserve their exclusive
    // zones -- where a maximized window goes. Only valid once has_area is set;
    // output_usable_area falls back to the full output until then.
    int32_t area_x, area_y;
    int32_t area_width, area_height;
    bool has_area;

    struct output           *next;
};

struct window {
    struct river_window_v1  *handle;
    struct river_node_v1    *node;
    struct satori           *satori;
    char                    *app_id;
    char                    *title;
    int32_t width, height;
    bool proposed;          // dimensions proposed; the server owes us a dimensions event
    bool close_pending;     // close requested; sent in the next manage sequence
    bool fullscreen;        // desired state, not necessarily the applied one
    bool fullscreen_dirty;  // fullscreen differs from what the server has; apply it
    struct output           *fs_output;     // output to fullscreen on; NULL = the first
    struct window           *next;
};

struct seat {
    struct river_seat_v1                *handle;
    struct river_layer_shell_seat_v1    *layer;     // NULL if the layer shell is unavailable
    struct satori                       *satori;
    // A layer surface holds this seat's keyboard focus. Our focus requests are
    // either ignored (exclusive) or would steal it (non-exclusive), so they
    // wait for the focus_none that follows.
    bool layer_focus;
    struct seat             *next;
};

// Lets one action serve many bindings -- the spawn action is the whole reason,
// and it is what a config file will fill in per binding.
union satori_arg {
    const char  *cmd;
    uint32_t    u;
};

// An action never touches window management state directly: bindings fire
// outside a manage sequence. Actions record intent on struct satori, the next
// manage sequence applies it.
typedef void (*satori_action)(struct satori *satori, union satori_arg arg);

struct keybind {
    uint32_t            keysym;     // xkbcommon keysym, see XKB_KEY_* in <xkbcommon/xkbcommon-keysyms.h>
    uint32_t            modifiers;  // river_seat_v1.modifiers bitfield
    satori_action       action;
    union satori_arg    arg;
};

struct binding {
    struct river_xkb_binding_v1 *handle;
    struct satori               *satori;
    const struct keybind        *keybind;
    bool enabled;               // enable is window management state, so it waits for a manage sequence
    struct binding              *next;
};

// wm.c -- the manage/render sequence driver.
extern const struct river_window_manager_v1_listener wm_listener;

// output.c
void output_create(struct satori *satori, struct river_output_v1 *handle);
struct output *output_from_handle(struct satori *satori, struct river_output_v1 *handle);
void output_usable_area(const struct output *out, int32_t *x, int32_t *y,
        int32_t *width, int32_t *height);
void outputs_destroy_all(struct satori *satori);

// layer.c -- wlr-layer-shell support (panels, bars, launchers, wallpapers).
void layer_output_create(struct satori *satori, struct output *out);
void layer_output_destroy(struct output *out);
void layer_seat_create(struct satori *satori, struct seat *seat);
void layer_seat_destroy(struct seat *seat);
void layer_apply_default_output(struct satori *satori);     // manage sequence

// window.c
void window_create(struct satori *satori, struct river_window_v1 *handle);
void window_focus(struct satori *satori, struct window *win);
void windows_invalidate_layout(struct satori *satori);
void windows_forget_output(struct satori *satori, struct output *out);
void windows_apply_fullscreen(struct satori *satori);   // manage sequence
void windows_propose(struct satori *satori);        // manage sequence
void windows_apply_closes(struct satori *satori);   // manage sequence
void windows_render(struct satori *satori);         // render sequence
void windows_destroy_all(struct satori *satori);

// input.c -- seats, key bindings, actions.
void seat_create(struct satori *satori, struct river_seat_v1 *handle);
void seats_apply_focus(struct satori *satori);      // manage sequence
void seats_destroy_all(struct satori *satori);
void bindings_enable_pending(struct satori *satori);  // manage sequence
void bindings_destroy_all(struct satori *satori);

#endif // SATORI_H
