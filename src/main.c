#define _POSIX_C_SOURCE 200809L
#define RIVER_WINDOW_MANAGER_V1_VERSION 4

#include <errno.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "river-window-management-v1-client-protocol.h"


struct satori {
    struct river_window_manager_v1 *wm;
    bool got_unavailable;
    bool finished_received;
};


static void unavailable(void *data, struct river_window_manager_v1 *rwm) {
    struct satori *state = data;
    (void)rwm;
    fprintf(stderr, "wm: unavailable\n");
    state->got_unavailable = true;
}

static void finished(void *data, struct river_window_manager_v1 *rwm) {
    struct satori *state = data;
    (void)rwm;
    state->finished_received = true;
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
    struct satori *state = data;
    if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
        uint32_t v = version < RIVER_WINDOW_MANAGER_V1_VERSION ? version : RIVER_WINDOW_MANAGER_V1_VERSION;
        state->wm = wl_registry_bind(registry, name, &river_window_manager_v1_interface, v);
        river_window_manager_v1_add_listener(state->wm, &wm_listener, state);
        fprintf(stderr, "bound river_window_manager_v1 v%u\n", v);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
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

    struct satori satori = {0};

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &satori);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC);

    if (sigfd == -1) {
        fprintf(stderr, "signalfd failed: %s\n", strerror(errno));
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 1;
    }

    int wl_fd = wl_display_get_fd(display);
    bool should_exit = false;

    while (!should_exit) {
        while (wl_display_prepare_read(display) != 0) {
            wl_display_dispatch_pending(display);
        }

        wl_display_flush(display);

        struct pollfd pfds[2] = {
            { .fd = wl_fd, .events = POLLIN },
            { .fd = sigfd, .events = POLLIN },
        };
        int ret = poll(pfds, 2, -1);

        if (ret < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) continue;
            fprintf(stderr, "poll %s\n", strerror(errno));
            break;
        }

        if (pfds[0].revents & POLLIN) {
            wl_display_read_events(display);
            wl_display_dispatch_pending(display);
        } else {
            wl_display_cancel_read(display);
        }

        if (pfds[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            read(sigfd, &si, sizeof si);
            should_exit = true;
        }
    }

    wl_display_roundtrip(display);
    if (!satori.wm) {
        fprintf(stderr, "could not bind to global river_window_manager_v1\n");
        wl_registry_destroy(registry);
        close(sigfd);
        wl_display_disconnect(display);
        return 1;
    }

    if (!satori.got_unavailable) {
        river_window_manager_v1_stop(satori.wm);
        wl_display_flush(display);
        while (!satori.finished_received && wl_display_dispatch(display) != -1) {
        }
    }

    river_window_manager_v1_destroy(satori.wm);
    wl_registry_destroy(registry);
    close(sigfd);
    wl_display_disconnect(display);
    return 0;
}
