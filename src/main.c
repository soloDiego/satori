// Entry point: connect, bind the globals, run the event loop, shut down.

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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
    } else if (strcmp(interface, river_layer_shell_v1_interface.name) == 0) {
        // Binding this is what tells river we support layer surfaces. Without
        // it the compositor closes every one, so bars, launchers and wallpapers
        // silently never map.
        uint32_t v = clamp_version(version, SATORI_LAYER_SHELL_VERSION);
        satori->layer_shell = wl_registry_bind(registry, name, &river_layer_shell_v1_interface, v);
        fprintf(stderr, "bound river_layer_shell_v1 v%u\n", v);    // no events on this object
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

    // Before the first roundtrip: seat_create builds its bindings straight out
    // of the table, and the seat event can arrive in the very first dispatch.
    satori.config_path = config_default_path();
    satori.config = config_load(satori.config_path, true);
    if (!satori.config) {
        // A broken file is not fatal at startup. Falling back to the built-ins
        // leaves a session that can still open a terminal, fix the config, and
        // reload -- which beats no bindings at all.
        fprintf(stderr, "config: falling back to the built-in bindings\n");
        satori.config = config_load(NULL, true);
    }
    if (!satori.config) {
        fprintf(stderr, "could not build a key binding table\n");
        free(satori.config_path);
        wl_display_disconnect(display);
        return 1;
    }
    fprintf(stderr, "config: %zu bindings from %s\n", satori.config->len,
            satori.config_path ? satori.config_path : "the built-in table");

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &satori);

    // SIGINT/SIGTERM are blocked and read off a fd instead, so a signal can
    // never land in the middle of a sequence. SIGHUP rides along as the
    // scriptable half of a config reload.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC);

    if (sigfd == -1) {
        fprintf(stderr, "signalfd failed: %s\n", strerror(errno));
        config_destroy(satori.config);
        free(satori.config_path);
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

        // Another WM holds the slot. unavailable is the first and only event
        // that object ever gets, so there is nothing left to wait for -- without
        // this we poll forever on a display that will never speak again. The
        // check sits after prepare_read so it catches the event whether it was
        // dispatched above or at the bottom of the previous iteration; the
        // pending read has to be cancelled before leaving.
        if (satori.got_unavailable) {
            wl_display_cancel_read(display);
            break;
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
            if (read(sigfd, &si, sizeof si) != sizeof si) continue;

            if (si.ssi_signo == SIGHUP) {
                // Unlike a keypress, a signal does not bring a manage sequence
                // with it, so ask for one -- otherwise the reload sits pending
                // until something else happens to move a window.
                satori.reload_pending = true;
                if (satori.wm) river_window_manager_v1_manage_dirty(satori.wm);
            } else {
                should_exit = true;
            }
        }
    }

    wl_display_roundtrip(display);
    if (!satori.wm) {
        fprintf(stderr, "could not bind to global river_window_manager_v1\n");
        config_destroy(satori.config);
        free(satori.config_path);
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

    bindings_destroy_all(&satori);      // the proxies borrow into satori.config
    seats_destroy_all(&satori);
    windows_destroy_all(&satori);
    outputs_destroy_all(&satori);
    config_destroy(satori.config);
    free(satori.config_path);

    // After the per-output and per-seat layer objects, which the walks above destroy.
    if (satori.layer_shell) river_layer_shell_v1_destroy(satori.layer_shell);
    if (satori.xkb) river_xkb_bindings_v1_destroy(satori.xkb);
    river_window_manager_v1_destroy(satori.wm);
    wl_registry_destroy(registry);
    close(sigfd);
    wl_display_disconnect(display);
    return 0;
}
