#ifndef SATORI_H
#define SATORI_H

#include <stdbool.h>
#include <stdint.h>

#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

// Protocol versions we know how to speak. The actual bind clamps to whatever
// the compositor advertises.
#define SATORI_WM_VERSION           4
#define SATORI_XKB_BINDINGS_VERSION 3

// Command run by the spawn action. Becomes a config key once scfg parsing lands.
#define SATORI_TERMINAL "foot"

struct output;
struct window;
struct seat;
struct binding;

struct satori {
    struct river_window_manager_v1  *wm;
    struct river_xkb_bindings_v1    *xkb;
    bool got_unavailable;
    bool finished_received;

    struct output   *outputs;
    struct window   *windows;   // newest first
    struct seat     *seats;
    struct binding  *bindings;

    struct window   *focused;       // NULL = nothing focused
    bool            focus_dirty;    // focus must be re-applied in the next manage sequence
};

struct output {
    struct river_output_v1  *handle;
    struct satori           *satori;
    int32_t x, y;
    int32_t width, height;
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
    struct window           *next;
};

struct seat {
    struct river_seat_v1    *handle;
    struct satori           *satori;
    struct seat             *next;
};

// An action never touches window management state directly: bindings fire
// outside a manage sequence. Actions record intent on struct satori, the next
// manage sequence applies it.
typedef void (*satori_action)(struct satori *satori);

struct keybind {
    uint32_t        keysym;     // xkbcommon keysym, see XKB_KEY_* in <xkbcommon/xkbcommon-keysyms.h>
    uint32_t        modifiers;  // river_seat_v1.modifiers bitfield
    satori_action   action;
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
void outputs_destroy_all(struct satori *satori);

// window.c
void window_create(struct satori *satori, struct river_window_v1 *handle);
void window_focus(struct satori *satori, struct window *win);
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
