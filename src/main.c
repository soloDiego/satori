#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wayland-client.h>

#include "river-window-management-v1-client-protocol.h"

#define RIVER_WINDOW_MANAGER_V1_VERSION 4

static struct river_window_manager_v1 *wm = NULL;

static void unavailable(void *data, struct river_window_manager_v1 *rwm) {
    (void)data;
    (void)rwm;
    fprintf(stderr, "wm: unavailable\n");

}

static void finished(void *data, struct river_window_manager_v1 *rwm) {
    (void)data;
    (void)rwm;
    fprintf(stderr, "wm: finished\n");
}

static void manage_start(void *data, struct river_window_manager_v1 *rwm) {
    (void)data;
    fprintf(stderr, "wm: manage start\n");
    river_window_manager_v1_manage_finish(rwm);
}

static void render_start(void *data, struct river_window_manager_v1 *rwm) {
    (void)data;
    fprintf(stderr, "wm: render start\n");
    river_window_manager_v1_render_finish(rwm);
}

static void session_locked(void *data, struct river_window_manager_v1 *rwm) {
    (void)data;
    (void)rwm;
    fprintf(stderr, "wm: session locked\n");
}

static void session_unlocked(void *data, struct river_window_manager_v1 *rwm) {
    (void)data;
    (void)rwm;
    fprintf(stderr, "wm: session unlocked\n");
}

static void window(void *data, 
        struct river_window_manager_v1 *rwm, 
        struct river_window_v1 *id) {
    (void)data;
    (void)rwm;
    (void)id;
    fprintf(stderr, "wm: window\n");
}

static void output(void *data, 
        struct river_window_manager_v1 *rwm, 
        struct river_output_v1 *id) {
    (void) data;
    (void) rwm;
    (void) id;
    fprintf(stderr, "wm: output\n");
}

static void seat(void *data, 
        struct river_window_manager_v1 *rwm, 
        struct river_seat_v1 *id) {
    (void) data;
    (void) rwm;
    (void) id;
    fprintf(stderr, "wm: seat\n");
}

static const struct river_window_manager_v1_listener wm_listener = {
    .unavailable  = unavailable,
    .finished     = finished,
    .manage_start = manage_start,
    .render_start = render_start,
    .session_locked = session_locked,
    .session_unlocked = session_unlocked,
    .window = window,
    .output = output,
    .seat = seat
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface,
        uint32_t version) {
    (void) data;

    fprintf(stderr, "global: name:%u, interface:%s, version%u\n",
            name, interface, version);

    if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
        uint32_t v = version < RIVER_WINDOW_MANAGER_V1_VERSION ? version : RIVER_WINDOW_MANAGER_V1_VERSION;
        wm = wl_registry_bind(registry, name,
                &river_window_manager_v1_interface, v);
        fprintf(stderr, "bound river_window_manager_v1 v%u\n", v);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;

    fprintf(stderr, "global remove: name:%u", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);

    if (!display) {
        fprintf(stderr, "could not connect to wayland display "
                "(is WAYLAND_DISPLAY set and a compositor running?)\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &wm);
    wl_display_roundtrip(display);
    river_window_manager_v1_add_listener(wm, &wm_listener, NULL);
    wl_display_roundtrip(display);

    if (!wm) {
        fprintf(stderr, "could not bind to global river_window_manager_v1\n");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 1;
    }


    river_window_manager_v1_destroy(wm);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
