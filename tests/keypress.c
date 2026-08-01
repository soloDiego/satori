// Inject a key chord into a Wayland compositor via zwp_virtual_keyboard_v1.
//
// Test-only tool. The headless backend has no keyboard, so this is the only way
// to prove a real key press reaches a satori binding.
//
//   keypress super+return
//
// Keys are pressed left to right and released in reverse, like a real chord.
// Names come from the table below; a bare number is an evdev keycode.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define MAX_KEYS 8

// evdev keycodes, linux/input-event-codes.h. Not the xkb keycode (which is
// this + 8): wl_keyboard and this protocol both carry evdev codes.
static const struct { const char *name; uint32_t code; } key_names[] = {
    { "super", 125 }, { "alt", 56 }, { "ctrl", 29 }, { "shift", 42 },
    { "return", 28 }, { "escape", 1 }, { "space", 57 }, { "tab", 15 },
    { "a", 30 }, { "b", 48 }, { "c", 46 }, { "d", 32 }, { "e", 18 },
    { "f", 33 }, { "g", 34 }, { "h", 35 }, { "i", 23 }, { "j", 36 },
    { "k", 37 }, { "l", 38 }, { "m", 50 }, { "n", 49 }, { "o", 24 },
    { "p", 25 }, { "q", 16 }, { "r", 19 }, { "s", 31 }, { "t", 20 },
    { "u", 22 }, { "v", 47 }, { "w", 17 }, { "x", 45 }, { "y", 21 },
    { "z", 44 },
};

struct state {
    struct wl_seat *seat;
    struct zwp_virtual_keyboard_manager_v1 *manager;
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    (void) version;
    struct state *state = data;

    if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        state->manager = wl_registry_bind(registry, name,
                &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static bool parse_key(const char *name, uint32_t *out) {
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++) {
        if (strcmp(name, key_names[i].name) == 0) {
            *out = key_names[i].code;
            return true;
        }
    }
    char *end;
    unsigned long code = strtoul(name, &end, 10);
    if (*name && !*end) {
        *out = (uint32_t) code;
        return true;
    }
    return false;
}

// The compositor mmaps this, so it has to be a real file description. A memfd
// avoids leaving anything on disk.
static int keymap_fd(size_t *size_out) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) return -1;

    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        xkb_context_unref(ctx);
        return -1;
    }
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    if (!text) return -1;

    size_t size = strlen(text) + 1;
    int fd = memfd_create("satori-keymap", MFD_CLOEXEC);
    if (fd >= 0 && write(fd, text, size) != (ssize_t) size) {
        close(fd);
        fd = -1;
    }
    free(text);

    *size_out = size;
    return fd;
}

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Let the compositor process each event: satori answers a binding with a whole
// manage sequence, and the chord must not outrun it.
static void settle(struct wl_display *display) {
    wl_display_roundtrip(display);
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 40 * 1000 * 1000 };
    nanosleep(&pause, NULL);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <chord>   e.g. super+return\n", argv[0]);
        return 2;
    }

    uint32_t keys[MAX_KEYS];
    int nkeys = 0;
    char chord[128];
    snprintf(chord, sizeof chord, "%s", argv[1]);

    for (char *tok = strtok(chord, "+"); tok; tok = strtok(NULL, "+")) {
        if (nkeys == MAX_KEYS) {
            fprintf(stderr, "keypress: too many keys in chord\n");
            return 2;
        }
        if (!parse_key(tok, &keys[nkeys++])) {
            fprintf(stderr, "keypress: unknown key '%s'\n", tok);
            return 2;
        }
    }

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "keypress: could not connect to wayland display\n");
        return 1;
    }

    struct state state = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(display);

    if (!state.seat || !state.manager) {
        fprintf(stderr, "keypress: compositor lacks %s\n",
                state.seat ? "zwp_virtual_keyboard_manager_v1" : "wl_seat");
        return 1;
    }

    struct zwp_virtual_keyboard_v1 *keyboard =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(state.manager, state.seat);

    size_t size = 0;
    int fd = keymap_fd(&size);
    if (fd < 0) {
        fprintf(stderr, "keypress: could not build a keymap\n");
        return 1;
    }
    // Required before any key request, or the compositor raises no_keymap.
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
    close(fd);
    settle(display);

    for (int i = 0; i < nkeys; i++) {
        zwp_virtual_keyboard_v1_key(keyboard, now_ms(), keys[i], WL_KEYBOARD_KEY_STATE_PRESSED);
        settle(display);
    }
    for (int i = nkeys - 1; i >= 0; i--) {
        zwp_virtual_keyboard_v1_key(keyboard, now_ms(), keys[i], WL_KEYBOARD_KEY_STATE_RELEASED);
        settle(display);
    }

    zwp_virtual_keyboard_v1_destroy(keyboard);
    zwp_virtual_keyboard_manager_v1_destroy(state.manager);
    wl_seat_destroy(state.seat);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
