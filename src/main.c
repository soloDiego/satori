// Entry point: connect, bind the globals, run the event loop, shut down.

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <wayland-client.h>

#include "satori.h"

static uint32_t clamp_version(uint32_t advertised, uint32_t supported) {
    return advertised < supported ? advertised : supported;
}

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct satori *satori = data;

    if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
        uint32_t v = clamp_version(version, SATORI_WM_VERSION);
        satori->wm = wl_registry_bind(registry, name, &river_window_manager_v1_interface, v);
        satori->wm_version = v;
        // The listener goes on immediately: unavailable can be the first and
        // only event on this object, so a roundtrip first would miss it.
        river_window_manager_v1_add_listener(satori->wm, &wm_listener, satori);
        fprintf(stderr, "bound river_window_manager_v1 v%u\n", v);
    } else if (strcmp(interface, river_xkb_bindings_v1_interface.name) == 0) {
        uint32_t v = clamp_version(version, SATORI_XKB_BINDINGS_VERSION);
        satori->xkb = wl_registry_bind(registry, name, &river_xkb_bindings_v1_interface, v);
        fprintf(stderr, "bound river_xkb_bindings_v1 v%u\n", v);   // no events on this object
    }
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
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

    // SIGINT/SIGTERM are blocked and read off a fd instead, so a signal can
    // never land in the middle of a sequence.
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
            { .fd = wl_fd,  .events = POLLIN },
            { .fd = sigfd,  .events = POLLIN },
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

    // Active WM: hand the session back before disconnecting. If we never became
    // active, stop would be a protocol error.
    if (!satori.got_unavailable) {
        river_window_manager_v1_stop(satori.wm);
        wl_display_flush(display);
        while (!satori.finished_received && wl_display_dispatch(display) != -1) {
        }
    }

    bindings_destroy_all(&satori);
    seats_destroy_all(&satori);
    windows_destroy_all(&satori);
    outputs_destroy_all(&satori);

    if (satori.xkb) river_xkb_bindings_v1_destroy(satori.xkb);
    river_window_manager_v1_destroy(satori.wm);
    wl_registry_destroy(registry);
    close(sigfd);
    wl_display_disconnect(display);
    return 0;
}
